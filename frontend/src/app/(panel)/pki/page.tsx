"use client";

import { Badge } from "@/components/catalyst/badge";
import { Button } from "@/components/catalyst/button";
import { Link } from "@/components/catalyst/link";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { EnrollmentBadge, RevocationBadge } from "@/components/badges";
import { Callout, Empty, ErrorCallout, LiveIndicator, Loading, Mono, PageHeading, Section, Stat } from "@/components/ui";
import { apiText } from "@/lib/api";
import { fmtDateTime, shortHex } from "@/lib/format";
import { useApi, useStream } from "@/lib/hooks";
import type { Enrollment, OriginCert, PkiCa, PkiOrigin } from "@/lib/types";
import { ArrowDownTrayIcon, DocumentTextIcon } from "@heroicons/react/16/solid";
import { useCallback, useEffect, useRef, useState } from "react";

/** Offer `text` as a file download. */
function download(name: string, text: string) {
  const url = URL.createObjectURL(new Blob([text], { type: "application/x-pem-file" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

type OriginView = "workers" | "manager" | "factory";

function OriginTable({ rows, empty }: { rows: OriginCert[]; empty: string }) {
  if (rows.length === 0) return <Empty>{empty}</Empty>;
  return (
    <Table dense data-testid="origin-table">
      <TableHead>
        <TableRow>
          <TableHeader>Nombre</TableHeader>
          <TableHeader>Serie</TableHeader>
          <TableHeader>Huella SHA-256</TableHeader>
          <TableHeader>Validez</TableHeader>
          <TableHeader />
        </TableRow>
      </TableHead>
      <TableBody>
        {rows.map((row) => {
          const label = row.device ?? row.subject_cn ?? row.name ?? row.subject ?? "—";
          return (
            <TableRow key={row.fingerprint} data-testid={`origin-row-${row.fingerprint}`}>
              <TableCell className="font-medium">
                {label}
                {row.current && (
                  <Badge color="sky" className="ml-1">
                    vigente
                  </Badge>
                )}
              </TableCell>
              <TableCell>{row.serial ? <Mono>{shortHex(row.serial, 8)}</Mono> : "—"}</TableCell>
              <TableCell title={row.fingerprint}>
                <Mono>{shortHex(row.fingerprint, 10)}</Mono>
              </TableCell>
              <TableCell className="tabular-nums">
                {fmtDateTime(row.not_before)} → {fmtDateTime(row.not_after)}
              </TableCell>
              <TableCell>
                <Button plain onClick={() => download(`${label}.pem`, row.cert_pem.endsWith("\n") ? row.cert_pem : `${row.cert_pem}\n`)}>
                  <ArrowDownTrayIcon />
                  PEM
                </Button>
              </TableCell>
            </TableRow>
          );
        })}
      </TableBody>
    </Table>
  );
}

export default function PkiPage() {
  const ca = useApi<PkiCa>("/api/pki/ca");
  const origin = useApi<PkiOrigin>("/api/pki/origin");
  const [originView, setOriginView] = useState<OriginView>("workers");
  const enrollments = useApi<Enrollment[]>("/api/enrollments?limit=50");
  const [crl, setCrl] = useState<string | null>(null);
  const [crlError, setCrlError] = useState<unknown>(null);
  const [loadingCrl, setLoadingCrl] = useState(false);
  const crlShown = useRef(false);
  useEffect(() => {
    crlShown.current = crl !== null;
  }, [crl]);

  const fetchCrl = useCallback(async () => {
    setLoadingCrl(true);
    setCrlError(null);
    try {
      const pem = await apiText("/api/pki/crl");
      setCrl(pem);
      return pem;
    } catch (e) {
      setCrlError(e);
      return null;
    } finally {
      setLoadingCrl(false);
    }
  }, []);

  const reloadCa = ca.reload;
  const reloadEnrollments = enrollments.reload;
  const reloadOrigin = origin.reload;
  const onEvent = useCallback(() => {
    reloadCa();
    reloadEnrollments();
    reloadOrigin();
    // A CRL on screen is fetched again, freshly signed, when a revocation changes.
    if (crlShown.current) void fetchCrl();
  }, [reloadCa, reloadEnrollments, reloadOrigin, fetchCrl]);
  const stream = useStream({ channels: ["enrollment", "device"] }, onEvent);

  const data = ca.data;
  const active = data?.roots.find((r) => r.fingerprint === data.signing_fingerprint);

  return (
    <>
      <PageHeading
        title="PKI de dispositivos"
        description="La fábrica firma las identidades que SROS2 presenta al manager. El servidor sigue firmando las CSR de la demo, y el manager firma la lista que cada worker verifica."
        testId="page-pki"
        actions={<LiveIndicator {...stream} />}
      />
      <Section
        title="Círculo de confianza"
        testId="section-origin"
        actions={
          <div className="flex gap-2" data-testid="origin-switch">
            {(
              [
                ["workers", "Workers"],
                ["manager", "Manager"],
                ["factory", "Fábrica"],
              ] as const
            ).map(([id, label]) =>
              originView === id ? (
                <Button key={id} onClick={() => setOriginView(id)} data-testid={`origin-${id}`}>
                  {label}
                </Button>
              ) : (
                <Button key={id} outline onClick={() => setOriginView(id)} data-testid={`origin-${id}`}>
                  {label}
                </Button>
              ),
            )}
          </div>
        }
      >
        <p className="mb-4 text-sm text-zinc-600">
          El worker deriva su clave del PUF y la fábrica firma el certificado. SROS2 lo presenta al manager, que tiene esa
          raíz. El manager firma la lista de revocación y cada worker la comprueba con el certificado del manager.
        </p>
        <ErrorCallout error={origin.error} />
        {origin.error ? null : !origin.data ? (
          <Loading />
        ) : (
          <>
            <OriginTable
              empty={
                originView === "workers"
                  ? "Ningún worker tiene todavía un certificado firmado por la fábrica."
                  : originView === "manager"
                    ? "El manager aún no tiene certificado firmado por la fábrica."
                    : "No hay raíz de fábrica registrada."
              }
              rows={
                originView === "workers"
                  ? origin.data.workers
                  : originView === "manager"
                    ? origin.data.manager
                      ? [origin.data.manager]
                      : []
                    : origin.data.factory
                      ? [origin.data.factory]
                      : []
              }
            />
            <div className="mt-6" data-testid="origin-crl">
              <h3 className="mb-2 text-sm font-medium text-zinc-950">Lista firmada por el manager</h3>
              {!origin.data.crl ? (
                <Empty>El manager aún no ha firmado su lista.</Empty>
              ) : origin.data.crl.serials.length === 0 ? (
                <Empty>La lista firmada no contiene ningún serial.</Empty>
              ) : (
                <Table dense>
                  <TableHead>
                    <TableRow>
                      <TableHeader>Serie</TableHeader>
                      <TableHeader>Firmada</TableHeader>
                    </TableRow>
                  </TableHead>
                  <TableBody>
                    {origin.data.crl.serials.map((serial) => (
                      <TableRow key={serial} data-testid={`origin-crl-row-${serial}`}>
                        <TableCell>
                          <Mono>{shortHex(serial, 8)}</Mono>
                        </TableCell>
                        <TableCell className="tabular-nums">{fmtDateTime(origin.data?.crl?.signed_at)}</TableCell>
                      </TableRow>
                    ))}
                  </TableBody>
                </Table>
              )}
            </div>
          </>
        )}
      </Section>
      <div className="mt-6">
        <ErrorCallout error={ca.error} />
      </div>
      {!data ? (
        <Loading />
      ) : (
        <>
          <div className="mt-6 grid grid-cols-1 gap-4 sm:grid-cols-3">
            <Stat label="CA firmante" value={<span className="text-base">{active?.name ?? "—"}</span>} testId="stat-ca" />
            <Stat label="CA raíz de confianza" value={data.roots.length} />
            <Stat label="Certificados revocados" value={data.revoked.length} testId="stat-revoked" />
          </div>

          <Section title="CA raíz" testId="section-roots">
            <Table dense data-testid="roots-table">
              <TableHead>
                <TableRow>
                  <TableHeader>Nombre</TableHeader>
                  <TableHeader>Sujeto</TableHeader>
                  <TableHeader>Huella SHA-256</TableHeader>
                  <TableHeader>Validez</TableHeader>
                  <TableHeader />
                </TableRow>
              </TableHead>
              <TableBody>
                {data.roots.map((r) => (
                  <TableRow key={r.ca_id} data-testid={`root-row-${r.ca_id}`} data-signing={String(r.fingerprint === data.signing_fingerprint)}>
                    <TableCell className="font-medium">
                      {r.name}{" "}
                      {r.fingerprint === data.signing_fingerprint && (
                        <Badge color="sky" className="ml-1">
                          firma
                        </Badge>
                      )}
                      {!r.is_active && (
                        <Badge color="zinc" className="ml-1">
                          inactiva
                        </Badge>
                      )}
                    </TableCell>
                    <TableCell className="text-zinc-600">{r.subject}</TableCell>
                    <TableCell title={r.fingerprint} data-testid="root-fingerprint" data-fingerprint={r.fingerprint}>
                      <Mono>{shortHex(r.fingerprint, 10)}</Mono>
                    </TableCell>
                    <TableCell className="tabular-nums">
                      {fmtDateTime(r.not_before)} → {fmtDateTime(r.not_after)}
                    </TableCell>
                    <TableCell>
                      <Button plain onClick={() => download(`${r.name}.pem`, r.cert_pem + "\n")} data-testid={`download-ca-${r.ca_id}`}>
                        <ArrowDownTrayIcon />
                        PEM
                      </Button>
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </Section>

          <Section
            title="Lista de revocación (CRL)"
            testId="section-crl"
            actions={
              <div className="flex gap-2">
                <Button outline onClick={() => void fetchCrl()} disabled={loadingCrl} data-testid="crl-show">
                  <DocumentTextIcon />
                  {crl ? "Actualizar CRL" : "Ver CRL firmada"}
                </Button>
                <Button
                  onClick={async () => {
                    const pem = crl ?? (await fetchCrl());
                    if (pem) download("pgdemo-crl.pem", pem);
                  }}
                  data-testid="crl-download"
                >
                  <ArrowDownTrayIcon />
                  Descargar CRL
                </Button>
              </div>
            }
          >
            <ErrorCallout error={crlError} />
            {crl && (
              <pre className="mb-6 max-h-48 overflow-auto rounded-lg bg-zinc-950 p-4 text-xs text-zinc-100" data-testid="crl-pem">
                {crl}
              </pre>
            )}
            {data.revoked.length === 0 ? (
              <Empty>No hay certificados revocados.</Empty>
            ) : (
              <Table dense data-testid="crl-table">
                <TableHead>
                  <TableRow>
                    <TableHeader>Serie</TableHeader>
                    <TableHeader>Dispositivo</TableHeader>
                    <TableHeader>Motivo</TableHeader>
                    <TableHeader>Revocado</TableHeader>
                    <TableHeader>Por</TableHeader>
                  </TableRow>
                </TableHead>
                <TableBody>
                  {data.revoked.map((r) => (
                    <TableRow key={`${r.ca}-${r.serial}`} data-testid={`crl-row-${r.serial}`}>
                      <TableCell>
                        <Mono>{shortHex(r.serial, 8)}</Mono>
                      </TableCell>
                      <TableCell>
                        <Link href={`/device/?name=${encodeURIComponent(r.device)}`} className="text-sky-700 hover:underline">
                          {r.device}
                        </Link>
                      </TableCell>
                      <TableCell>
                        <RevocationBadge reason={r.reason} />
                      </TableCell>
                      <TableCell className="tabular-nums">{fmtDateTime(r.revoked_at)}</TableCell>
                      <TableCell>{r.revoked_by ?? "—"}</TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            )}
            <div className="mt-4">
              <Callout tone="info">
                <span className="font-medium">superseded</span> marca el certificado anterior tras una rotación;{" "}
                <span className="font-medium">certificateHold</span> es la única revocación reversible.
              </Callout>
            </div>
          </Section>
        </>
      )}

      <Section title="Solicitudes de alta y rotación" testId="section-enrollments">
        <ErrorCallout error={enrollments.error} />
        <Table dense data-testid="enrollments-table">
          <TableHead>
            <TableRow>
              <TableHeader>#</TableHeader>
              <TableHeader>Dispositivo</TableHeader>
              <TableHeader>Tipo</TableHeader>
              <TableHeader>Estado</TableHeader>
              <TableHeader>Solicitada</TableHeader>
              <TableHeader>Resuelta</TableHeader>
            </TableRow>
          </TableHead>
          <TableBody>
            {(enrollments.data ?? []).map((e) => (
              <TableRow key={e.request_id} data-testid={`enrollment-row-${e.request_id}`}>
                <TableCell className="tabular-nums">{e.request_id}</TableCell>
                <TableCell>{e.device}</TableCell>
                <TableCell>{e.kind === "ENROLL" ? `Alta (${e.type_id})` : "Rotación"}</TableCell>
                <TableCell>
                  <EnrollmentBadge value={e.status} />
                  {e.error_code && <span className="ml-2 text-xs text-red-700">{e.error_code}</span>}
                </TableCell>
                <TableCell className="tabular-nums">
                  {fmtDateTime(e.requested_at)} <span className="text-zinc-500">· {e.requested_by}</span>
                </TableCell>
                <TableCell className="tabular-nums">{fmtDateTime(e.decided_at)}</TableCell>
              </TableRow>
            ))}
          </TableBody>
        </Table>
      </Section>
    </>
  );
}
