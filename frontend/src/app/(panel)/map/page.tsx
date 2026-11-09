"use client";

import { Link } from "@/components/catalyst/link";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { SessionBadge } from "@/components/badges";
import { GpsMap, project, type Track } from "@/components/charts";
import { Empty, ErrorCallout, LiveIndicator, Loading, PageHeading } from "@/components/ui";
import { api } from "@/lib/api";
import { fmtLatLon, fmtNumber, fmtTime } from "@/lib/format";
import { useApi, useStream } from "@/lib/hooks";
import type { SseEvent } from "@/lib/sse";
import type { DeviceSummary, GpsEvent, TelemetryRow } from "@/lib/types";
import { useCallback, useEffect, useMemo, useState } from "react";

const KEEP = 120;

type Fix = { lat: number; lon: number; t: string };

export default function MapPage() {
  const devices = useApi<DeviceSummary[]>("/api/devices");
  const gps = useMemo(() => (devices.data ?? []).filter((d) => d.payload === "gps"), [devices.data]);
  const [fixes, setFixes] = useState<Record<string, Fix[]>>({});
  const names = gps.map((d) => d.device).join(",");

  useEffect(() => {
    if (!names) return;
    const ctrl = new AbortController();
    for (const name of names.split(",")) {
      api<TelemetryRow[]>(`/api/devices/${encodeURIComponent(name)}/history?limit=${KEEP}`, { signal: ctrl.signal }).then(
        (rows) => {
          const past = rows
            .filter((r) => r.lat !== undefined && r.lon !== undefined)
            .map((r) => ({ lat: r.lat!, lon: r.lon!, t: r.recorded_at }))
            .reverse();
          setFixes((m) => ({ ...m, [name]: [...past, ...(m[name] ?? []).filter((f) => f.t > (past.at(-1)?.t ?? ""))].slice(-KEEP) }));
        },
        () => {},
      );
    }
    return () => ctrl.abort();
  }, [names]);

  const onEvent = useCallback((ev: SseEvent) => {
    if (ev.event !== "telemetry_gps") return;
    const d = JSON.parse(ev.data) as GpsEvent;
    setFixes((m) => ({ ...m, [d.device]: [...(m[d.device] ?? []), { lat: d.lat, lon: d.lon, t: d.ts }].slice(-KEEP) }));
  }, []);
  const stream = useStream({ channels: ["telemetry_gps"] }, onEvent);

  const tracks: Track[] = gps.map((d) => ({
    device: d.device,
    fixes: fixes[d.device]?.length ? fixes[d.device] : d.lat !== null && d.lon !== null ? [{ lat: d.lat, lon: d.lon }] : [],
  }));

  return (
    <>
      <PageHeading
        title="Mapa"
        description="Posiciones GPS en metros alrededor del origen de las órbitas (Puerta del Sol). Sin teselas externas: el panel funciona sin conexión a Internet."
        testId="page-map"
        actions={<LiveIndicator {...stream} />}
      />
      <div className="mt-8">
        <ErrorCallout error={devices.error} />
        {devices.loading && !devices.data ? (
          <Loading />
        ) : gps.length === 0 ? (
          <Empty>No hay dispositivos GPS (tipo gps_orbit).</Empty>
        ) : (
          <div className="grid items-start gap-8 lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
            <GpsMap tracks={tracks} />
            <Table dense data-testid="gps-table">
              <TableHead>
                <TableRow>
                  <TableHeader>Dispositivo</TableHeader>
                  <TableHeader>Posición</TableHeader>
                  <TableHeader className="text-right">Distancia</TableHeader>
                  <TableHeader>Hora</TableHeader>
                  <TableHeader>Sesión</TableHeader>
                </TableRow>
              </TableHead>
              <TableBody>
                {gps.map((d) => {
                  const last = fixes[d.device]?.at(-1);
                  const lat = last?.lat ?? d.lat;
                  const lon = last?.lon ?? d.lon;
                  const p = lat !== null && lon !== null ? project(lat, lon) : null;
                  return (
                    <TableRow key={d.device} data-testid={`gps-row-${d.device}`}>
                      <TableCell className="font-medium">
                        <Link href={`/device/?name=${encodeURIComponent(d.device)}`} className="text-sky-700 hover:underline">
                          {d.device}
                        </Link>
                      </TableCell>
                      <TableCell className="tabular-nums">{fmtLatLon(lat, lon)}</TableCell>
                      <TableCell className="text-right tabular-nums">{p ? `${fmtNumber(Math.hypot(p.x, p.y), 0)} m` : "—"}</TableCell>
                      <TableCell className="tabular-nums">{fmtTime(last?.t ?? d.updated_at)}</TableCell>
                      <TableCell>
                        <SessionBadge value={d.session} />
                      </TableCell>
                    </TableRow>
                  );
                })}
              </TableBody>
            </Table>
          </div>
        )}
      </div>
    </>
  );
}
