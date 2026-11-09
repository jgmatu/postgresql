import { api, ApiError, request } from "./api";
import { pollUntil } from "./poll";
import type { EnrollReply, Enrollment } from "./types";

/** End state of an enrollment or rotation, as the admin pages report it. */
export type EnrollResult =
  | { kind: "issued"; enrollment: Enrollment; serial?: string }
  | { kind: "pending"; enrollment: Enrollment }
  | { kind: "rejected"; enrollment: Enrollment }
  | { kind: "conflict"; message: string }
  | { kind: "error"; error: unknown };

const settled = (e: Enrollment) => e.status === "ISSUED" || e.status === "REJECTED" || e.status === "EXPIRED";

/**
 * POST an enrollment (`/api/devices`) or rotation (`/api/devices/{name}/rotate`)
 * and follow it to the end: 201/200 issued, 202 polled on /api/enrollments/{id}
 * until issued, rejected or expired; 409 conflict; 422 refused by the CA.
 */
export async function enrollAndFollow(path: string, body?: unknown, onPending?: (e: Enrollment) => void): Promise<EnrollResult> {
  try {
    const r = await request<EnrollReply>(path, { method: "POST", body: body ?? {} });
    if (r.status !== 202) return { kind: "issued", enrollment: r.data.enrollment, serial: r.data.device?.certificate?.serial };
    onPending?.(r.data.enrollment);
    const last = await pollUntil(() => api<Enrollment>(`/api/enrollments/${r.data.request_id}`), settled);
    if (last.status === "ISSUED") return { kind: "issued", enrollment: last };
    if (settled(last)) return { kind: "rejected", enrollment: last };
    return { kind: "pending", enrollment: last };
  } catch (e) {
    if (e instanceof ApiError && e.status === 409) return { kind: "conflict", message: e.message };
    if (e instanceof ApiError && e.status === 422 && (e.body as EnrollReply)?.enrollment)
      return { kind: "rejected", enrollment: (e.body as EnrollReply).enrollment };
    return { kind: "error", error: e };
  }
}
