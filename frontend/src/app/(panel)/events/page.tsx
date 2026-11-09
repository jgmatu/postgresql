"use client";

import { Badge } from "@/components/catalyst/badge";
import { Button } from "@/components/catalyst/button";
import { Checkbox, CheckboxField, CheckboxGroup } from "@/components/catalyst/checkbox";
import { Field, Fieldset, Label, Legend } from "@/components/catalyst/fieldset";
import { Input } from "@/components/catalyst/input";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { Callout, Empty, LiveIndicator, Mono, PageHeading } from "@/components/ui";
import { fmtTime } from "@/lib/format";
import { useStream } from "@/lib/hooks";
import type { SseEvent } from "@/lib/sse";
import { CHANNELS, type Channel } from "@/lib/types";
import { PauseIcon, PlayIcon, TrashIcon } from "@heroicons/react/16/solid";
import type { ComponentProps } from "react";
import { useCallback, useEffect, useRef, useState } from "react";

const KEEP = 200;
const DEVICE_RE = /^[a-z][a-z0-9_.-]{1,63}$/;

const COLOR: Record<Channel, NonNullable<ComponentProps<typeof Badge>["color"]>> = {
  telemetry_scalar: "sky",
  telemetry_gps: "cyan",
  device: "violet",
  config: "green",
  error: "red",
  enrollment: "amber",
};

const LABEL: Record<Channel, string> = {
  telemetry_scalar: "Telemetría escalar",
  telemetry_gps: "Telemetría GPS",
  device: "Dispositivos y sesiones",
  config: "Configuración aplicada",
  error: "Errores",
  enrollment: "Altas y rotaciones",
};

interface Row extends SseEvent {
  key: number;
  at: string;
  device: string | null;
}

export default function EventsPage() {
  const [channels, setChannels] = useState<Channel[]>([...CHANNELS]);
  const [deviceInput, setDeviceInput] = useState("");
  const [rows, setRows] = useState<Row[]>([]);
  const [paused, setPaused] = useState(false);
  const counter = useRef(0);
  const pausedRef = useRef(paused);
  useEffect(() => {
    pausedRef.current = paused;
  }, [paused]);

  const devices = deviceInput
    .split(",")
    .map((s) => s.trim())
    .filter(Boolean);
  const badDevice = devices.find((d) => !DEVICE_RE.test(d));
  const applied = badDevice ? [] : devices;

  const onEvent = useCallback((ev: SseEvent) => {
    if (pausedRef.current) return;
    let device: string | null = null;
    try {
      device = (JSON.parse(ev.data) as { device?: string | null }).device ?? null;
    } catch {
      // not JSON
    }
    const row: Row = { ...ev, key: ++counter.current, at: new Date().toISOString(), device };
    setRows((xs) => [row, ...xs].slice(0, KEEP));
  }, []);
  const stream = useStream({ channels: channels.length ? channels : ["device"], devices: applied }, onEvent);

  const toggle = (c: Channel, on: boolean) =>
    setChannels((cs) => (on ? CHANNELS.filter((x) => x === c || cs.includes(x)) : cs.filter((x) => x !== c)));

  return (
    <>
      <PageHeading
        title="Eventos"
        description="Notificaciones de PostgreSQL (LISTEN/NOTIFY) reenviadas por el servidor como Server-Sent Events. Los filtros se aplican en el servidor (?channels=&devices=)."
        testId="page-events"
        actions={
          <>
            <LiveIndicator {...stream} />
            <Button outline onClick={() => setPaused((p) => !p)} data-testid="events-pause">
              {paused ? <PlayIcon /> : <PauseIcon />}
              {paused ? "Reanudar" : "Pausar"}
            </Button>
            <Button plain onClick={() => setRows([])} data-testid="events-clear">
              <TrashIcon />
              Vaciar
            </Button>
          </>
        }
      />
      <div className="mt-8 grid gap-8 lg:grid-cols-[16rem_minmax(0,1fr)]">
        <div className="space-y-8">
          <Fieldset>
            <Legend>Canales</Legend>
            <CheckboxGroup>
              {CHANNELS.map((c) => (
                <CheckboxField key={c}>
                  <Checkbox checked={channels.includes(c)} onChange={(on) => toggle(c, on)} data-testid={`filter-${c}`} />
                  <Label>
                    <Badge color={COLOR[c]}>{c}</Badge>
                  </Label>
                  <span data-slot="description" className="text-xs text-zinc-500">
                    {LABEL[c]}
                  </span>
                </CheckboxField>
              ))}
            </CheckboxGroup>
          </Fieldset>
          <Field>
            <Label>Dispositivos</Label>
            <Input
              placeholder="todos (p. ej. dummy1,dummy3)"
              value={deviceInput}
              onChange={(e) => setDeviceInput(e.target.value)}
              invalid={!!badDevice}
              data-testid="filter-devices"
            />
          </Field>
          {badDevice && <Callout tone="warning">«{badDevice}» no es un nombre de dispositivo válido.</Callout>}
          {channels.length === 0 && <Callout tone="info">Sin canales marcados se escucha solo «device».</Callout>}
        </div>
        <div className="min-w-0">
          {rows.length === 0 ? (
            <Empty>{paused ? "En pausa." : "Esperando eventos…"}</Empty>
          ) : (
            <Table dense data-testid="events-table">
              <TableHead>
                <TableRow>
                  <TableHeader>Hora</TableHeader>
                  <TableHeader className="text-right">id</TableHeader>
                  <TableHeader>Canal</TableHeader>
                  <TableHeader>Dispositivo</TableHeader>
                  <TableHeader>Datos</TableHeader>
                </TableRow>
              </TableHead>
              <TableBody>
                {rows.map((r) => (
                  <TableRow key={r.key} data-testid="event-row" data-channel={r.event} data-device={r.device ?? ""}>
                    <TableCell className="tabular-nums">{fmtTime(r.at)}</TableCell>
                    <TableCell className="text-right tabular-nums text-zinc-500">{r.id}</TableCell>
                    <TableCell>
                      <Badge color={COLOR[r.event as Channel] ?? "zinc"}>{r.event}</Badge>
                    </TableCell>
                    <TableCell>{r.device ?? "—"}</TableCell>
                    <TableCell className="max-w-md truncate">
                      <Mono className="text-zinc-600">{r.data}</Mono>
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          )}
        </div>
      </div>
    </>
  );
}
