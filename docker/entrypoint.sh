#!/usr/bin/env bash
# Run one pgdemo process in the foreground. Compose is the supervisor.
# Tini is pid 1 and this script execs the binary, so SIGTERM reaches it.
#
# Usage: entrypoint.sh server|controller|factory|device
set -euo pipefail

if [[ -f /opt/ros/lyrical/setup.bash ]]; then
    set +u
    # shellcheck disable=SC1091
    source /opt/ros/lyrical/setup.bash
    set -u
fi
export LD_LIBRARY_PATH="/app/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# The pgdemo home is /app, owned by root. rcl refuses to start when it cannot
# create $HOME/.ros/log. Workers inherit this path.
export ROS_LOG_DIR=/tmp/ros-log
mkdir -p "${ROS_LOG_DIR}"

# The factory has no database. Its key stays in this container; only the
# certificate is copied to the volume the controller mounts.
if [[ "${1:-}" == factory ]]; then
    /app/scripts/gen-factory.sh /app/run/factory
    cp -f /app/run/factory/root.crt /app/run/factory-cert/root.crt
    chmod 644 /app/run/factory-cert/root.crt
    exec /app/build/factory/pgdemo-factory \
        --key /app/run/factory/root.key \
        --cert /app/run/factory/root.crt \
        --listen 0.0.0.0 --port 9443
fi

# A worker has no database. The controller starts this as root so the private
# volume, the keystore and the ROS log can be given to pgdemo, then the
# process drops to that user.
if [[ "${1:-}" == device ]]; then
    shift
    mkdir -p /var/lib/pgdemo/puf /var/lib/pgdemo/keystore "${ROS_LOG_DIR}"
    if [[ "$(id -u)" -eq 0 ]]; then
        chown pgdemo:pgdemo /var/lib/pgdemo /var/lib/pgdemo/puf /var/lib/pgdemo/keystore "${ROS_LOG_DIR}"
        chmod 700 /var/lib/pgdemo/puf /var/lib/pgdemo/keystore
        exec runuser -u pgdemo -- /app/build/device/pgdemo-device "$@"
    fi
    exec /app/build/device/pgdemo-device "$@"
fi

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
        if [[ "${PGDEMO_DOCKER_SPAWN:-}" == 1 ]]; then
            export ROS_AUTOMATIC_DISCOVERY_RANGE=SUBNET
        fi
        factory_args=()
        if [[ -f /app/run/factory-cert/root.crt ]]; then
            /app/controller/scripts/gen-manager-key.sh /app/run/controller
            factory_args=(
                --manager-key /app/run/controller/manager.key
                --factory-cert /app/run/factory-cert/root.crt
                --factory "${PGDEMO_FACTORY:-172.28.30.10:9443}"
            )
        fi
        exec /app/build/controller/pgdemo-controller \
            --device-bin /app/build/device/pgdemo-device \
            --devices-dir /app/run/devices \
            --max-devices "${PGDEMO_MAX_DEVICES:-16}" \
            "${factory_args[@]}"
        ;;
    *)
        printf '[entrypoint] usage: entrypoint.sh server|controller\n' >&2
        exit 1
        ;;
esac
