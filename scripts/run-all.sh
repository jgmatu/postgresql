#!/usr/bin/env bash
# Start the operational stack: PostgreSQL, SeaweedFS, the server and the controller.
#
# Usage: scripts/run-all.sh [server|controller|all]
#        scripts/run-all.sh --factory-reset
#
#   server           the API, and the database and init it depends on
#   controller       the manager, and the database and init it depends on
#   all              the whole stack (default)
#   --factory-reset  drop and recreate the pgdemo database; volumes stay
#
# The server listens on HTTPS inside the container. Compose publishes it on
# 127.0.0.1:8080. Ctrl-C stops the attached containers. scripts/stop-all.sh
# stops them from another terminal. The volumes are external, so stop, down
# and down -v leave the database and the keys in place. --factory-reset is
# the one command that drops the database; it still leaves every volume.
#
# The end-to-end suite uses docker/e2e and is not this script.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${ROOT}/docker/operational/compose.yaml"
ENV_FILE="${ROOT}/docker/operational/.env"
WHAT=all
RESET=0
TARGET_SET=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --https) ;;
        --http)
            printf '[run-all] ERROR: the operational stack is HTTPS\n' >&2
            exit 1
            ;;
        --factory-reset) RESET=1 ;;
        -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        server|controller|all) WHAT="$1"; TARGET_SET=1 ;;
        *) printf '[run-all] ERROR: unknown argument %s\n' "$1" >&2; exit 1 ;;
    esac
    shift
done

if (( RESET && TARGET_SET )); then
    printf '[run-all] ERROR: --factory-reset does not take a service\n' >&2
    exit 1
fi

if [[ ! -f "${ENV_FILE}" ]]; then
    printf '[run-all] ERROR: %s not found; copy docker/operational/.env.example and set both passwords\n' "${ENV_FILE}" >&2
    exit 1
fi
# shellcheck disable=SC1090
set -a
source "${ENV_FILE}"
set +a
if [[ -z "${POSTGRES_PASSWORD:-}" || -z "${DEMO_ADMIN_PASSWORD:-}" ]]; then
    printf '[run-all] ERROR: %s must set POSTGRES_PASSWORD and DEMO_ADMIN_PASSWORD\n' "${ENV_FILE}" >&2
    printf '[run-all] copy docker/operational/.env.example. This file is not run/demo.env\n' >&2
    exit 1
fi

for volume in pgdemo_pgdata pgdemo_demo-run pgdemo_ca-keys pgdemo_device-keys \
             pgdemo_factory-key pgdemo_factory-cert pgdemo_manager-keys \
             pgdemo_seaweed; do
    docker volume create --label pgdemo.stack=operational "${volume}" >/dev/null
done

compose=(docker compose -f "${COMPOSE_FILE}" --env-file "${ENV_FILE}")
if (( RESET )); then
    printf '[run-all] resetting the pgdemo database; volumes stay\n'
    "${compose[@]}" stop server controller
    "${compose[@]}" run --rm init scripts/db-init.sh --reset
    exec "${compose[@]}" up -d server controller
fi

args=("${compose[@]}" up --build)
if [[ "${WHAT}" != all ]]; then
    args+=("${WHAT}")
fi
exec "${args[@]}"
