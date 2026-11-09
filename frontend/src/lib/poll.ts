/**
 * Call `fetch` every `intervalMs` until `done(value)` holds or `timeoutMs`
 * passes; resolves with the last value either way. Used after a 202, when the
 * request row is the source of truth (`/api/enrollments/{id}`,
 * `/api/config/requests/{id}`).
 */
export async function pollUntil<T>(
  fetch: () => Promise<T>,
  done: (value: T) => boolean,
  { intervalMs = 1000, timeoutMs = 30_000, signal }: { intervalMs?: number; timeoutMs?: number; signal?: AbortSignal } = {},
): Promise<T> {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = await fetch();
    if (done(v) || Date.now() + intervalMs > deadline || signal?.aborted) return v;
    await new Promise((r) => setTimeout(r, intervalMs));
  }
}
