#!/usr/bin/env bash
# Start the controller and the server against the database set up by db-init.sh.
#
# Usage: scripts/run-all.sh [--https|--http] [server|controller|all]
#
#   --https   HTTPS, TLS 1.3 only, on port 8443 (default)
#   --http    plain HTTP on port 8080
#
# TLS 1.3 and the key-exchange groups come from build/server/openssl-oqs.cnf,
# which pgdemo-server loads before oatpp-openssl creates its SSL_CTX.
#
# Each process logs in with its own PostgreSQL role (demo_server, demo_controller)
# and knows nothing about the other one. Logs go to run/logs/. Ctrl-C stops both.
#
# Variables (defaults from run/demo.env):
#   PGDEMO_PGHOST, PGDEMO_PGPORT, PGDEMO_DB   database location
#   PGDEMO_LISTEN_ADDR (127.0.0.1)            bind address (Docker uses 0.0.0.0)
#   PGDEMO_SERVER_ARGS, PGDEMO_CONTROLLER_ARGS  extra command-line options
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${PGDEMO_ENV_FILE:-${ROOT}/run/demo.env}"
BUILD="${PGDEMO_BUILD_DIR:-${ROOT}/build}"
SCHEME=https
WHAT=all
while [[ $# -gt 0 ]]; do
    case "$1" in
        --https) SCHEME=https ;;
        --http) SCHEME=http ;;
        -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        server|controller|all) WHAT="$1" ;;
        *) printf '[run-all] ERROR: unknown argument %s\n' "$1" >&2; exit 1 ;;
    esac
    shift
done

die() { printf '[run-all] ERROR: %s\n' "$*" >&2; exit 1; }
[[ -f "${ENV_FILE}" ]] || die "${ENV_FILE} not found; run scripts/db-init.sh first"
# shellcheck disable=SC1090
source "${ENV_FILE}"

case "${WHAT}" in server|controller|all) ;; *) die "unknown target '${WHAT}'" ;; esac

PGHOST_="${PGDEMO_PGHOST:-127.0.0.1}"
PGPORT_="${PGDEMO_PGPORT:-5432}"
DB_="${PGDEMO_DB:-pgdemo}"
LOGS="${ROOT}/run/logs"
mkdir -p "${LOGS}"

pids=()
logs=()
tail_pid=""
cleanup() {
    trap - INT TERM EXIT
    for pid in "${pids[@]}"; do kill -TERM "${pid}" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "${pid}" 2>/dev/null || true; done
    sleep 0.2
    [[ -n "${tail_pid}" ]] && kill "${tail_pid}" 2>/dev/null || true
}
trap cleanup INT TERM EXIT

start_controller() {
    [[ -x "${BUILD}/controller/pgdemo-controller" ]] || die "build first: cmake --build build"
    : > "${LOGS}/controller.log"
    logs+=("${LOGS}/controller.log")
    # shellcheck disable=SC2086
    PGHOST="${PGHOST_}" PGPORT="${PGPORT_}" PGDATABASE="${DB_}" \
    PGUSER=demo_controller PGPASSWORD="${DEMO_CONTROLLER_DB_PASSWORD}" \
        "${BUILD}/controller/pgdemo-controller" ${PGDEMO_CONTROLLER_ARGS:-} \
        >> "${LOGS}/controller.log" 2>&1 &
    pids+=("$!")
    echo "[run-all] controller pid $! (log run/logs/controller.log)"
}

start_server() {
    [[ -x "${BUILD}/server/pgdemo-server" ]] || die "build first: cmake --build build"
    "${ROOT}/scripts/gen-jwt-keys.sh" >/dev/null
    local -a tls_args=()
    local port=8080 mode="plain HTTP"
    if [[ "${SCHEME}" == https ]]; then
        "${ROOT}/scripts/gen-tls-cert.sh" >/dev/null
        tls_args=(--tls-key "${ROOT}/run/keys/tls.key" --tls-cert "${ROOT}/run/keys/tls.crt")
        port=8443
        mode="TLS 1.3, openssl-oqs.cnf"
    fi
    local addr="${PGDEMO_LISTEN_ADDR:-127.0.0.1}"
    : > "${LOGS}/server.log"
    logs+=("${LOGS}/server.log")
    # shellcheck disable=SC2086
    PGHOST="${PGHOST_}" PGPORT="${PGPORT_}" PGDATABASE="${DB_}" \
    PGUSER=demo_server PGPASSWORD="${DEMO_SERVER_DB_PASSWORD}" \
        "${BUILD}/server/pgdemo-server" \
        --listen-addr "${addr}" --port "${port}" \
        --jwt-key "${ROOT}/run/keys/jwt.key" --jwt-cert "${ROOT}/run/keys/jwt.crt" \
        "${tls_args[@]}" \
        ${PGDEMO_SERVER_ARGS:-} \
        >> "${LOGS}/server.log" 2>&1 &
    pids+=("$!")
    echo "[run-all] server pid $! on ${SCHEME}://${addr}:${port} (${mode}; log run/logs/server.log)"
}

if [[ "${WHAT}" == controller || "${WHAT}" == all ]]; then start_controller; fi
if [[ "${WHAT}" == server || "${WHAT}" == all ]]; then start_server; fi

# Echo the logs to the console. Not in pids: its exit does not stop the demo.
tail -n +1 -q -F "${logs[@]}" 2>/dev/null &
tail_pid=$!

echo "[run-all] Ctrl-C to stop"
# Exit as soon as either process ends; the trap stops the other one.
wait -n "${pids[@]}"
