#!/usr/bin/env bash
# End-to-end test: fresh database, both processes, client scenarios.
#
# Usage: tests/e2e.sh          (needs a superuser in PGHOST/PGUSER/PGPASSWORD,
#                               a build in build/, curl and jq)
#
# Scenarios: authentication, role enforcement (HTTP and PostgreSQL), live data
# from both tables, the desired/applied configuration loop through the
# Dispatcher, controller-side rejection, the persistent queue while the
# controller is down, and SSE multiplexing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
C="${ROOT}/client/pgdemo.sh"
WORK="$(mktemp -d)"
export PGDEMO_TOKEN="${WORK}/token"
export PGDEMO_URL="http://127.0.0.1:${PGDEMO_PORT:-8080}"

pids=()
cleanup() {
    for p in "${pids[@]}"; do kill -TERM "${p}" 2>/dev/null || true; done
    for p in "${pids[@]}"; do wait "${p}" 2>/dev/null || true; done
    rm -rf "${WORK}"
}
trap cleanup EXIT

pass=0
fail=0
ok() { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
ko() { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }
check() { local name="$1"; shift; if "$@" >/dev/null 2>&1; then ok "${name}"; else ko "${name}"; fi; }

# http_code METHOD PATH [curl args]: prints only the status code.
http_code() {
    local m="$1" p="$2"
    shift 2
    curl -s -o /dev/null -w '%{http_code}' -X "${m}" "$@" "${PGDEMO_URL}${p}"
}
code_is() { [[ "$(http_code "${@:2}")" == "$1" ]]; }
bearer() { printf '%s' "@${PGDEMO_TOKEN}"; }

start() {
    "${ROOT}/scripts/run-all.sh" "$1" > "${WORK}/$1.out" 2>&1 < /dev/null &
    pids+=("$!")
    eval "pid_$1=$!"
}
wait_health() {
    for _ in $(seq 1 50); do
        code_is 200 GET /api/health && return 0
        sleep 0.2
    done
    return 1
}

echo "== database"
"${ROOT}/scripts/db-init.sh" --reset > "${WORK}/db-init.out" 2>&1 || { cat "${WORK}/db-init.out"; exit 1; }
# shellcheck disable=SC1091
source "${ROOT}/run/demo.env"

echo "== server only (controller down)"
start server
wait_health || { cat "${WORK}/server.out"; exit 1; }
check "health is public" code_is 200 GET /api/health
check "data without token is 401" code_is 401 GET /api/data/dummy1
check "garbage token is 401" code_is 401 GET /api/data/dummy1 -H "Authorization: Bearer x.y.z"
check "wrong password is 401" code_is 401 POST /api/auth/login -H 'Content-Type: application/json' \
    --data '{"username":"admin","password":"wrong-password"}'
check "unknown user is 401" code_is 401 POST /api/auth/login -H 'Content-Type: application/json' \
    --data '{"username":"nobody","password":"wrong-password"}'
check "admin login" env PGDEMO_PASSWORD="${DEMO_ADMIN_PASSWORD}" "${C}" login admin

queued="$("${C}" config set dummy1 rate_ms=300 2>/dev/null || true)"
check "config set without controller is queued (202 PENDING)" \
    test "$(jq -r .status <<<"${queued}")" = PENDING
queued_id="$(jq -r .request_id <<<"${queued}")"

echo "== controller starts and drains the queue"
start controller
sleep 1.5
check "queued request applied on controller start-up" \
    test "$("${C}" config request "${queued_id}" 2>/dev/null | jq -r .status)" = SUCCESS
check "applied rate is the queued one" \
    test "$("${C}" config status 2>/dev/null | jq -r '.[] | select(.target=="dummy1") | .applied_rate_ms')" = 300

echo "== data plane"
sleep 1
check "dummy1 has live rows" test "$("${C}" data dummy1 2>/dev/null | jq length)" -ge 1
check "dummy2 has live rows" test "$("${C}" data dummy2 2>/dev/null | jq length)" -ge 1
check "history rows name their monthly partition" \
    bash -c "'${C}' history dummy1 limit=1 2>/dev/null | jq -e '.[0].partition | test(\"^dummy1_history_y[0-9]{4}m[0-9]{2}\$\")'"
check "unknown table is 400" code_is 400 GET /api/data/dummy3 -H "$(bearer)"

timeout 3 curl -sN -H "$(bearer)" "${PGDEMO_URL}/api/stream?channels=dummy1,dummy2" > "${WORK}/sse.txt" || true
check "SSE carries dummy1 events" grep -q '^event: dummy1' "${WORK}/sse.txt"
check "SSE carries dummy2 events" grep -q '^event: dummy2' "${WORK}/sse.txt"
check "SSE filter excludes config" bash -c "! grep -q '^event: config' '${WORK}/sse.txt'"

echo "== control plane"
r="$("${C}" config set dummy2 rate_ms=400 amplitude=7.5 2>/dev/null)"
check "valid config is applied (SUCCESS)" test "$(jq -r .status <<<"${r}")" = SUCCESS
check "Dispatcher woke on config_events" test "$(jq -r .dispatcher <<<"${r}")" = SUCCESS
r="$("${C}" config set dummy2 rate_ms=5 2>/dev/null || true)"
check "out-of-range rate is rejected by the controller" test "$(jq -r .request.error_code <<<"${r}")" = INVALID_RATE
r="$("${C}" config set dummy2 amplitude=5000 2>/dev/null || true)"
check "out-of-range amplitude is rejected" test "$(jq -r .request.error_code <<<"${r}")" = INVALID_AMPLITUDE
check "sync view shows the error" \
    test "$("${C}" config status 2>/dev/null | jq -r '.[] | select(.target=="dummy2") | .sync')" = ERROR
check "bad body is 400" code_is 400 POST /api/config -H "$(bearer)" --data '{"target":"dummy1","rate_ms":"fast"}'

burst="$("${C}" burst 25 dummy1 2>/dev/null)"
check "burst: 20 applied" grep -q 'HTTP 200 x 20' <<<"${burst}"
check "burst: 5 rejected" grep -q 'HTTP 422 x 5' <<<"${burst}"
check "config history records applied changes" test "$("${C}" config history dummy1 limit=5 2>/dev/null | jq length)" -eq 5

echo "== users and roles"
check "admin creates a normal user" env PGDEMO_NEW_PASSWORD=normal-user-password "${C}" users add alice normal
check "duplicate user is 409" code_is 409 POST /api/users -H "$(bearer)" -H 'Content-Type: application/json' \
    --data '{"username":"alice","password":"another-password","role":"normal"}'
check "invalid username is 422" code_is 422 POST /api/users -H "$(bearer)" -H 'Content-Type: application/json' \
    --data '{"username":"Bad Name!","password":"another-password","role":"normal"}'
check "short password is 400" code_is 400 POST /api/users -H "$(bearer)" -H 'Content-Type: application/json' \
    --data '{"username":"bob","password":"short","role":"normal"}'
check "last admin cannot be deleted" code_is 422 DELETE /api/users/admin -H "$(bearer)"
check "normal user login" env PGDEMO_PASSWORD=normal-user-password "${C}" login alice
check "normal user reads data" code_is 200 GET /api/data/dummy1 -H "$(bearer)"
check "normal user reads config status" code_is 200 GET /api/config -H "$(bearer)"
check "normal user cannot list users (403)" code_is 403 GET /api/users -H "$(bearer)"
check "normal user cannot set config (403)" code_is 403 POST /api/config -H "$(bearer)" --data '{"target":"dummy1"}'
check "me reports demo_normal" test "$("${C}" me 2>/dev/null | jq -r .pg_role)" = demo_normal

echo "== shutdown"
kill -TERM "${pid_server}" "${pid_controller}"
wait "${pid_server}" 2>/dev/null || true
wait "${pid_controller}" 2>/dev/null || true
pids=()
check "server logged a clean listener exit" grep -q 'listener exiting' "${ROOT}/run/logs/server.log"
check "controller logged a clean listener exit" grep -q 'listener exiting' "${ROOT}/run/logs/controller.log"

echo
echo "${pass} passed, ${fail} failed"
[[ "${fail}" -eq 0 ]]
