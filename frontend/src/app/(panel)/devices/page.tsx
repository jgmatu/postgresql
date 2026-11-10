"use client";

import { Button } from "@/components/catalyst/button";
import { Link } from "@/components/catalyst/link";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { IdentityBadge, SessionBadge } from "@/components/badges";
import { Sparkline } from "@/components/charts";
import { Empty, ErrorCallout, LiveIndicator, Loading, PageHeading, Stat } from "@/components/ui";
import { fmtLatLon, fmtNumber, fmtTime, fmtXy, fmtXyz } from "@/lib/format";
import { useApi, useSession, useStream } from "@/lib/hooks";
import type { SseEvent } from "@/lib/sse";
import type { DeviceSummary, GpsEvent, ScalarEvent, XyEvent, XyzEvent } from "@/lib/types";
import { WrenchScrewdriverIcon } from "@heroicons/react/16/solid";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";

const SPARK = 40;

type Live = Pick<DeviceSummary, "value" | "lat" | "lon" | "x" | "y" | "z" | "seq" | "updated_at">;

export default function DevicesPage() {
  const session = useSession();
  const list = useApi<DeviceSummary[]>("/api/devices");
  const [live, setLive] = useState<Record<string, Live>>({});
  const [spark, setSpark] = useState<Record<string, number[]>>({});
  const [events, setEvents] = useState(0);
  const reloadTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const { reload } = list;

  useEffect(() => () => clearTimeout(reloadTimer.current), []);

  const onEvent = useCallback(
    (ev: SseEvent) => {
      setEvents((n) => n + 1);
      if (ev.event === "telemetry_scalar" || ev.event === "telemetry_gps" || ev.event === "telemetry_xy" || ev.event === "telemetry_xyz") {
        const d = JSON.parse(ev.data) as ScalarEvent & GpsEvent & XyEvent & XyzEvent;
        setLive((m) => ({
          ...m,
          [d.device]: {
            value: ev.event === "telemetry_scalar" ? d.value : null,
            lat: ev.event === "telemetry_gps" ? d.lat : null,
            lon: ev.event === "telemetry_gps" ? d.lon : null,
            x: ev.event === "telemetry_xy" || ev.event === "telemetry_xyz" ? d.x : null,
            y: ev.event === "telemetry_xy" || ev.event === "telemetry_xyz" ? d.y : null,
            z: ev.event === "telemetry_xyz" ? d.z : null,
            seq: d.seq,
            updated_at: d.ts,
          },
        }));
        if (ev.event === "telemetry_scalar")
          setSpark((m) => ({ ...m, [d.device]: [...(m[d.device] ?? []), d.value].slice(-SPARK) }));
      } else {
        // Registration, removal, revocation or a session change: re-read the list.
        clearTimeout(reloadTimer.current);
        reloadTimer.current = setTimeout(reload, 300);
      }
    },
    [reload],
  );
  const stream = useStream({ channels: ["telemetry_scalar", "telemetry_gps", "telemetry_xy", "telemetry_xyz", "device"] }, onEvent);

  const rows = useMemo(
    () => (list.data ?? []).map((d) => ({ ...d, ...(live[d.device] && (live[d.device].seq ?? 0) >= (d.seq ?? 0) ? live[d.device] : {}) })),
    [list.data, live],
  );
  const online = rows.filter((d) => d.session === "ONLINE").length;
  const active = rows.filter((d) => d.identity === "ACTIVE").length;

  return (
    <>
      <PageHeading
        title="Dispositivos"
        description="Última muestra de cada dispositivo, actualizada en vivo por Server-Sent Events."
        testId="page-devices"
        actions={
          <>
            <LiveIndicator {...stream} />
            {session?.role === "admin" && (
              <Button href="/admin/devices/" color="amber" data-testid="manage-devices">
                <WrenchScrewdriverIcon />
                Gestionar
              </Button>
            )}
          </>
        }
      />
      <div className="mt-6 grid grid-cols-2 gap-4 lg:grid-cols-4">
        <Stat label="Dispositivos" value={rows.length} testId="stat-devices" />
        <Stat label="En línea" value={online} testId="stat-online" />
        <Stat label="Identidad activa" value={active} testId="stat-active" />
        <Stat label="Eventos recibidos" value={events} testId="stat-events" />
      </div>
      <div className="mt-8">
        <ErrorCallout error={list.error} />
        {list.loading && !list.data ? (
          <Loading />
        ) : rows.length === 0 ? (
          <Empty>No hay dispositivos registrados.</Empty>
        ) : (
          <Table className="[--gutter:--spacing(6)] lg:[--gutter:--spacing(10)]" data-testid="devices-table">
            <TableHead>
              <TableRow>
                <TableHeader>Dispositivo</TableHeader>
                <TableHeader>Tipo</TableHeader>
                <TableHeader>Última muestra</TableHeader>
                <TableHeader className="hidden md:table-cell">Tendencia</TableHeader>
                <TableHeader className="text-right">seq</TableHeader>
                <TableHeader>Actualizado</TableHeader>
                <TableHeader>Identidad</TableHeader>
                <TableHeader>Sesión</TableHeader>
              </TableRow>
            </TableHead>
            <TableBody>
              {rows.map((d) => (
                <TableRow key={d.device} data-testid={`device-row-${d.device}`}>
                  <TableCell className="font-medium">
                    <Link href={`/device/?name=${encodeURIComponent(d.device)}`} className="text-sky-700 hover:underline" data-testid={`device-link-${d.device}`}>
                      {d.device}
                    </Link>
                  </TableCell>
                  <TableCell className="text-zinc-500">{d.type_id}</TableCell>
                  <TableCell className="tabular-nums" data-testid="device-value">
                    {d.payload === "gps" ? fmtLatLon(d.lat, d.lon) : d.payload === "xyz" ? fmtXyz(d.x, d.y, d.z) : d.payload === "xy" ? fmtXy(d.x, d.y) : fmtNumber(d.value, 3)}
                  </TableCell>
                  <TableCell className="hidden md:table-cell">
                    {d.payload === "scalar" ? (
                      <Sparkline values={spark[d.device] ?? []} />
                    ) : (
                      <span className="text-xs text-zinc-400">{d.payload === "xyz" ? "x, y, z" : d.payload === "xy" ? "x, y" : "GPS"}</span>
                    )}
                  </TableCell>
                  <TableCell className="text-right tabular-nums" data-testid="device-seq">
                    {d.seq ?? "—"}
                  </TableCell>
                  <TableCell className="tabular-nums text-zinc-500">{fmtTime(d.updated_at)}</TableCell>
                  <TableCell>
                    <IdentityBadge value={d.identity} />
                  </TableCell>
                  <TableCell>
                    <SessionBadge value={d.session} />
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        )}
      </div>
    </>
  );
}
