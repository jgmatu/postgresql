"use client";

import { Badge } from "@/components/catalyst/badge";
import { Button } from "@/components/catalyst/button";
import { DescriptionDetails, DescriptionList, DescriptionTerm } from "@/components/catalyst/description-list";
import { Select } from "@/components/catalyst/select";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { TextLink } from "@/components/catalyst/text";
import { EnrollmentBadge, IdentityBadge, RevocationBadge, SessionBadge, SyncBadge } from "@/components/badges";
import { GpsMap, LineChart, type Point } from "@/components/charts";
import { Callout, Empty, ErrorCallout, LiveIndicator, Loading, Mono, PageHeading, Section, Stat } from "@/components/ui";
import { ApiError } from "@/lib/api";
import { fmtDateTime, fmtLatLon, fmtNumber, fmtTime, shortHex } from "@/lib/format";
import { useApi, useSession, useStream } from "@/lib/hooks";
import type { SseEvent } from "@/lib/sse";
import type { Certificate, ConfigHistoryRow, DeviceDetail, Enrollment, GpsEvent, ScalarEvent, TelemetryRow } from "@/lib/types";
import { AdjustmentsHorizontalIcon, ArrowLeftIcon, FingerPrintIcon } from "@heroicons/react/16/solid";
import { useSearchParams } from "next/navigation";
import { Suspense, useCallback, useMemo, useState } from "react";

const MAX_POINTS = 600;

interface Fix {
  seq: number;
  t: number;
  value?: number;
  lat?: number;
  lon?: number;
}

function DeviceView({ name }: { name: string }) {
  const session = useSession();
  const enc = encodeURIComponent(name);
  const [limit, setLimit] = useState(200);
  const detail = useApi<DeviceDetail>(`/api/devices/${enc}`);
  const history = useApi<TelemetryRow[]>(`/api/devices/${enc}/history?limit=${limit}`);
  const certs = useApi<Certificate[]>(`/api/devices/${enc}/certificates`);
  const enrollments = useApi<Enrollment[]>(`/api/enrollments?device=${enc}&limit=20`);
  const configHistory = useApi<ConfigHistoryRow[]>(`/api/config/history?device=${enc}&limit=20`);
  const [live, setLive] = useState<Fix[]>([]);

  const reloadDetail = detail.reload;
  const reloadCerts = certs.reload;
  const reloadEnrollments = enrollments.reload;
  const reloadConfig = configHistory.reload;
  const onEvent = useCallback(
    (ev: SseEvent) => {
      const d = JSON.parse(ev.data) as Partial<ScalarEvent & GpsEvent>;
      switch (ev.event) {
        case "telemetry_scalar":
        case "telemetry_gps":
          setLive((xs) => [...xs, { seq: d.seq!, t: Date.parse(d.ts!), value: d.value, lat: d.lat, lon: d.lon }].slice(-MAX_POINTS));
          return;
        case "config":
        case "error":
          reloadDetail();
          reloadConfig();
          return;
        default:
          reloadDetail();
          reloadCerts();
          reloadEnrollments();
      }
    },
    [reloadDetail, reloadCerts, reloadEnrollments, reloadConfig],
  );
  const stream = useStream({ devices: [name] }, onEvent);

  const fixes = useMemo(() => {
    const past: Fix[] = (history.data ?? [])
      .map((r) => ({ seq: r.seq, t: Date.parse(r.recorded_at), value: r.value, lat: r.lat, lon: r.lon }))
      .reverse();
    const lastSeq = past.length ? past[past.length - 1].seq : -1;
    const lastT = past.length ? past[past.length - 1].t : -Infinity;
    // seq restarts with the device process, so a new process is newer by time.
    return [...past, ...live.filter((f) => f.t > lastT || f.seq > lastSeq)].slice(-MAX_POINTS);
  }, [history.data, live]);

  if (detail.error instanceof ApiError && detail.error.status === 404) {
    return (
      <>
        <PageHeading title={name} />
        <div className="mt-6">
          <Callout tone="warning" testId="device-not-found" title="Dispositivo no encontrado">
            No existe ningún dispositivo llamado «{name}». <TextLink href="/devices/">Volver a la lista</TextLink>.
          </Callout>
        </div>
      </>
    );
  }

  const d = detail.data;
  const isAdmin = session?.role === "admin";
  const latest = fixes[fixes.length - 1];
  const gps = d?.payload === "gps";

  return (
    <>
      <div className="mb-4">
        <TextLink href="/devices/" className="inline-flex items-center gap-1 text-sm">
          <ArrowLeftIcon className="size-4" /> Dispositivos
        </TextLink>
      </div>
      <PageHeading
        title={
          <span className="flex flex-wrap items-center gap-3" data-testid="device-title">
            {name}
            {d && <Badge color="sky">{d.type_id}</Badge>}
            {d && <IdentityBadge value={d.identity} />}
            {d && <SessionBadge value={d.process?.state ?? d.session} />}
          </span>
        }
        description={d ? `Registrado el ${fmtDateTime(d.created_at)} · carga ${d.payload === "gps" ? "GPS (PostGIS)" : "escalar"}` : undefined}
        actions={
          <>
            <LiveIndicator {...stream} />
            {isAdmin && (
              <>
                <Button href={`/admin/identity/?name=${enc}`} color="amber" data-testid="manage-identity">
                  <FingerPrintIcon />
                  Gestionar identidad
                </Button>
                <Button href={`/admin/config/?device=${enc}`} outline data-testid="manage-config">
                  <AdjustmentsHorizontalIcon />
                  Cambiar configuración
                </Button>
              </>
            )}
          </>
        }
      />
      <div className="mt-6">
        <ErrorCallout error={detail.error} />
      </div>
      {!d ? (
        <Loading />
      ) : (
        <>
          <div className="mt-6 grid grid-cols-2 gap-4 lg:grid-cols-4">
            <Stat label={gps ? "Posición" : "Valor"} value={gps ? fmtLatLon(latest?.lat ?? d.lat, latest?.lon ?? d.lon) : fmtNumber(latest?.value ?? d.value, 3)} testId="stat-value" />
            <Stat label="seq" value={<span data-testid="device-seq">{latest?.seq ?? d.seq ?? "—"}</span>} />
            <Stat label="Última muestra" value={fmtTime(latest ? new Date(latest.t).toISOString() : d.updated_at)} />
            <Stat label="Proceso" value={d.process?.pid ? `PID ${d.process.pid}` : "—"} />
          </div>

          <Section
            title={gps ? "Recorrido" : "Telemetría"}
            testId="section-telemetry"
            actions={
              <Select aria-label="Muestras" value={limit} onChange={(e) => setLimit(Number(e.target.value))} className="max-w-40" data-testid="history-limit">
                <option value={50}>50 muestras</option>
                <option value={200}>200 muestras</option>
                <option value={1000}>1000 muestras</option>
              </Select>
            }
          >
            {gps ? (
              <GpsMap tracks={[{ device: name, fixes: fixes.filter((f) => f.lat !== undefined).map((f) => ({ lat: f.lat!, lon: f.lon! })) }]} />
            ) : (
              <LineChart points={fixes.filter((f) => f.value !== undefined).map((f): Point => ({ t: f.t, v: f.value! }))} />
            )}
          </Section>

          <div className="grid gap-x-10 lg:grid-cols-2">
            <Section title="Configuración" testId="section-config" actions={<SyncBadge value={d.config?.sync} />}>
              {d.config ? (
                <DescriptionList>
                  <DescriptionTerm>Aplicada</DescriptionTerm>
                  <DescriptionDetails data-testid="config-applied">
                    {d.config.applied_rate_ms ?? "—"} ms · amplitud {fmtNumber(d.config.applied_amplitude, 2)} · {d.config.applied_is_active ? "activo" : "pausado"}
                  </DescriptionDetails>
                  <DescriptionTerm>Aplicada el</DescriptionTerm>
                  <DescriptionDetails>{fmtDateTime(d.config.applied_at)}</DescriptionDetails>
                  <DescriptionTerm>Deseada</DescriptionTerm>
                  <DescriptionDetails data-testid="config-desired">
                    {d.config.desired_request_id
                      ? `${d.config.desired_rate_ms} ms · amplitud ${fmtNumber(d.config.desired_amplitude, 2)} · ${d.config.desired_is_active ? "activo" : "pausado"} (petición ${d.config.desired_request_id}, ${d.config.desired_status})`
                      : "igual que la aplicada"}
                  </DescriptionDetails>
                  {d.config.last_error_code && (
                    <>
                      <DescriptionTerm>Último error</DescriptionTerm>
                      <DescriptionDetails data-testid="config-last-error">
                        <Badge color="red">{d.config.last_error_code}</Badge> {d.config.last_error_detail} ({fmtDateTime(d.config.last_error_at)})
                      </DescriptionDetails>
                    </>
                  )}
                </DescriptionList>
              ) : (
                <Empty>Sin configuración.</Empty>
              )}
            </Section>

            <Section title="Certificado actual" testId="section-certificate">
              {d.certificate ? (
                <DescriptionList>
                  <DescriptionTerm>Serie</DescriptionTerm>
                  <DescriptionDetails>
                    <Mono>{d.certificate.serial}</Mono>
                  </DescriptionDetails>
                  <DescriptionTerm>Huella SHA-256</DescriptionTerm>
                  <DescriptionDetails>
                    <Mono>{d.certificate.fingerprint}</Mono>
                  </DescriptionDetails>
                  <DescriptionTerm>Validez</DescriptionTerm>
                  <DescriptionDetails>
                    {fmtDateTime(d.certificate.not_before)} → {fmtDateTime(d.certificate.not_after)}
                  </DescriptionDetails>
                  <DescriptionTerm>CA</DescriptionTerm>
                  <DescriptionDetails>{d.certificate.ca}</DescriptionDetails>
                  {d.certificate.revocation_reason && (
                    <>
                      <DescriptionTerm>Revocado</DescriptionTerm>
                      <DescriptionDetails>
                        <RevocationBadge reason={d.certificate.revocation_reason} /> {fmtDateTime(d.certificate.revoked_at)}
                      </DescriptionDetails>
                    </>
                  )}
                </DescriptionList>
              ) : (
                <Empty>Sin certificado.</Empty>
              )}
            </Section>
          </div>

          <Section title="Histórico de muestras" testId="section-history">
            <ErrorCallout error={history.error} />
            <Table dense data-testid="history-table">
              <TableHead>
                <TableRow>
                  <TableHeader>Registrada</TableHeader>
                  <TableHeader className="text-right">seq</TableHeader>
                  <TableHeader>{gps ? "Posición" : "Valor"}</TableHeader>
                  <TableHeader>Partición</TableHeader>
                </TableRow>
              </TableHead>
              <TableBody>
                {(history.data ?? []).slice(0, 15).map((r) => (
                  <TableRow key={r.history_id}>
                    <TableCell className="tabular-nums">{fmtDateTime(r.recorded_at)}</TableCell>
                    <TableCell className="text-right tabular-nums">{r.seq}</TableCell>
                    <TableCell className="tabular-nums">{gps ? fmtLatLon(r.lat, r.lon) : fmtNumber(r.value, 3)}</TableCell>
                    <TableCell>
                      <Mono className="text-zinc-500">
                        <span data-testid="history-partition">{r.partition}</span>
                      </Mono>
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </Section>

          <Section title="Certificados" testId="section-certificates">
            <ErrorCallout error={certs.error} />
            <Table dense data-testid="certificates-table">
              <TableHead>
                <TableRow>
                  <TableHeader>Serie</TableHeader>
                  <TableHeader>Emitido</TableHeader>
                  <TableHeader>Válido hasta</TableHeader>
                  <TableHeader>Estado</TableHeader>
                  <TableHeader>Revocación</TableHeader>
                </TableRow>
              </TableHead>
              <TableBody>
                {(certs.data ?? []).map((c) => (
                  <TableRow key={c.cert_id} data-testid={`cert-row-${c.serial}`}>
                    <TableCell>
                      <Mono>{shortHex(c.serial, 8)}</Mono>
                    </TableCell>
                    <TableCell className="tabular-nums">{fmtDateTime(c.issued_at)}</TableCell>
                    <TableCell className="tabular-nums">{fmtDateTime(c.not_after)}</TableCell>
                    <TableCell>{c.current ? <Badge color="sky">actual</Badge> : <Badge color="zinc">anterior</Badge>}</TableCell>
                    <TableCell>
                      <RevocationBadge reason={c.revocation_reason} />
                      {c.revoked_by && <span className="ml-2 text-xs text-zinc-500">por {c.revoked_by}</span>}
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </Section>

          <div className="grid gap-x-10 lg:grid-cols-2">
            <Section title="Altas y rotaciones" testId="section-enrollments">
              <Table dense data-testid="enrollments-table">
                <TableHead>
                  <TableRow>
                    <TableHeader>#</TableHeader>
                    <TableHeader>Tipo</TableHeader>
                    <TableHeader>Estado</TableHeader>
                    <TableHeader>Solicitada</TableHeader>
                  </TableRow>
                </TableHead>
                <TableBody>
                  {(enrollments.data ?? []).map((e) => (
                    <TableRow key={e.request_id}>
                      <TableCell className="tabular-nums">{e.request_id}</TableCell>
                      <TableCell>{e.kind === "ENROLL" ? "Alta" : "Rotación"}</TableCell>
                      <TableCell>
                        <EnrollmentBadge value={e.status} />
                        {e.error_code && <span className="ml-2 text-xs text-red-700">{e.error_code}</span>}
                      </TableCell>
                      <TableCell className="tabular-nums">
                        {fmtDateTime(e.requested_at)} <span className="text-zinc-500">· {e.requested_by}</span>
                      </TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </Section>

            <Section title="Configuraciones aplicadas" testId="section-config-history">
              <Table dense data-testid="config-history-table">
                <TableHead>
                  <TableRow>
                    <TableHeader>Fecha</TableHeader>
                    <TableHeader className="text-right">rate_ms</TableHeader>
                    <TableHeader className="text-right">Amplitud</TableHeader>
                    <TableHeader>Activo</TableHeader>
                  </TableRow>
                </TableHead>
                <TableBody>
                  {(configHistory.data ?? []).map((h) => (
                    <TableRow key={h.history_id}>
                      <TableCell className="tabular-nums">{fmtDateTime(h.recorded_at)}</TableCell>
                      <TableCell className="text-right tabular-nums">{h.rate_ms}</TableCell>
                      <TableCell className="text-right tabular-nums">{fmtNumber(h.amplitude, 2)}</TableCell>
                      <TableCell>{h.is_active ? "sí" : "no"}</TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            </Section>
          </div>
        </>
      )}
    </>
  );
}

function DeviceFromQuery() {
  const name = useSearchParams().get("name");
  if (!name) {
    return (
      <>
        <PageHeading title="Dispositivo" />
        <div className="mt-6">
          <Callout tone="info">
            Elige un dispositivo en la <TextLink href="/devices/">lista</TextLink>.
          </Callout>
        </div>
      </>
    );
  }
  return <DeviceView key={name} name={name} />;
}

export default function DevicePage() {
  return (
    <Suspense fallback={<Loading />}>
      <DeviceFromQuery />
    </Suspense>
  );
}
