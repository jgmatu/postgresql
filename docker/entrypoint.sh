#!/usr/bin/env bash
# Run one pgdemo process in the foreground. Compose is the supervisor.
# Tini is pid 1 and this script execs the binary, so SIGTERM reaches it.
#
# Usage: entrypoint.sh server|controller
set -euo pipefail

if [[ -f /opt/ros/lyrical/setup.bash ]]; then
    set +u
    # shellcheck disable=SC1091
    source /opt/ros/lyrical/setup.bash
    set -u
fi
export LD_LIBRARY_PATH="/app/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

ENV_FILE=/app/run/demo.env
if [[ ! -f "${ENV_FILE}" ]]; then
    printf '[entrypoint] %s is missing; init has not finished\n' "${ENV_FILE}" >&2
    exit 1
fi
# shellcheck disable=SC1090
source "${ENV_FILE}"

export PGHOST="${PGDEMO_PGHOST:-postgres}"
export PGPORT="${PGDEMO_PGPORT:-5432}"
export PGDATABASE="${PGDEMO_DB:-pgdemo}"

case "${1:-}" in
    server)
        /app/scripts/gen-jwt-keys.sh
        /app/scripts/gen-tls-cert.sh
        webroot_args=()
        if [[ -f /app/webroot/index.html ]]; then
            webroot_args=(--webroot /app/webroot)
        else
            printf '[entrypoint] WARN: panel no disponible: webroot/index.html is absent\n' >&2
        fi
        export PGUSER=demo_server
        export PGPASSWORD="${DEMO_SERVER_DB_PASSWORD}"
        exec /app/build/server/pgdemo-server \
            --listen-addr "${PGDEMO_LISTEN_ADDR:-0.0.0.0}" --port 8443 \
            --jwt-key /app/run/keys/jwt.key --jwt-cert /app/run/keys/jwt.crt \
            --ca-key /app/run/ca/ca.key --ca-cert /app/run/ca/ca.crt \
            --tls-key /app/run/keys/tls.key --tls-cert /app/run/keys/tls.crt \
            "${webroot_args[@]}"
        ;;
    controller)
        export PGUSER=demo_controller
        export PGPASSWORD="${DEMO_CONTROLLER_DB_PASSWORD}"
        exec /app/build/controller/pgdemo-controller \
            --device-bin /app/build/device/pgdemo-device \
            --devices-dir /app/run/devices \
            --max-devices "${PGDEMO_MAX_DEVICES:-16}"
        ;;
    *)
        printf '[entrypoint] usage: entrypoint.sh server|controller\n' >&2
        exit 1
        ;;
esac
