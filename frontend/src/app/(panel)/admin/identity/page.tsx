"use client";

import { Badge } from "@/components/catalyst/badge";
import { Button } from "@/components/catalyst/button";
import { DescriptionDetails, DescriptionList, DescriptionTerm } from "@/components/catalyst/description-list";
import { Field, Label } from "@/components/catalyst/fieldset";
import { Select } from "@/components/catalyst/select";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { TextLink } from "@/components/catalyst/text";
import { IdentityBadge, RevocationBadge, SessionBadge } from "@/components/badges";
import { ConfirmDialog } from "@/components/ConfirmDialog";
import { OutcomeCallout, type Outcome } from "@/components/Outcome";
import { Callout, ErrorCallout, Loading, Mono, PageHeading, Section } from "@/components/ui";
import { api, describeError } from "@/lib/api";
import { enrollAndFollow } from "@/lib/enroll";
import { fmtDateTime, shortHex } from "@/lib/format";
import { useApi, useStream } from "@/lib/hooks";
import { REVOCATION_REASONS, type Certificate, type DeviceDetail, type DeviceSummary, type RevocationReason } from "@/lib/types";
import { ArrowPathIcon, LockClosedIcon, LockOpenIcon, NoSymbolIcon, TrashIcon } from "@heroicons/react/16/solid";
import { useRouter, useSearchParams } from "next/navigation";
import { Suspense, useCallback, useState } from "react";

const REASON_LABEL: Record<RevocationReason, string> = {
  certificateHold: "certificateHold — suspensión reversible",
  keyCompromise: "keyCompromise — clave comprometida",
  cessationOfOperation: "cessationOfOperation — fuera de servicio",
  affiliationChanged: "affiliationChanged — cambio de propietario",
  unspecified: "unspecified — sin especificar",
};

function IdentityView({ name }: { name: string }) {
  const router = useRouter();
  const enc = encodeURIComponent(name);
  const detail = useApi<DeviceDetail>(`/api/devices/${enc}`);
  const certs = useApi<Certificate[]>(`/api/devices/${enc}/certificates`);
  const [reason, setReason] = useState<RevocationReason>("certificateHold");
  const [confirm, setConfirm] = useState<null | { kind: "revoke" } | { kind: "cert"; cert: Certificate }>(null);
  const [busy, setBusy] = useState(false);
  const [outcome, setOutcome] = useState<Outcome | null>(null);

  const reloadDetail = detail.reload;
  const reloadCerts = certs.reload;
  const reload = useCallback(() => {
    reloadDetail();
    reloadCerts();
  }, [reloadDetail, reloadCerts]);
  useStream({ channels: ["device", "enrollment"], devices: [name] }, reload);

  async function run(label: string, state: string, fn: () => Promise<Outcome | void>) {
    setBusy(true);
    try {
      setOutcome((await fn()) ?? { state, tone: "success", title: label });
    } catch (e) {
      setOutcome({ state: "error", tone: "error", title: "La operación falló", detail: describeError(e) });
    } finally {
      setBusy(false);
      setConfirm(null);
      reload();
    }
  }

  const revoke = () =>
    run(`Certificado de ${name} revocado (${reason})`, reason === "certificateHold" ? "suspended" : "revoked", async () => {
      await api(`/api/devices/${enc}/revoke`, { method: "POST", body: { reason } });
    });

  const unhold = () =>
    run(`Suspensión de ${name} levantada`, "unheld", async () => {
      await api(`/api/devices/${enc}/revoke`, { method: "DELETE" });
    });

  const rotate = () =>
    run("", "rotated", async () => {
      const r = await enrollAndFollow(`/api/devices/${enc}/rotate`);
      switch (r.kind) {
        case "issued":
          return {
            state: "rotated",
            tone: "success",
            title: `Clave de ${name} rotada`,
            detail: "Nuevo certificado emitido; el anterior queda en la CRL como superseded y puede borrarse abajo.",
          };
        case "pending":
          return { state: "pending", tone: "warning", title: "Rotación sin resolver", detail: `Solicitud #${r.enrollment.request_id} en ${r.enrollment.status}.` };
        case "rejected":
          return { state: "rejected", tone: "error", title: "Rotación rechazada", detail: `${r.enrollment.error_code}: ${r.enrollment.error_detail ?? ""}` };
        case "conflict":
          return { state: "conflict", tone: "warning", title: "No se puede rotar ahora", detail: r.message };
        case "error":
          return { state: "error", tone: "error", title: "La rotación falló", detail: describeError(r.error) };
      }
    });

  const deleteCert = (c: Certificate) =>
    run("", "cert-deleted", async () => {
      const r = await api<{ device_deleted: boolean }>(`/api/devices/${enc}/certificates/${c.serial}`, { method: "DELETE" });
      if (r.device_deleted) {
        router.replace("/admin/devices/");
        return { state: "device-deleted", tone: "warning", title: `Certificado actual borrado: ${name} eliminado` };
      }
      return { state: "cert-deleted", tone: "success", title: `Certificado ${shortHex(c.serial, 8)} borrado` };
    });

  const d = detail.data;
  const identity = d?.identity;
  const suspended = identity === "SUSPENDED";
  const revoked = identity === "REVOKED";

  return (
    <>
      <PageHeading
        title={
          <span className="flex flex-wrap items-center gap-3">
            Identidad de {name} {d && <IdentityBadge value={d.identity} />} {d && <SessionBadge value={d.session} />}
          </span>
        }
        description={
          <>
            Revocación, suspensión y rotación del certificado. <TextLink href={`/device/?name=${enc}`}>Ver en el panel de visualización</TextLink>.
          </>
        }
        testId="page-admin-identity"
      />
      <div className="mt-6">
        <OutcomeCallout outcome={outcome} testId="identity-result" />
        <ErrorCallout error={detail.error} />
      </div>
      {!d ? (
        !detail.error && <Loading />
      ) : (
        <>
          <div className="mt-6 grid gap-8 lg:grid-cols-2">
            <div className="rounded-lg p-5 ring-1 ring-zinc-950/10">
              <h2 className="font-semibold text-zinc-950">Revocar o suspender</h2>
              <p className="mt-1 text-sm text-zinc-600">
                POST /api/devices/{"{name}"}/revoke. El controlador detiene el proceso; solo certificateHold puede levantarse.
              </p>
              <Field className="mt-4">
                <Label>Motivo</Label>
                <Select value={reason} onChange={(e) => setReason(e.target.value as RevocationReason)} data-testid="identity-reason">
                  {REVOCATION_REASONS.map((r) => (
                    <option key={r} value={r}>
                      {REASON_LABEL[r]}
                    </option>
                  ))}
                </Select>
              </Field>
              <div className="mt-4 flex flex-wrap gap-2">
                <Button
                  color={reason === "certificateHold" ? "amber" : "red"}
                  disabled={busy || revoked || suspended}
                  onClick={() => (reason === "certificateHold" ? void revoke() : setConfirm({ kind: "revoke" }))}
                  data-testid="identity-revoke"
                >
                  {reason === "certificateHold" ? <LockClosedIcon /> : <NoSymbolIcon />}
                  {reason === "certificateHold" ? "Suspender" : "Revocar"}
                </Button>
                <Button outline disabled={busy || !suspended} onClick={() => void unhold()} data-testid="identity-unhold">
                  <LockOpenIcon />
                  Levantar suspensión
                </Button>
              </div>
              {revoked && (
                <div className="mt-4">
                  <Callout tone="warning">Revocación permanente: para volver a usar el nombre, borra el dispositivo y dalo de alta de nuevo.</Callout>
                </div>
              )}
            </div>
            <div className="rounded-lg p-5 ring-1 ring-zinc-950/10">
              <h2 className="font-semibold text-zinc-950">Rotar la clave</h2>
              <p className="mt-1 text-sm text-zinc-600">
                POST /api/devices/{"{name}"}/rotate. El proceso genera una clave nueva y la CA firma su CSR; el certificado anterior pasa a la CRL como superseded.
              </p>
              <Button className="mt-4" color="amber" disabled={busy || revoked || suspended} onClick={() => void rotate()} data-testid="identity-rotate">
                <ArrowPathIcon />
                {busy ? "Trabajando…" : "Rotar"}
              </Button>
              {d.certificate && (
                <DescriptionList className="mt-4">
                  <DescriptionTerm>Serie actual</DescriptionTerm>
                  <DescriptionDetails>
                    <Mono>
                      <span data-testid="identity-serial">{d.certificate.serial}</span>
                    </Mono>
                  </DescriptionDetails>
                  <DescriptionTerm>Válido hasta</DescriptionTerm>
                  <DescriptionDetails>{fmtDateTime(d.certificate.not_after)}</DescriptionDetails>
                </DescriptionList>
              )}
            </div>
          </div>

          <Section title="Certificados" testId="section-admin-certificates">
            <Table dense data-testid="admin-certificates-table">
              <TableHead>
                <TableRow>
                  <TableHeader>Serie</TableHeader>
                  <TableHeader>Emitido</TableHeader>
                  <TableHeader>Estado</TableHeader>
                  <TableHeader>Revocación</TableHeader>
                  <TableHeader className="text-right">Acciones</TableHeader>
                </TableRow>
              </TableHead>
              <TableBody>
                {(certs.data ?? []).map((c) => (
                  <TableRow key={c.cert_id} data-testid={`admin-cert-row-${c.serial}`} data-current={String(c.current)}>
                    <TableCell>
                      <Mono>{c.serial}</Mono>
                    </TableCell>
                    <TableCell className="tabular-nums">{fmtDateTime(c.issued_at)}</TableCell>
                    <TableCell>{c.current ? <Badge color="sky">actual</Badge> : <Badge color="zinc">anterior</Badge>}</TableCell>
                    <TableCell>
                      <RevocationBadge reason={c.revocation_reason} />
                    </TableCell>
                    <TableCell className="text-right">
                      <Button plain disabled={busy} onClick={() => setConfirm({ kind: "cert", cert: c })} data-testid={`cert-delete-${c.serial}`}>
                        <TrashIcon className="fill-red-500!" />
                        Borrar
                      </Button>
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </Section>
        </>
      )}

      <ConfirmDialog
        open={confirm?.kind === "revoke"}
        title={`Revocar ${name} (${reason})`}
        confirmLabel="Revocar definitivamente"
        busy={busy}
        onClose={() => setConfirm(null)}
        onConfirm={() => void revoke()}
        testId="dialog-revoke"
      >
        {reason} es una revocación permanente: el número de serie entra en la CRL, el controlador detiene el dispositivo y no podrá volver a conectarse ni
        rotar su clave.
      </ConfirmDialog>
      <ConfirmDialog
        open={confirm?.kind === "cert"}
        title="Borrar certificado"
        confirmLabel={confirm?.kind === "cert" && confirm.cert.current ? "Borrar certificado y dispositivo" : "Borrar certificado"}
        busy={busy}
        onClose={() => setConfirm(null)}
        onConfirm={() => confirm?.kind === "cert" && void deleteCert(confirm.cert)}
        testId="dialog-delete-cert"
      >
        {confirm?.kind === "cert" && confirm.cert.current
          ? `Es el certificado actual de ${name}: borrarlo elimina también el dispositivo.`
          : "Se borra un certificado anterior; si estaba revocado, su número de serie sigue en la CRL."}
      </ConfirmDialog>
    </>
  );
}

function DevicePicker({ current }: { current: string | null }) {
  const router = useRouter();
  const devices = useApi<DeviceSummary[]>("/api/devices");
  return (
    <Field className="mb-6 max-w-sm">
      <Label>Dispositivo</Label>
      <Select
        value={current ?? ""}
        onChange={(e) => router.replace(`/admin/identity/?name=${encodeURIComponent(e.target.value)}`)}
        data-testid="identity-device"
      >
        <option value="" disabled>
          Elige un dispositivo…
        </option>
        {(devices.data ?? []).map((d) => (
          <option key={d.device} value={d.device}>
            {d.device} ({d.identity})
          </option>
        ))}
        {current && !(devices.data ?? []).some((d) => d.device === current) && <option value={current}>{current}</option>}
      </Select>
    </Field>
  );
}

function IdentityFromQuery() {
  const name = useSearchParams().get("name");
  return (
    <>
      <DevicePicker current={name} />
      {name ? <IdentityView key={name} name={name} /> : <Callout tone="info">Elige un dispositivo para gestionar su certificado.</Callout>}
    </>
  );
}

export default function AdminIdentityPage() {
  return (
    <Suspense fallback={<Loading />}>
      <IdentityFromQuery />
    </Suspense>
  );
}
