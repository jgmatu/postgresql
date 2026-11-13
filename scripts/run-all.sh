#!/usr/bin/env bash
# Start the operational stack: PostgreSQL, the server and the controller.
#
# Usage: scripts/run-all.sh [server|controller|all]
#
#   server      the API, and the database and init it depends on
#   controller  the manager, and the database and init it depends on
#   all         the whole stack (default)
#
# The server listens on HTTPS inside the container. Compose publishes it on
# 127.0.0.1:8080. Ctrl-C stops the attached containers. scripts/stop-all.sh
# stops them from another terminal. Volumes survive both.
#
# The end-to-end suite uses docker/e2e and is not this script.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${ROOT}/docker/operational/compose.yaml"
ENV_FILE="${ROOT}/docker/operational/.env"
WHAT=all

while [[ $# -gt 0 ]]; do
    case "$1" in
        --https) ;;
        --http)
            printf '[run-all] ERROR: the operational stack is HTTPS\n' >&2
            exit 1
            ;;
        -h|--help) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        server|controller|all) WHAT="$1" ;;
        *) printf '[run-all] ERROR: unknown argument %s\n' "$1" >&2; exit 1 ;;
    esac
    shift
done

if [[ ! -f "${ENV_FILE}" ]]; then
    printf '[run-all] ERROR: %s not found; copy docker/operational/.env.example and set both passwords\n' "${ENV_FILE}" >&2
    exit 1
fi

args=(docker compose -f "${COMPOSE_FILE}" --env-file "${ENV_FILE}" up --build)
if [[ "${WHAT}" != all ]]; then
    args+=("${WHAT}")
fi
exec "${args[@]}"
