import { Badge } from "@/components/catalyst/badge";
import type { ComponentProps } from "react";
import type { EnrollmentStatus, Identity, Role, SessionState, SyncState } from "@/lib/types";

type Color = NonNullable<ComponentProps<typeof Badge>["color"]>;
type Spec = Record<string, [label: string, color: Color]>;

/**
 * A badge for a state value from the API. The Spanish label is for people;
 * `data-state` keeps the raw value for the UI tests.
 */
function StateBadge({ spec, value, testId }: { spec: Spec; value: string | null | undefined; testId?: string }) {
  const v = value ?? "";
  const [label, color] = spec[v] ?? [v || "—", "zinc"];
  return (
    <Badge color={color} data-state={v} data-testid={testId}>
      {label}
    </Badge>
  );
}

const IDENTITY: Record<Identity, [string, Color]> = {
  ACTIVE: ["Activa", "lime"],
  SUSPENDED: ["Suspendida", "amber"],
  REVOKED: ["Revocada", "red"],
  CA_INACTIVE: ["CA inactiva", "orange"],
  EXPIRED: ["Caducada", "zinc"],
};

const SESSION: Record<SessionState, [string, Color]> = {
  ONLINE: ["En línea", "green"],
  STARTING: ["Arrancando", "sky"],
  OFFLINE: ["Desconectado", "zinc"],
  UNMANAGED: ["Sin gestionar", "zinc"],
  AUTH_FAILED: ["Fallo de autenticación", "red"],
  DISABLED: ["Deshabilitado", "zinc"],
};

const SYNC: Record<SyncState, [string, Color]> = {
  SYNC: ["Sincronizada", "green"],
  PENDING: ["Pendiente", "amber"],
  ERROR: ["Error", "red"],
  UNINITIALISED: ["Sin inicializar", "zinc"],
};

const ENROLLMENT: Record<EnrollmentStatus, [string, Color]> = {
  PENDING: ["Pendiente", "amber"],
  CSR_SUBMITTED: ["CSR recibida", "sky"],
  ISSUED: ["Emitido", "green"],
  REJECTED: ["Rechazada", "red"],
  EXPIRED: ["Caducada", "zinc"],
};

const CONFIG_REQUEST: Spec = {
  PENDING: ["En cola", "amber"],
  SUCCESS: ["Aplicada", "green"],
  ERROR: ["Rechazada", "red"],
  TIMEOUT: ["Caducada", "zinc"],
};

export function IdentityBadge({ value, testId = "badge-identity" }: { value: Identity | null | undefined; testId?: string }) {
  return <StateBadge spec={IDENTITY} value={value} testId={testId} />;
}

export function SessionBadge({ value, testId = "badge-session" }: { value: SessionState | null | undefined; testId?: string }) {
  return <StateBadge spec={SESSION} value={value ?? "OFFLINE"} testId={testId} />;
}

export function SyncBadge({ value, testId = "badge-sync" }: { value: SyncState | null | undefined; testId?: string }) {
  return <StateBadge spec={SYNC} value={value} testId={testId} />;
}

export function EnrollmentBadge({ value, testId }: { value: EnrollmentStatus | null | undefined; testId?: string }) {
  return <StateBadge spec={ENROLLMENT} value={value} testId={testId} />;
}

export function ConfigRequestBadge({ value, testId }: { value: string | null | undefined; testId?: string }) {
  return <StateBadge spec={CONFIG_REQUEST} value={value} testId={testId} />;
}

/** Admin in amber, the colour of the administration panel; normal in zinc. */
export function RoleBadge({ role, testId = "badge-role" }: { role: Role; testId?: string }) {
  return (
    <Badge color={role === "admin" ? "amber" : "zinc"} data-state={role} data-testid={testId}>
      {role}
    </Badge>
  );
}

/** Whether a revocation is permanent; only certificateHold can be lifted. */
export function RevocationBadge({ reason }: { reason: string | null }) {
  if (!reason) return <span className="text-zinc-400">—</span>;
  return <Badge color={reason === "certificateHold" ? "amber" : reason === "superseded" ? "zinc" : "red"}>{reason}</Badge>;
}
