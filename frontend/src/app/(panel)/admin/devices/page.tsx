"use client";

import { Button } from "@/components/catalyst/button";
import { Dialog, DialogActions, DialogBody, DialogDescription, DialogTitle } from "@/components/catalyst/dialog";
import { Description, Field, FieldGroup, Fieldset, Label } from "@/components/catalyst/fieldset";
import { Input } from "@/components/catalyst/input";
import { Link } from "@/components/catalyst/link";
import { Select } from "@/components/catalyst/select";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { IdentityBadge, SessionBadge } from "@/components/badges";
import { ConfirmDialog } from "@/components/ConfirmDialog";
import { OutcomeCallout, type Outcome } from "@/components/Outcome";
import { ErrorCallout, Loading, Mono, PageHeading } from "@/components/ui";
import { api, describeError } from "@/lib/api";
import { enrollAndFollow } from "@/lib/enroll";
import { useApi, useStream } from "@/lib/hooks";
import type { DeviceSummary, DeviceType } from "@/lib/types";
import { EyeIcon, FingerPrintIcon, PlusIcon, TrashIcon } from "@heroicons/react/16/solid";
import { useCallback, useState, type FormEvent } from "react";

const NAME_RE = /^[a-z][a-z0-9_.-]{1,63}$/;

function EnrollDialog({ open, onClose, types, onDone }: { open: boolean; onClose: () => void; types: DeviceType[]; onDone: (o: Outcome) => void }) {
  const [name, setName] = useState("");
  const [type, setType] = useState("");
  const [busy, setBusy] = useState(false);
  const valid = NAME_RE.test(name);
  const chosen = type || types[0]?.type_id || "";

  async function submit(e: FormEvent) {
    e.preventDefault();
    setBusy(true);
    const r = await enrollAndFollow("/api/devices", { name, type: chosen }, () =>
      onDone({ state: "pending", tone: "info", title: `Alta de ${name} en curso`, detail: "El controlador arranca el proceso y la CA firma su CSR…" }),
    );
    setBusy(false);
    switch (r.kind) {
      case "issued":
        onDone({
          state: "issued",
          tone: "success",
          title: `${name} dado de alta`,
          detail: (
            <>
              Certificado emitido (solicitud #{r.enrollment.request_id}){r.serial && <> · serie <Mono>{r.serial}</Mono></>}.
            </>
          ),
        });
        setName("");
        onClose();
        return;
      case "pending":
        onDone({ state: "pending", tone: "warning", title: `Alta de ${name} sin resolver`, detail: `La solicitud #${r.enrollment.request_id} sigue en ${r.enrollment.status}.` });
        onClose();
        return;
      case "rejected":
        onDone({
          state: "rejected",
          tone: "error",
          title: `Alta de ${name} rechazada`,
          detail: `${r.enrollment.error_code ?? r.enrollment.status}: ${r.enrollment.error_detail ?? ""}`,
        });
        onClose();
        return;
      case "conflict":
        onDone({ state: "conflict", tone: "warning", title: `«${name}» ya existe o tiene un alta en curso`, detail: r.message });
        onClose();
        return;
      case "error":
        onDone({ state: "error", tone: "error", title: "No se pudo dar de alta", detail: describeError(r.error) });
        onClose();
    }
  }

  return (
    <Dialog open={open} onClose={() => !busy && onClose()}>
      <form onSubmit={submit} data-testid="dialog-enroll">
        <DialogTitle>Dar de alta un dispositivo</DialogTitle>
        <DialogDescription>
          El servidor crea la solicitud, el controlador arranca el proceso con una clave nueva y la CA firma su CSR. La fila del dispositivo aparece al emitirse el
          certificado.
        </DialogDescription>
        <DialogBody>
          <Fieldset>
            <FieldGroup>
              <Field>
                <Label>Nombre</Label>
                <Input
                  value={name}
                  onChange={(e) => setName(e.target.value.trim())}
                  placeholder="sensor-b"
                  invalid={name !== "" && !valid}
                  autoFocus
                  data-testid="enroll-name"
                />
                <Description>Minúsculas, dígitos y . _ - ; empieza por letra (2 a 64 caracteres).</Description>
              </Field>
              <Field>
                <Label>Tipo</Label>
                <Select value={chosen} onChange={(e) => setType(e.target.value)} data-testid="enroll-type">
                  {types.map((t) => (
                    <option key={t.type_id} value={t.type_id}>
                      {t.type_id} — {t.description}
                    </option>
                  ))}
                </Select>
              </Field>
            </FieldGroup>
          </Fieldset>
        </DialogBody>
        <DialogActions>
          <Button plain onClick={onClose} disabled={busy}>
            Cancelar
          </Button>
          <Button type="submit" color="amber" disabled={!valid || busy} data-testid="enroll-submit">
            {busy ? "Dando de alta…" : "Dar de alta"}
          </Button>
        </DialogActions>
      </form>
    </Dialog>
  );
}

export default function AdminDevicesPage() {
  const devices = useApi<DeviceSummary[]>("/api/devices");
  const types = useApi<DeviceType[]>("/api/device-types");
  const [enrolling, setEnrolling] = useState(false);
  const [deleting, setDeleting] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [outcome, setOutcome] = useState<Outcome | null>(null);
  const { reload } = devices;
  useStream({ channels: ["device"] }, useCallback(() => reload(), [reload]));

  async function remove(name: string) {
    setBusy(true);
    try {
      await api(`/api/devices/${encodeURIComponent(name)}`, { method: "DELETE" });
      setOutcome({ state: "deleted", tone: "success", title: `${name} eliminado`, detail: "Sus certificados se borran; la telemetría y la CRL se conservan." });
    } catch (e) {
      setOutcome({ state: "error", tone: "error", title: `No se pudo eliminar ${name}`, detail: describeError(e) });
    } finally {
      setBusy(false);
      setDeleting(null);
      reload();
    }
  }

  return (
    <>
      <PageHeading
        title="Alta y baja de dispositivos"
        description="POST /api/devices (201 emitido, 202 en curso, 409 duplicado, 422 rechazado) y DELETE /api/devices/{name}."
        testId="page-admin-devices"
        actions={
          <Button color="amber" onClick={() => setEnrolling(true)} disabled={!types.data} data-testid="enroll-open">
            <PlusIcon />
            Dar de alta
          </Button>
        }
      />
      <div className="mt-6">
        <OutcomeCallout outcome={outcome} testId="admin-devices-result" />
        <ErrorCallout error={devices.error ?? types.error} />
        {devices.loading && !devices.data ? (
          <Loading />
        ) : (
          <Table data-testid="admin-devices-table">
            <TableHead>
              <TableRow>
                <TableHeader>Dispositivo</TableHeader>
                <TableHeader>Tipo</TableHeader>
                <TableHeader>Identidad</TableHeader>
                <TableHeader>Sesión</TableHeader>
                <TableHeader className="text-right">Acciones</TableHeader>
              </TableRow>
            </TableHead>
            <TableBody>
              {(devices.data ?? []).map((d) => (
                <TableRow key={d.device} data-testid={`admin-device-row-${d.device}`}>
                  <TableCell className="font-medium">{d.device}</TableCell>
                  <TableCell className="text-zinc-500">{d.type_id}</TableCell>
                  <TableCell>
                    <IdentityBadge value={d.identity} />
                  </TableCell>
                  <TableCell>
                    <SessionBadge value={d.session} />
                  </TableCell>
                  <TableCell className="text-right">
                    <div className="flex justify-end gap-1">
                      <Button plain href={`/device/?name=${encodeURIComponent(d.device)}`} aria-label="Ver detalle">
                        <EyeIcon />
                      </Button>
                      <Button plain href={`/admin/identity/?name=${encodeURIComponent(d.device)}`} data-testid={`identity-${d.device}`}>
                        <FingerPrintIcon />
                        Identidad
                      </Button>
                      <Button plain onClick={() => setDeleting(d.device)} data-testid={`delete-${d.device}`}>
                        <TrashIcon className="fill-red-500!" />
                        Eliminar
                      </Button>
                    </div>
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        )}
      </div>
      <p className="mt-6 text-sm text-zinc-500">
        Para revocar, suspender o rotar el certificado de un dispositivo, abre su{" "}
        <Link href="/admin/identity/" className="text-amber-700 underline">
          identidad
        </Link>
        .
      </p>
      <EnrollDialog
        open={enrolling}
        onClose={() => setEnrolling(false)}
        types={types.data ?? []}
        onDone={(o) => {
          setOutcome(o);
          reload();
        }}
      />
      <ConfirmDialog
        open={deleting !== null}
        title={`Eliminar ${deleting ?? ""}`}
        confirmLabel="Eliminar"
        busy={busy}
        onClose={() => setDeleting(null)}
        onConfirm={() => deleting && remove(deleting)}
        testId="dialog-delete"
      >
        El controlador detiene el proceso y se borran el dispositivo y sus certificados. Los números de serie revocados siguen en la CRL y la telemetría
        histórica se conserva.
      </ConfirmDialog>
    </>
  );
}
