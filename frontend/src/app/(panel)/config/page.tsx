"use client";

import { Badge } from "@/components/catalyst/badge";
import { Button } from "@/components/catalyst/button";
import { DescriptionDetails, DescriptionList, DescriptionTerm } from "@/components/catalyst/description-list";
import { Field, Label } from "@/components/catalyst/fieldset";
import { Input } from "@/components/catalyst/input";
import { Link } from "@/components/catalyst/link";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { ConfigRequestBadge, SyncBadge } from "@/components/badges";
import { Callout, Empty, ErrorCallout, LiveIndicator, Loading, Mono, PageHeading, Section } from "@/components/ui";
import { ApiError, api } from "@/lib/api";
import { fmtDateTime, fmtNumber } from "@/lib/format";
import { useApi, useSession, useStream } from "@/lib/hooks";
import type { ConfigHistoryRow, ConfigRequest, ConfigStatus } from "@/lib/types";
import { MagnifyingGlassIcon, WrenchScrewdriverIcon } from "@heroicons/react/16/solid";
import { Fragment, useCallback, useState, type FormEvent } from "react";

function cfg(rate: number | null, amp: number | null, active: boolean | null): string {
  if (rate === null) return "—";
  return `${rate} ms · ${fmtNumber(amp, 2)} · ${active ? "activo" : "pausado"}`;
}

/** One request by id: what was asked, by whom, and how the controller answered. */
function RequestLookup() {
  const [id, setId] = useState("");
  const [busy, setBusy] = useState(false);
  const [found, setFound] = useState<ConfigRequest | null>(null);
  const [error, setError] = useState<unknown>(null);

  async function find(e: FormEvent) {
    e.preventDefault();
    setBusy(true);
    setFound(null);
    setError(null);
    try {
      setFound(await api<ConfigRequest>(`/api/config/requests/${encodeURIComponent(id.trim())}`));
    } catch (err) {
      setError(err);
    } finally {
      setBusy(false);
    }
  }

  const shown = found
    ? Object.entries(found).filter(([k, v]) => v !== null && v !== undefined && !["request_id", "status"].includes(k))
    : [];
  return (
    <Section title="Buscar una petición" testId="section-request-lookup">
      <form onSubmit={find} className="flex max-w-md items-end gap-3" data-testid="request-lookup">
        <Field className="flex-1">
          <Label>Número de petición</Label>
          <Input type="number" min={1} value={id} onChange={(e) => setId(e.target.value)} required data-testid="request-id" />
        </Field>
        <Button type="submit" outline disabled={busy} data-testid="request-find">
          <MagnifyingGlassIcon />
          Buscar
        </Button>
      </form>
      <div className="mt-4 max-w-2xl">
        {error !== null &&
          (error instanceof ApiError && error.status === 404 ? (
            <Callout tone="info" testId="request-not-found">
              No existe la petición #{id}; las de un dispositivo eliminado se borran con él.
            </Callout>
          ) : (
            <ErrorCallout error={error} testId="request-error" />
          ))}
        {found && (
          <div className="rounded-lg p-4 ring-1 ring-zinc-950/10" data-testid="request-result" data-state={found.status}>
            <div className="flex items-center gap-3 font-semibold text-zinc-950">
              Petición #{found.request_id} <ConfigRequestBadge value={found.status} />
            </div>
            <DescriptionList className="mt-2">
              {shown.map(([k, v]) => (
                <Fragment key={k}>
                  <DescriptionTerm>
                    <Mono>{k}</Mono>
                  </DescriptionTerm>
                  <DescriptionDetails className="tabular-nums">{typeof v === "object" ? JSON.stringify(v) : String(v)}</DescriptionDetails>
                </Fragment>
              ))}
            </DescriptionList>
          </div>
        )}
      </div>
    </Section>
  );
}

export default function ConfigPage() {
  const session = useSession();
  const status = useApi<ConfigStatus[]>("/api/config");
  const history = useApi<ConfigHistoryRow[]>("/api/config/history?limit=50");
  const reloadStatus = status.reload;
  const reloadHistory = history.reload;
  const onEvent = useCallback(() => {
    reloadStatus();
    reloadHistory();
  }, [reloadStatus, reloadHistory]);
  const stream = useStream({ channels: ["config", "error"] }, onEvent);

  return (
    <>
      <PageHeading
        title="Configuración"
        description="Configuración aplicada por el controlador frente a la deseada (v_device_config_sync_status). Los cambios se piden en cola y el controlador los valida."
        testId="page-config"
        actions={
          <>
            <LiveIndicator {...stream} />
            {session?.role === "admin" && (
              <Button href="/admin/config/" color="amber" data-testid="manage-config">
                <WrenchScrewdriverIcon />
                Gestionar
              </Button>
            )}
          </>
        }
      />
      <div className="mt-8">
        <ErrorCallout error={status.error} />
        {status.loading && !status.data ? (
          <Loading />
        ) : (
          <Table data-testid="config-table">
            <TableHead>
              <TableRow>
                <TableHeader>Dispositivo</TableHeader>
                <TableHeader>Aplicada (rate · amplitud · estado)</TableHeader>
                <TableHeader>Deseada</TableHeader>
                <TableHeader>Estado</TableHeader>
                <TableHeader>Último error</TableHeader>
              </TableRow>
            </TableHead>
            <TableBody>
              {(status.data ?? []).map((c) => (
                <TableRow key={c.device_id} data-testid={`config-row-${c.device}`}>
                  <TableCell className="font-medium">
                    <Link href={`/device/?name=${encodeURIComponent(c.device)}`} className="text-sky-700 hover:underline">
                      {c.device}
                    </Link>
                    <div className="text-xs text-zinc-500">{c.type_id}</div>
                  </TableCell>
                  <TableCell className="tabular-nums">
                    {cfg(c.applied_rate_ms, c.applied_amplitude, c.applied_is_active)}
                    <div className="text-xs text-zinc-500">{fmtDateTime(c.applied_at)}</div>
                  </TableCell>
                  <TableCell className="tabular-nums">
                    {c.desired_request_id ? (
                      <>
                        {cfg(c.desired_rate_ms, c.desired_amplitude, c.desired_is_active)}
                        <div className="text-xs text-zinc-500" data-request-id={c.desired_request_id}>
                          #{c.desired_request_id} · {c.requested_by} · {fmtDateTime(c.requested_at)}
                        </div>
                      </>
                    ) : (
                      <span className="text-zinc-400">—</span>
                    )}
                  </TableCell>
                  <TableCell>
                    <SyncBadge value={c.sync} />
                  </TableCell>
                  <TableCell className="max-w-xs whitespace-normal">
                    {c.last_error_code ? (
                      <>
                        <Badge color="red">{c.last_error_code}</Badge>
                        <div className="mt-1 text-xs text-zinc-500" data-request-id={c.last_error_request_id ?? undefined}>
                          #{c.last_error_request_id} · {c.last_error_detail}
                        </div>
                      </>
                    ) : (
                      <span className="text-zinc-400">—</span>
                    )}
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        )}
      </div>

      <RequestLookup />

      <Section title="Historial de configuraciones aplicadas" testId="section-config-history">
        <ErrorCallout error={history.error} />
        {(history.data ?? []).length === 0 ? (
          <Empty>Sin historial.</Empty>
        ) : (
          <Table dense data-testid="config-history-table">
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
              {(history.data ?? []).map((h) => (
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
    </>
  );
}
