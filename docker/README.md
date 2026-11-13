# Virtualization

Two projects include [`compose.yaml`](compose.yaml), each for a different use. They cannot run at the same time: both publish `127.0.0.1:8080` and both use `172.28.10.0/24` and `172.28.20.0/24`.

| Project | File | Name | Use |
|---|---|---|---|
| Operational | [`operational/compose.yaml`](operational/compose.yaml) | `pgdemo` | Development. External volumes survive `stop`, `down` and `down -v` |
| End to end | [`e2e/compose.yaml`](e2e/compose.yaml) | `pgdemo-e2e` | Stateless tests. Each run starts empty; `down -v` removes only the `pgdemo-e2e_` volumes |

The operational project is the one that stays. It runs four containers. PostgreSQL is the only path between the server and the controller. The factory is a fourth container on a network of its own. Compose is the supervisor: each process is the container's main program, and a stop sends `SIGTERM` to that program. The end-to-end project does not start the factory.

## Services

[`Dockerfile`](Dockerfile) builds one image, `pgdemo:latest`. The controller forks `pgdemo-device`, so that binary lives in the same image. PostgreSQL is `postgis/postgis:18-3.7`, not this image.

| Service | Role | Stays up |
|---|---|---|
| `postgres` | The database, attached to both zones | yes |
| `init` | [`scripts/db-init.sh`](../scripts/db-init.sh): schema, device CA, admin user, `run/demo.env` | no, it exits |
| `server` | `pgdemo-server` on HTTPS port 8443 | yes, `restart: unless-stopped` |
| `controller` | `pgdemo-controller` and one device process per enrolled device | yes, `restart: unless-stopped` |
| `factory` | `pgdemo-factory`, operational project only | yes, `restart: unless-stopped` |

[`entrypoint.sh`](entrypoint.sh) is the command of `server`, `controller` and `factory`. The server and the controller read `run/demo.env`. The server generates the JWT key and the TLS certificate. The factory generates its root and copies only `root.crt` out. Each one then `exec`s its binary. Tini is pid 1 (`init: true`) and forwards the signal to that process. `init` does not use the entrypoint: its command is `scripts/db-init.sh`.

The end-to-end project does not define `factory`, so the workers there still enroll with the demo CA.

## Networks

```mermaid
flowchart LR
    subgraph zoneA ["zone_a 172.28.10.0/24"]
        pgA["postgres 172.28.10.10"]
        server["server 172.28.10.20"]
    end
    subgraph zoneB ["zone_b internal 172.28.20.0/24"]
        pgB["postgres 172.28.20.10"]
        controller["controller 172.28.20.20"]
    end
    subgraph zoneF ["zone_factory internal 172.28.30.0/24"]
        factory["factory 172.28.30.10"]
        controllerF["controller 172.28.30.20"]
    end
    server --> pgA
    controller --> pgB
    controllerF --> factory
```

`zone_b` and `zone_factory` are `internal: true`. The controller has no route to `zone_a`. The factory is not on `zone_a` or `zone_b`, and its port is not published, so the server and the host cannot open it. The process binds `0.0.0.0` inside that one network. Device traffic stays inside the controller container: the children publish on its ROS domain and ask the factory at `172.28.30.10:9443`. The host publishes one port, bound to loopback: `127.0.0.1:8080` forwards to the server's 8443. The end-to-end suite checks that the controller cannot open a TCP connection to the server and that it can open one to PostgreSQL on `172.28.20.10:5432`. The end-to-end project has no `zone_factory`.

## Volumes

Operational volumes are external and named `pgdemo_pgdata`, `pgdemo_demo-run`, `pgdemo_ca-keys`, `pgdemo_device-keys`, `pgdemo_factory-key`, `pgdemo_factory-cert` and `pgdemo_manager-keys`. `scripts/run-all.sh` creates any that are missing. Compose does not delete them. A test run uses its own `pgdemo-e2e_` volumes, and those do go away with `down -v`.

Project `pgdemo` is the persistent virtual environment. In this flowchart a rectangle is a container and a cylinder is an external volume. The edge is the mount path inside the container. The cylinders sit outside the containers: `stop`, `down` and `down -v` leave them. `ca-keys` covers `/app/run/ca` on top of `demo-run`. The factory writes `factory-cert` and the controller mounts it read only.

```mermaid
flowchart LR
  subgraph containers [pgdemo containers]
    postgres[postgres]
    init[init]
    server[server]
    controller[controller]
    factory[factory]
  end
  subgraph persistent [external volumes]
    pgdata[("pgdemo_pgdata")]
    demoRun[("pgdemo_demo-run")]
    caKeys[("pgdemo_ca-keys")]
    deviceKeys[("pgdemo_device-keys")]
    factoryKey[("pgdemo_factory-key")]
    factoryCert[("pgdemo_factory-cert")]
    managerKeys[("pgdemo_manager-keys")]
  end
  postgres -->|"/var/lib/postgresql"| pgdata
  init -->|"/app/run"| demoRun
  server -->|"/app/run"| demoRun
  controller -->|"/app/run"| demoRun
  init -->|"/app/run/ca"| caKeys
  server -->|"/app/run/ca"| caKeys
  controller -->|"/app/run/devices"| deviceKeys
  controller -->|"/app/run/controller"| managerKeys
  controller -->|"ro /app/run/factory-cert"| factoryCert
  factory -->|"/app/run/factory"| factoryKey
  factory -->|"/app/run/factory-cert"| factoryCert
```

| Volume | Mounted by | Contents |
|---|---|---|
| `pgdata` | `postgres` | the database |
| `demo-run` | `init`, `server`, `controller` | `run/demo.env`, JWT keys, the TLS certificate |
| `ca-keys` | `init`, `server` | the device CA. The controller never sees the key |
| `device-keys` | `controller` | one directory per device, and the controller lock |
| `factory-key` | `factory` | `root.key`. Nothing else mounts it |
| `factory-cert` | `factory` writes it, `controller` reads it | `root.crt` only |
| `manager-keys` | `controller` | the manager's own key and self-signed root |

## Operational stack

Fill `webroot/` before the first build. The image copies it, and the entrypoint passes `--webroot` only when `index.html` is there.

```bash
cp docker/operational/.env.example docker/operational/.env   # set both passwords
scripts/run-all.sh                 # postgres, server and controller
scripts/run-all.sh server          # the API, plus the database and init
scripts/run-all.sh controller      # the manager and the factory, plus the database and init
scripts/run-all.sh --factory-reset # drop and recreate the pgdemo database; volumes stay
scripts/stop-all.sh                # stop the project; the external volumes stay
scripts/stop-all.sh server         # only the server (also: controller)
```

`docker/operational/.env` is gitignored. It is not `run/demo.env`. `POSTGRES_PASSWORD` is the container's database superuser. `DEMO_ADMIN_PASSWORD` is the API user `admin`. The role passwords of `demo_server` and `demo_controller` are generated by `init` and stay in the `demo-run` volume.

`--factory-reset` stops the server and the controller, then runs [`scripts/db-init.sh --reset`](../scripts/db-init.sh). That drops the `pgdemo` database and loads the schema again, including the `dummy1`, `dummy2` and `dummy3` enrollments, and seeds `admin`. No volume is removed: the cluster, `run/demo.env`, the device CA, device directories, the factory key and certificate, and the manager key stay. A later enrollment can fail when a device directory from the previous database is still on `device-keys`. The factory keeps running. The server and the controller then start again, without a rebuild. A normal `up` does not run `init` again, because Compose treats the exited container as finished; the reset uses `compose run`.

The client talks to the published port. Copy the certificate out of the server container:

```bash
mkdir -p run
docker compose -f docker/operational/compose.yaml --env-file docker/operational/.env \
    cp server:/app/run/keys/tls.crt run/docker-tls.crt
export PGDEMO_URL=https://127.0.0.1:8080 PGDEMO_CA=run/docker-tls.crt
. docker/operational/.env
PGDEMO_PASSWORD="$DEMO_ADMIN_PASSWORD" client/pgdemo.sh login admin
```

Logs are `docker compose -f docker/operational/compose.yaml logs`. Ctrl-C on `scripts/run-all.sh` stops the containers that are attached.

## End to end

[`e2e/compose.yaml`](e2e/compose.yaml) is the stateless stack the Robot suite creates and destroys. The project name is `pgdemo-e2e`. Setup runs `down -v` and teardown runs it again, so every execution starts with an empty database, a new device CA and no device keys. The factory is not defined, and enrollment stays on the demo CA. The volumes removed are `pgdemo-e2e_pgdata`, `pgdemo-e2e_demo-run`, `pgdemo-e2e_ca-keys` and `pgdemo-e2e_device-keys`. The operational `pgdemo_` volumes stay.

Stop the operational stack first (`scripts/stop-all.sh`): the port and the subnets collide. A build in either project replaces the local `pgdemo:latest` image the other one would start.

[tests/e2e/README.md](../tests/e2e/README.md) is the suite.
