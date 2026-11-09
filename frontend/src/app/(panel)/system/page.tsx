"use client";

import { Badge } from "@/components/catalyst/badge";
import { DescriptionDetails, DescriptionList, DescriptionTerm } from "@/components/catalyst/description-list";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/catalyst/table";
import { RoleBadge } from "@/components/badges";
import { ErrorCallout, Loading, Mono, PageHeading, Section, Stat } from "@/components/ui";
import { fmtNumber } from "@/lib/format";
import { useApi, useSession } from "@/lib/hooks";
import type { DbInfo, DeviceType, Health } from "@/lib/types";
import { useEffect, useState } from "react";

/** webroot/VERSION, written by scripts/export-webroot.sh; absent under `next dev`. */
function usePanelVersion(): Record<string, string> | null {
  const [v, setV] = useState<Record<string, string> | null>(null);
  useEffect(() => {
    fetch("/VERSION", { cache: "no-store" })
      .then((r) => (r.ok && !(r.headers.get("content-type") ?? "").includes("html") ? r.text() : ""))
      .then((t) =>
        setV(Object.fromEntries(t.split("\n").flatMap((l) => (l.includes("=") ? [l.split("=", 2) as [string, string]] : [])))),
      )
      .catch(() => setV({}));
  }, []);
  return v;
}

export default function SystemPage() {
  const session = useSession();
  const health = useApi<Health>("/api/health", 5000);
  const info = useApi<DbInfo>("/api/db/info", 5000);
  const types = useApi<DeviceType[]>("/api/device-types");
  const version = usePanelVersion();
  const i = info.data;

  return (
    <>
      <PageHeading
        title="Sistema"
        description="Estado del servidor, de PostgreSQL y del listener LISTEN/NOTIFY. Se actualiza cada 5 s."
        testId="page-system"
        actions={
          health.data && (
            <Badge color={health.data.status === "ok" ? "green" : "red"} data-testid="badge-health" data-state={health.data.status}>
              {health.data.status === "ok" ? "Servidor OK" : "Degradado"}
            </Badge>
          )
        }
      />
      <div className="mt-6 space-y-4">
        <ErrorCallout error={health.error} />
        <ErrorCallout error={info.error} />
      </div>
      {!i ? (
        <Loading />
      ) : (
        <>
          <div className="mt-6 grid grid-cols-2 gap-4 lg:grid-cols-4">
            <Stat label="Pool en uso" value={`${i.pool_in_use} / ${i.pool_size}`} testId="stat-pool" />
            <Stat label="Suscriptores SSE" value={i.sse_subscribers} testId="stat-sse" />
            <Stat label="Eventos publicados" value={fmtNumber(i.events_published, 0)} />
            <Stat label="Esperas del Dispatcher" value={i.dispatcher_waiters} />
          </div>
          <div className="grid gap-x-10 lg:grid-cols-2">
            <Section title="PostgreSQL" testId="section-postgres">
              <DescriptionList>
                <DescriptionTerm>Versión</DescriptionTerm>
                <DescriptionDetails data-testid="pg-version">{i.postgres_version.split(" on ")[0]}</DescriptionDetails>
                <DescriptionTerm>Base de datos</DescriptionTerm>
                <DescriptionDetails>{i.database}</DescriptionDetails>
                <DescriptionTerm>Usuario del servidor</DescriptionTerm>
                <DescriptionDetails>
                  <Mono>{i.user}</Mono>
                </DescriptionDetails>
                <DescriptionTerm>En marcha desde hace</DescriptionTerm>
                <DescriptionDetails>{i.uptime.replace(/\.\d+$/, "")}</DescriptionDetails>
                <DescriptionTerm>libpqxx</DescriptionTerm>
                <DescriptionDetails>{i.pqxx_version}</DescriptionDetails>
              </DescriptionList>
            </Section>
            <Section title="Listener y pool" testId="section-listener">
              <DescriptionList>
                <DescriptionTerm>Listener</DescriptionTerm>
                <DescriptionDetails>
                  <Badge color={i.listener_alive ? "green" : "red"} data-testid="badge-listener" data-state={String(i.listener_alive)}>
                    {i.listener_alive ? "vivo" : "caído"}
                  </Badge>{" "}
                  PID {i.listener_backend_pid}
                </DescriptionDetails>
                <DescriptionTerm>Canales LISTEN</DescriptionTerm>
                <DescriptionDetails>{i.listen_channels}</DescriptionDetails>
                <DescriptionTerm>Backends del pool</DescriptionTerm>
                <DescriptionDetails>
                  <Mono>{i.query_backend_pids.join(", ")}</Mono>
                </DescriptionDetails>
              </DescriptionList>
            </Section>
          </div>
        </>
      )}

      <div className="grid gap-x-10 lg:grid-cols-2">
        <Section title="Tipos de dispositivo" testId="section-types">
          <Table dense data-testid="types-table">
            <TableHead>
              <TableRow>
                <TableHeader>Tipo</TableHeader>
                <TableHeader>Carga</TableHeader>
                <TableHeader className="text-right">rate_ms</TableHeader>
                <TableHeader className="text-right">Amplitud</TableHeader>
                <TableHeader>Descripción</TableHeader>
              </TableRow>
            </TableHead>
            <TableBody>
              {(types.data ?? []).map((t) => (
                <TableRow key={t.type_id}>
                  <TableCell className="font-medium">{t.type_id}</TableCell>
                  <TableCell>{t.payload}</TableCell>
                  <TableCell className="text-right tabular-nums">{t.default_rate_ms}</TableCell>
                  <TableCell className="text-right tabular-nums">{fmtNumber(t.default_amplitude, 0)}</TableCell>
                  <TableCell className="whitespace-normal text-zinc-600">{t.description}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </Section>
        <Section title="Sesión y panel" testId="section-session">
          <DescriptionList>
            <DescriptionTerm>Usuario</DescriptionTerm>
            <DescriptionDetails>{session?.username}</DescriptionDetails>
            <DescriptionTerm>Rol</DescriptionTerm>
            <DescriptionDetails>{session && <RoleBadge role={session.role} testId="system-role" />}</DescriptionDetails>
            <DescriptionTerm>Grupo PostgreSQL</DescriptionTerm>
            <DescriptionDetails>
              <Mono>{session?.pgRole ?? "—"}</Mono>
            </DescriptionDetails>
            <DescriptionTerm>Token caduca</DescriptionTerm>
            <DescriptionDetails>{session ? new Date(session.expiresAt).toLocaleTimeString("es-ES") : "—"}</DescriptionDetails>
            <DescriptionTerm>Versión del panel</DescriptionTerm>
            <DescriptionDetails data-testid="panel-version">
              {version === null ? "…" : version.commit ? `${version.commit} · ${version.built_at}` : "desarrollo (next dev)"}
            </DescriptionDetails>
          </DescriptionList>
        </Section>
      </div>
    </>
  );
}
