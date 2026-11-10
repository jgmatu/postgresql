"use client";

import { Field, Label } from "@/components/catalyst/fieldset";
import { Input } from "@/components/catalyst/input";
import { Link } from "@/components/catalyst/link";
import { Select } from "@/components/catalyst/select";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { EnrollmentBadge, RevocationBadge } from "@/components/badges";
import { Empty, ErrorCallout, Loading, Mono, PageHeading, Section } from "@/components/ui";
import { fmtDateTime, fmtLatLon, fmtNumber, fmtXy } from "@/lib/format";
import { useApi, useSession } from "@/lib/hooks";
import type { ConfigHistoryRow, DeviceSummary, Enrollment, PkiCa, TelemetryRow } from "@/lib/types";
import { useState } from "react";

const SAMPLE_LIMITS = [50, 100, 500, 1000] as const;

/** `datetime-local` is wall time; the API wants a timestamptz. */
function iso(local: string): string | null {
  if (!local) return null;
  const d = new Date(local);
  return Number.isNaN(d.getTime()) ? null : d.toISOString();
}

function sampleQuery(device: string, from: string, to: string, limit: number): string | null {
  if (!device) return null;
  const q = new URLSearchParams({ limit: String(limit) });
  const start = iso(from);
  const end = iso(to);
  if (start) q.set("from", start);
  if (end) q.set("to", end);
  return `/api/devices/${encodeURIComponent(device)}/history?${q}`;
}

function withDevice(path: string, device: string): string {
  return device ? `${path}&device=${encodeURIComponent(device)}` : path;
}

function DeviceFilter({
  devices,
  value,
  onChange,
  testId,
  allowAll,
}: {
  devices: DeviceSummary[];
  value: string;
  onChange: (name: string) => void;
  testId: string;
  allowAll?: boolean;
}) {
  return (
    <Field className="min-w-48">
      <Label>Dispositivo</Label>
      <Select value={value} onChange={(e) => onChange(e.target.value)} data-testid={testId}>
        <option value="">{allowAll ? "Todos" : "Elige un dispositivo…"}</option>
        {devices.map((d) => (
          <option key={d.device} value={d.device}>
            {d.device}
          </option>
        ))}
      </Select>
    </Field>
  );
}

function sampleValue(row: TelemetryRow): string {
  if (row.x !== undefined && row.y !== undefined) return fmtXy(row.x, row.y);
  if (row.value !== undefined) return fmtNumber(row.value, 3);
  return fmtLatLon(row.lat, row.lon);
}

export default function HistoryPage() {
  const session = useSession();
  const devices = useApi<DeviceSummary[]>("/api/devices");
  const [device, setDevice] = useState("");
  const [from, setFrom] = useState("");
  const [to, setTo] = useState("");
  const [limit, setLimit] = useState<number>(100);
  const [appliedDevice, setAppliedDevice] = useState("");
  const [enrollmentDevice, setEnrollmentDevice] = useState("");

  const list = devices.data ?? [];
  const samples = useApi<TelemetryRow[]>(sampleQuery(device, from, to, limit));
  const applied = useApi<ConfigHistoryRow[]>(withDevice("/api/config/history?limit=50", appliedDevice));
  const enrollments = useApi<Enrollment[]>(withDevice("/api/enrollments?limit=50", enrollmentDevice));
  const pki = useApi<PkiCa>("/api/pki/ca");
  const role = session?.pgRole ?? "…";

  return (
    <>
      <PageHeading
        title="Histórico"
        description={`Consulta de solo lectura con el rol de la sesión (${role}). Muestras, configuraciones aplicadas, altas y revocaciones.`}
        testId="page-history"
      />

      <Section title="Muestras" testId="section-samples">
        <div className="mb-4 flex flex-wrap items-end gap-3">
          <DeviceFilter devices={list} value={device} onChange={setDevice} testId="history-device" />
          <Field>
            <Label>Desde</Label>
            <Input type="datetime-local" value={from} onChange={(e) => setFrom(e.target.value)} data-testid="sample-from" />
          </Field>
          <Field>
            <Label>Hasta</Label>
            <Input type="datetime-local" value={to} onChange={(e) => setTo(e.target.value)} data-testid="sample-to" />
          </Field>
          <Field className="min-w-36">
            <Label>Muestras</Label>
            <Select aria-label="Muestras" value={limit} onChange={(e) => setLimit(Number(e.target.value))} data-testid="sample-limit">
              {SAMPLE_LIMITS.map((n) => (
                <option key={n} value={n}>
                  {n}
                </option>
              ))}
            </Select>
          </Field>
        </div>
        <ErrorCallout error={samples.error} />
        {!device ? (
          <Empty>Elige un dispositivo. Sin fechas, la consulta cubre la última hora.</Empty>
        ) : samples.loading && !samples.data ? (
          <Loading />
        ) : (samples.data ?? []).length === 0 ? (
          <Empty>Sin muestras en esta ventana.</Empty>
        ) : (
          <Table dense data-testid="sample-history-table">
            <TableHead>
              <TableRow>
                <TableHeader>Fecha</TableHeader>
                <TableHeader className="text-right">seq</TableHeader>
                <TableHeader className="text-right">Valor</TableHeader>
                <TableHeader>Partición</TableHeader>
              </TableRow>
            </TableHead>
            <TableBody>
              {(samples.data ?? []).map((row) => (
                <TableRow key={row.history_id}>
                  <TableCell className="tabular-nums">{fmtDateTime(row.recorded_at)}</TableCell>
                  <TableCell className="text-right tabular-nums">{row.seq}</TableCell>
                  <TableCell className="text-right tabular-nums">{sampleValue(row)}</TableCell>
                  <TableCell>
                    <Mono>
                      <span data-testid="sample-partition">{row.partition}</span>
                    </Mono>
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        )}
      </Section>

      <Section title="Configuraciones aplicadas" testId="section-applied">
        <div className="mb-4">
          <DeviceFilter devices={list} value={appliedDevice} onChange={setAppliedDevice} testId="applied-device" allowAll />
        </div>
        <ErrorCallout error={applied.error} />
        {applied.loading && !applied.data ? (
          <Loading />
        ) : (applied.data ?? []).length === 0 ? (
          <Empty>Sin historial.</Empty>
        ) : (
          <Table dense data-testid="applied-history-table">
            <TableHead>
              <TableRow>
                <TableHeader>Fecha</TableHeader>
                <TableHeader>Dispositivo</TableHeader>
                <TableHeader className="text-right">rate_ms</TableHeader>
                <TableHeader className="text-right">Amplitud</TableHeader>
                <TableHeader>Activo</TableHeader>
                <TableHeader>Petición</TableHeader>
              </TableRow>
            </TableHead>
            <TableBody>
              {(applied.data ?? []).map((h) => (
                <TableRow key={h.history_id}>
                  <TableCell className="tabular-nums">{fmtDateTime(h.recorded_at)}</TableCell>
                  <TableCell>{h.device ?? <span className="text-zinc-400">eliminado (id {h.device_id})</span>}</TableCell>
                  <TableCell className="text-right tabular-nums">{h.rate_ms}</TableCell>
                  <TableCell className="text-right tabular-nums">{fmtNumber(h.amplitude, 2)}</TableCell>
                  <TableCell>{h.is_active ? "sí" : "no"}</TableCell>
                  <TableCell className="tabular-nums">{h.request_id ? `#${h.request_id}` : "alta"}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        )}
      </Section>

      <Section title="Altas y rotaciones" testId="section-enrollment-history">
        <div className="mb-4">
          <DeviceFilter devices={list} value={enrollmentDevice} onChange={setEnrollmentDevice} testId="enrollment-device" allowAll />
        </div>
        <ErrorCallout error={enrollments.error} />
        {enrollments.loading && !enrollments.data ? (
          <Loading />
        ) : (enrollments.data ?? []).length === 0 ? (
          <Empty>Sin solicitudes.</Empty>
        ) : (
          <Table dense data-testid="enrollment-history-table">
            <TableHead>
              <TableRow>
                <TableHeader>#</TableHeader>
                <TableHeader>Dispositivo</TableHeader>
                <TableHeader>Tipo</TableHeader>
                <TableHeader>Estado</TableHeader>
                <TableHeader>Solicitada por</TableHeader>
                <TableHeader>Resuelta</TableHeader>
              </TableRow>
            </TableHead>
            <TableBody>
              {(enrollments.data ?? []).map((e) => (
                <TableRow key={e.request_id}>
                  <TableCell className="tabular-nums">{e.request_id}</TableCell>
                  <TableCell>{e.device}</TableCell>
                  <TableCell>{e.kind === "ENROLL" ? `Alta (${e.type_id ?? "—"})` : "Rotación"}</TableCell>
                  <TableCell>
                    <EnrollmentBadge value={e.status} />
                  </TableCell>
                  <TableCell>
                    {e.requested_by} <span className="text-zinc-500">· {fmtDateTime(e.requested_at)}</span>
                  </TableCell>
                  <TableCell className="tabular-nums">{fmtDateTime(e.decided_at)}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        )}
      </Section>

      <Section title="Revocaciones" testId="section-revocations">
        <ErrorCallout error={pki.error} />
        {pki.loading && !pki.data ? (
          <Loading />
        ) : (pki.data?.revoked ?? []).length === 0 ? (
          <Empty>No hay certificados revocados.</Empty>
        ) : (
          <Table dense data-testid="crl-history-table">
            <TableHead>
              <TableRow>
                <TableHeader>Dispositivo</TableHeader>
                <TableHeader>Serie</TableHeader>
                <TableHeader>Motivo</TableHeader>
                <TableHeader>Fecha</TableHeader>
                <TableHeader>Por</TableHeader>
              </TableRow>
            </TableHead>
            <TableBody>
              {(pki.data?.revoked ?? []).map((r) => (
                <TableRow key={`${r.ca}-${r.serial}`}>
                  <TableCell>
                    <Link href={`/device/?name=${encodeURIComponent(r.device)}`} className="text-sky-700 hover:underline">
                      {r.device}
                    </Link>
                  </TableCell>
                  <TableCell>
                    <Mono>{r.serial}</Mono>
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
      </Section>
    </>
  );
}
