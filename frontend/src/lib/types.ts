/**
 * Shapes of the pgdemo-server JSON API. Timestamps are ISO 8601 strings with
 * offset; nullable columns come through as `null`.
 */

export type Role = "admin" | "normal";

export interface LoginResponse {
  token: string;
  token_type: "Bearer";
  expires_in: number;
  role: Role;
}

export interface Me {
  username: string;
  role: Role;
  pg_role: string;
}

/** Body of every non-2xx JSON reply: `{"error": code, "message": text}`. */
export interface ErrorBody {
  error: string;
  message: string;
}

export interface Health {
  status: "ok" | "degraded";
  listener_alive: boolean;
}

export interface DbInfo {
  postgres_version: string;
  uptime: string;
  database: string;
  user: string;
  queries_backend_pid: number;
  pool_size: number;
  pool_in_use: number;
  query_backend_pids: number[];
  listener_backend_pid: number;
  pqxx_version: string;
  listener_alive: boolean;
  listen_channels: number;
  sse_subscribers: number;
  events_published: number;
  dispatcher_waiters: number;
}

export type Payload = "scalar" | "gps" | "xy" | "xyz";

export interface DeviceType {
  type_id: string;
  payload: Payload;
  default_rate_ms: number;
  default_amplitude: number;
  description: string;
}

/** Identity of the current certificate, from `v_devices`. */
export type Identity = "ACTIVE" | "SUSPENDED" | "REVOKED" | "CA_INACTIVE" | "EXPIRED";
/** State of the device process, from `device_session`. */
export type SessionState = "STARTING" | "ONLINE" | "OFFLINE" | "UNMANAGED" | "AUTH_FAILED" | "DISABLED";

/** One row of `GET /api/devices`. */
export interface DeviceSummary {
  device_id: number;
  device: string;
  type_id: string;
  payload: Payload;
  value: number | null;
  lon: number | null;
  lat: number | null;
  x: number | null;
  y: number | null;
  z: number | null;
  seq: number | null;
  updated_at: string | null;
  created_at: string;
  identity: Identity;
  session: SessionState | null;
}

export type SyncState = "SYNC" | "PENDING" | "ERROR" | "UNINITIALISED";

/** Applied vs desired configuration of one device. */
export interface ConfigSync {
  applied_rate_ms: number | null;
  applied_amplitude: number | null;
  applied_is_active: boolean | null;
  applied_request_id: number | null;
  applied_at: string | null;
  desired_request_id: number | null;
  desired_rate_ms: number | null;
  desired_amplitude: number | null;
  desired_is_active: boolean | null;
  desired_status: string | null;
  requested_by: string | null;
  requested_at: string | null;
  sync: SyncState;
  last_error_code: string | null;
  last_error_detail: string | null;
  last_error_request_id: number | null;
  last_error_at: string | null;
}

/** One row of `GET /api/config`. */
export interface ConfigStatus extends ConfigSync {
  device_id: number;
  device: string;
  type_id: string;
}

export interface DeviceProcess {
  pid: number | null;
  state: SessionState;
  last_seen: string | null;
  last_error: string | null;
  connected_at: string | null;
}

export interface CurrentCertificate {
  ca: string;
  serial: string;
  cert_id: number;
  issued_at: string;
  not_after: string;
  not_before: string;
  revoked_at: string | null;
  fingerprint: string;
  certificates: number;
  ca_fingerprint: string;
  revocation_reason: string | null;
}

/** `GET /api/devices/{name}`. */
export interface DeviceDetail extends Omit<DeviceSummary, "session"> {
  session: SessionState | null;
  config: ConfigSync | null;
  process: DeviceProcess | null;
  certificate: CurrentCertificate | null;
}

/** One row of `GET /api/devices/{name}/history`, newest first. */
export interface TelemetryRow {
  history_id: number;
  device_id: number;
  value?: number;
  lon?: number;
  lat?: number;
  x?: number;
  y?: number;
  z?: number;
  seq: number;
  recorded_at: string;
  partition: string;
}

export interface Certificate {
  cert_id: number;
  serial: string;
  fingerprint: string;
  subject_cn: string;
  ca: string;
  not_before: string;
  not_after: string;
  issued_at: string;
  current: boolean;
  revocation_reason: string | null;
  revoked_at: string | null;
  revoked_by: string | null;
}

export type EnrollmentKind = "ENROLL" | "ROTATE";
export type EnrollmentStatus = "PENDING" | "CSR_SUBMITTED" | "ISSUED" | "REJECTED" | "EXPIRED";

export interface Enrollment {
  request_id: number;
  kind: EnrollmentKind;
  device: string;
  type_id: string | null;
  device_id: number | null;
  status: EnrollmentStatus;
  cert_id: number | null;
  error_code: string | null;
  error_detail: string | null;
  requested_by: string;
  requested_at: string;
  csr_at: string | null;
  decided_at: string | null;
}

/** Reply of `POST /api/devices` and `POST /api/devices/{name}/rotate` (201/200, 202, 422). */
export interface EnrollReply {
  request_id: number;
  status: EnrollmentStatus;
  dispatcher: string;
  enrollment: Enrollment;
  device?: DeviceDetail;
  message?: string;
}

export interface RootCa {
  ca_id: number;
  name: string;
  subject: string;
  fingerprint: string;
  not_before: string;
  not_after: string;
  is_active: boolean;
  created_at: string;
  cert_pem: string;
}

export interface CrlEntry {
  serial: string;
  fingerprint: string;
  device: string;
  ca: string;
  reason: string;
  revoked_at: string;
  revoked_by: string | null;
}

/** `GET /api/pki/ca`. */
export interface PkiCa {
  signing_fingerprint: string;
  roots: RootCa[];
  revoked: CrlEntry[];
}

/** One public certificate from `GET /api/pki/origin`. */
export interface OriginCert {
  name?: string;
  subject?: string;
  subject_cn?: string;
  fingerprint: string;
  serial?: string;
  not_before: string;
  not_after: string;
  cert_pem: string;
  device?: string;
  current?: boolean;
}

/** `GET /api/pki/origin`: factory root, manager certificate and factory-issued workers. */
export interface PkiOrigin {
  factory: OriginCert | null;
  manager: OriginCert | null;
  workers: OriginCert[];
}

export interface ConfigHistoryRow {
  history_id: number;
  device_id: number;
  device: string | null;
  rate_ms: number;
  amplitude: number;
  is_active: boolean;
  request_id: number | null;
  recorded_at: string;
}

export interface ConfigRequest {
  request_id: number;
  device_id: number;
  status: "PENDING" | "SUCCESS" | "ERROR" | "TIMEOUT";
  [column: string]: unknown;
}

/** Reply of `POST /api/config` (200, 202, 422). */
export interface ConfigReply {
  request_id: number;
  status: ConfigRequest["status"];
  dispatcher: string;
  request: ConfigRequest;
  message?: string;
}

export interface ConfigSubmit {
  device: string;
  rate_ms?: number;
  amplitude?: number;
  is_active?: boolean;
}

export interface User {
  user_id: number;
  username: string;
  role: Role;
  created_at: string;
  updated_at: string;
}

export const REVOCATION_REASONS = [
  "unspecified",
  "keyCompromise",
  "affiliationChanged",
  "cessationOfOperation",
  "certificateHold",
] as const;
export type RevocationReason = (typeof REVOCATION_REASONS)[number];

/** SSE channels of `GET /api/stream`, as named by `EventHub`. */
export const CHANNELS = ["telemetry_scalar", "telemetry_gps", "telemetry_xy", "telemetry_xyz", "device", "config", "error", "enrollment"] as const;
export type Channel = (typeof CHANNELS)[number];

export interface ScalarEvent {
  device_id: number;
  device: string;
  action: string;
  value: number;
  seq: number;
  ts: string;
}

export interface GpsEvent {
  device_id: number;
  device: string;
  action: string;
  lon: number;
  lat: number;
  seq: number;
  ts: string;
}

export interface XyEvent {
  device_id: number;
  device: string;
  action: string;
  x: number;
  y: number;
  seq: number;
  ts: string;
}

export interface XyzEvent {
  device_id: number;
  device: string;
  action: string;
  x: number;
  y: number;
  z: number;
  seq: number;
  ts: string;
}

/** `device` channel: INSERT, DELETE, ROTATED, REVOKED, UNREVOKED or a session state. */
export interface DeviceEvent {
  action: string;
  device_id: number | null;
  device: string | null;
  [field: string]: unknown;
}
