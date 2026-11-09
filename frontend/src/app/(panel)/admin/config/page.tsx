"use client";

import { Button } from "@/components/catalyst/button";
import { Description, Field, FieldGroup, Fieldset, Label, Legend } from "@/components/catalyst/fieldset";
import { Input } from "@/components/catalyst/input";
import { Select } from "@/components/catalyst/select";
import { Switch, SwitchField } from "@/components/catalyst/switch";
import { Text, TextLink } from "@/components/catalyst/text";
import { SyncBadge } from "@/components/badges";
import { OutcomeCallout, type Outcome } from "@/components/Outcome";
import { ErrorCallout, Loading, PageHeading } from "@/components/ui";
import { ApiError, api, describeError, request } from "@/lib/api";
import { fmtNumber } from "@/lib/format";
import { useApi, useStream } from "@/lib/hooks";
import { pollUntil } from "@/lib/poll";
import type { ConfigReply, ConfigRequest, ConfigStatus, ConfigSubmit } from "@/lib/types";
import { PaperAirplaneIcon } from "@heroicons/react/16/solid";
import { useRouter, useSearchParams } from "next/navigation";
import { Suspense, useCallback, useState, type FormEvent } from "react";

function rejection(r: ConfigRequest): string {
  return `${String(r.error_code ?? r.status)}: ${String(r.error_detail ?? "")}`;
}

/** POST /api/config and report every documented reply. */
async function submitConfig(body: ConfigSubmit): Promise<Outcome> {
  try {
    const r = await request<ConfigReply>("/api/config", { method: "POST", body });
    if (r.status === 200) return { state: "applied", tone: "success", title: `Configuración de ${body.device} aplicada`, detail: `Petición #${r.data.request_id}.` };
    const last = await pollUntil(
      () => api<ConfigRequest>(`/api/config/requests/${r.data.request_id}`),
      (q) => q.status !== "PENDING",
    );
    if (last.status === "SUCCESS") return { state: "applied", tone: "success", title: `Configuración de ${body.device} aplicada`, detail: `Petición #${last.request_id}, tras esperar en cola.` };
    if (last.status === "PENDING")
      return { state: "queued", tone: "warning", title: "En cola", detail: `La petición #${last.request_id} se aplicará cuando el controlador vacíe la cola.` };
    return { state: "rejected", tone: "error", title: "Rechazada por el controlador", detail: rejection(last) };
  } catch (e) {
    if (e instanceof ApiError) {
      const reply = e.body as Partial<ConfigReply> | null;
      if (e.status === 422 && reply?.request) return { state: "rejected", tone: "error", title: "Rechazada por el controlador", detail: rejection(reply.request) };
      if (e.status === 409) return { state: "conflict", tone: "warning", title: "Dispositivo sin inicializar", detail: e.message };
      if (e.status === 503) return { state: "busy", tone: "warning", title: "Demasiadas peticiones en curso", detail: "Prueba de nuevo en unos segundos." };
    }
    return { state: "error", tone: "error", title: "No se pudo enviar", detail: describeError(e) };
  }
}

function ConfigForm({ row }: { row: ConfigStatus }) {
  const [rate, setRate] = useState(String(row.applied_rate_ms ?? ""));
  const [amplitude, setAmplitude] = useState(String(row.applied_amplitude ?? ""));
  const [active, setActive] = useState(row.applied_is_active ?? true);
  const [busy, setBusy] = useState(false);
  const [outcome, setOutcome] = useState<Outcome | null>(null);

  async function submit(e: FormEvent) {
    e.preventDefault();
    const body: ConfigSubmit = { device: row.device, is_active: active };
    // The server only checks types; ranges are the controller's decision, so
    // out-of-range values go through and come back as a 422 with the reason.
    if (rate !== "") body.rate_ms = Math.trunc(Number(rate));
    if (amplitude !== "") body.amplitude = Number(amplitude);
    setBusy(true);
    setOutcome({ state: "sending", tone: "info", title: "Enviando…" });
    setOutcome(await submitConfig(body));
    setBusy(false);
  }

  return (
    <form onSubmit={submit} className="max-w-xl" data-testid="config-form">
      <OutcomeCallout outcome={outcome} testId="config-result" />
      <Fieldset>
        <Legend className="flex items-center gap-3">
          {row.device} <SyncBadge value={row.sync} />
        </Legend>
        <Text>
          Aplicada: {row.applied_rate_ms ?? "—"} ms · amplitud {fmtNumber(row.applied_amplitude, 2)} · {row.applied_is_active ? "activo" : "pausado"}.
        </Text>
        <FieldGroup>
          <Field>
            <Label>Periodo de muestreo (rate_ms)</Label>
            <Input type="number" value={rate} onChange={(e) => setRate(e.target.value)} data-testid="config-rate" />
            <Description>El controlador acepta de 50 a 10000 ms; fuera de rango responde INVALID_RATE.</Description>
          </Field>
          <Field>
            <Label>Amplitud</Label>
            <Input type="number" step="any" value={amplitude} onChange={(e) => setAmplitude(e.target.value)} data-testid="config-amplitude" />
          </Field>
          <SwitchField>
            <Label>Activo</Label>
            <Description>Pausado, el proceso sigue conectado pero deja de enviar muestras.</Description>
            <Switch checked={active} onChange={setActive} color="amber" data-testid="config-active" />
          </SwitchField>
        </FieldGroup>
      </Fieldset>
      <div className="mt-8 flex items-center gap-4">
        <Button type="submit" color="amber" disabled={busy} data-testid="config-submit">
          <PaperAirplaneIcon />
          {busy ? "Esperando al controlador…" : "Enviar"}
        </Button>
        <TextLink href={`/device/?name=${encodeURIComponent(row.device)}`}>Ver el dispositivo</TextLink>
      </div>
    </form>
  );
}

function AdminConfigView() {
  const router = useRouter();
  const device = useSearchParams().get("device");
  const status = useApi<ConfigStatus[]>("/api/config");
  const { reload } = status;
  useStream({ channels: ["config", "error"] }, useCallback(() => reload(), [reload]));
  const rows = status.data ?? [];
  const row = rows.find((r) => r.device === device) ?? null;

  return (
    <>
      <PageHeading
        title="Cambiar configuración"
        description="POST /api/config: 200 aplicada, 202 en cola, 409 sin inicializar, 422 rechazada por el controlador, 503 saturado."
        testId="page-admin-config"
      />
      <div className="mt-6">
        <ErrorCallout error={status.error} />
        <Field className="mb-8 max-w-sm">
          <Label>Dispositivo</Label>
          <Select value={device ?? ""} onChange={(e) => router.replace(`/admin/config/?device=${encodeURIComponent(e.target.value)}`)} data-testid="config-device">
            <option value="" disabled>
              Elige un dispositivo…
            </option>
            {rows.map((r) => (
              <option key={r.device} value={r.device}>
                {r.device}
              </option>
            ))}
          </Select>
        </Field>
        {status.loading && !status.data ? <Loading /> : row ? <ConfigForm key={row.device} row={row} /> : null}
      </div>
    </>
  );
}

export default function AdminConfigPage() {
  return (
    <Suspense fallback={<Loading />}>
      <AdminConfigView />
    </Suspense>
  );
}
