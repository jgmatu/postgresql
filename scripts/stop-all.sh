#!/usr/bin/env bash
# Stop the operational stack. Volumes stay. Device processes die with the
# controller container. PostgreSQL stops only with `all`, and its data volume
# remains.
#
# Usage: scripts/stop-all.sh [server|controller|all]
#
#   server      the API only
#   controller  the manager and its device processes
#   all         the whole project (default)
#
# The end-to-end project pgdemo-e2e is not this script.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${ROOT}/docker/operational/compose.yaml"
ENV_FILE="${ROOT}/docker/operational/.env"
WHAT="${1:-all}"

case "${WHAT}" in
    -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    server|controller|all) ;;
    *) printf '[stop-all] ERROR: unknown target %s\n' "${WHAT}" >&2; exit 1 ;;
esac

if [[ ! -f "${ENV_FILE}" ]]; then
    printf '[stop-all] ERROR: %s not found; copy docker/operational/.env.example and set both passwords\n' "${ENV_FILE}" >&2
    exit 1
fi

args=(docker compose -f "${COMPOSE_FILE}" --env-file "${ENV_FILE}" stop)
if [[ "${WHAT}" != all ]]; then
    args+=("${WHAT}")
fi
exec "${args[@]}"
