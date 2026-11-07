# PostgreSQL as a reactive, persistent message bus

A small, complete reference architecture in which **PostgreSQL is the only channel between two processes that never talk to each other**. Tables are the persistent queue and the source of truth; triggers and `LISTEN/NOTIFY` make the exchange reactive; roles and grants enforce the same authorization as the HTTP API.

The demo runs on dummy data with two tables, `dummy1` and `dummy2`. It covers:

- **Reactive IPC across network zones.** The server (zone A) and the controller (zone B) share no socket, no broker and no addresses. Each one only knows the database.
- **Channel multiplexing on one connection.** `dummy1_events` and `dummy2_events` (plus two control-plane channels) arrive on a single `LISTEN` socket. libpqxx routes each notification to the callback registered for its channel.
- **Desired vs applied configuration.** An admin posts a desired configuration. The controller validates it and applies or rejects it. The HTTP request waits for the outcome through a request-id **Dispatcher**, so a concurrent, synchronous API sits on top of an asynchronous queue.
- **Monthly-partitioned history.** Every sample is appended to a `RANGE`-partitioned history table. Partitions are created ahead of time, rows that landed in `DEFAULT` are moved, and retention drops whole months.
- **Secure authentication and authorization.** Passwords are stored as Argon2id hashes and requests carry ES384 JWTs. Each request runs under `SET LOCAL ROLE demo_admin | demo_normal`, so PostgreSQL enforces the role a second time, independently of the HTTP check.

```mermaid
flowchart LR
    client["client/pgdemo.sh<br/>(curl + jq)"]
    subgraph zoneA["zone A"]
        server["pgdemo-server<br/>Oat++ HTTP API"]
    end
    subgraph db["PostgreSQL 18"]
        queue[("dummy_config_pending<br/>(queue)")]
        applied[("dummy_config<br/>(applied)")]
        data[("dummy1 / dummy2<br/>+ monthly history")]
    end
    subgraph zoneB["zone B"]
        controller["pgdemo-controller<br/>producers + queue worker"]
    end

    client -- "HTTP + Bearer JWT" --> server
    server -- "INSERT desired" --> queue
    queue -. "NOTIFY config_requested" .-> controller
    controller -- "UPSERT applied / errors" --> applied
    applied -. "NOTIFY config_events / error_events" .-> server
    controller -- "UPSERT + history" --> data
    data -. "NOTIFY dummy1_events / dummy2_events" .-> server
    server -- "SSE /api/stream" --> client
```

Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design: sequence diagrams, the listener lifecycle, the Dispatcher, partitioning and the privilege matrix.

## Components

| Path | What it is |
|---|---|
| [`sql/`](sql) | Roles, schema, partition maintenance, triggers, views and grants (`00`–`05`, loaded in order). |
| [`libs/db`](libs/db) | `DatabaseManager`: query connection, listener connection and thread, callback registry, role-scoped transactions. |
| [`libs/auth`](libs/auth) | `PasswordHasher` (Argon2id) and `JwtManager` (ES384), both on Botan 3. |
| [`server/`](server) | `pgdemo-server`: Oat++ API, `AuthInterceptor`, `Dispatcher`, `EventHub` for Server-Sent Events. |
| [`controller/`](controller) | `pgdemo-controller`: one producer thread per table and the configuration queue worker. |
| [`client/pgdemo.sh`](client/pgdemo.sh) | Command-line client: login, users, data, history, configuration, live monitor. |
| [`patches/`](patches) | libpqxx 8.0.0 patch that adds a truly blocking `wait_notification()`. |
| [`docker/`](docker) | Image and compose file with two isolated network zones. |
| [`tests/`](tests) | GoogleTest (auth, dispatcher, database) and an end-to-end script. |

## Quick start (Ubuntu 26.04)

```bash
sudo apt install build-essential cmake ninja-build git pkg-config patch \
    libpq-dev libbotan-3-dev libboost-json-dev libgtest-dev \
    postgresql-18 postgresql-client curl jq openssl

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

libpqxx 8.0.0 (with the patch) and Oat++ 1.3.1 are downloaded and built by CMake.

Prepare the database. `db-init.sh` needs a superuser connection; on a local cluster the simplest one is a superuser role named after your account, used over the Unix socket:

```bash
sudo -u postgres createuser --superuser "$USER"     # once
PGHOST=/var/run/postgresql scripts/db-init.sh       # add --reset to start over
```

`db-init.sh` creates the roles and the `pgdemo` database, loads `sql/`, seeds the API user `admin`, and writes generated passwords to `run/demo.env` (mode 0600, gitignored). The two processes then connect over TCP to `127.0.0.1:5432` with those passwords (`scram-sha-256` in the default Ubuntu `pg_hba.conf`).

Start both processes. Each one logs in with its own PostgreSQL role:

```bash
scripts/run-all.sh          # or: scripts/run-all.sh server | controller
```

In another terminal:

```bash
. run/demo.env
PGDEMO_PASSWORD="$DEMO_ADMIN_PASSWORD" client/pgdemo.sh login admin

client/pgdemo.sh monitor                      # live events from both tables
client/pgdemo.sh config set dummy1 rate_ms=200 amplitude=25
client/pgdemo.sh config set dummy1 rate_ms=5  # rejected by the controller: 422 INVALID_RATE
client/pgdemo.sh config status                # applied vs desired, per target
client/pgdemo.sh burst 50                     # 50 concurrent requests through the Dispatcher
client/pgdemo.sh history dummy2 limit=5       # each row names its monthly partition

client/pgdemo.sh users add alice normal       # admin only
PGDEMO_PASSWORD=... client/pgdemo.sh login alice
client/pgdemo.sh users list                   # 403: normal users cannot manage users
```

Stop the controller and post a configuration: the API answers `202` with the request still `PENDING` in the queue. Start the controller again and it applies the request at start-up. The queue table, not the notification, is the source of truth.

## Quick start (Docker)

```bash
cp docker/.env.example docker/.env            # set both passwords
docker compose -f docker/compose.yaml --env-file docker/.env up --build
. docker/.env && PGDEMO_PASSWORD="$DEMO_ADMIN_PASSWORD" client/pgdemo.sh login admin
```

The server sits on `zone_a` (172.28.10.0/24) and is published on `127.0.0.1:8080` only. The controller sits on `zone_b` (172.28.20.0/24, `internal`, no outbound route). PostgreSQL is attached to both zones and is the only path between them. CI checks that the controller cannot open a TCP connection to the server.

## HTTP API

| Method and path | Role | Description |
|---|---|---|
| `POST /api/auth/login` | public | `{"username","password"}` → ES384 token |
| `GET /api/health` | public | Listener liveness |
| `GET /api/auth/me` | any | Identity and the PostgreSQL group used for the request |
| `GET /api/db/info` | any | Server version, backend PIDs, libpqxx version, SSE and Dispatcher counters |
| `GET /api/data/{dummy1\|dummy2}` | any | Live rows |
| `GET /api/history/{table}?from=&to=&item_id=&limit=` | any | History, newest first, with the partition name |
| `GET /api/stream?channels=dummy1,dummy2,config,error` | any | Server-Sent Events |
| `GET /api/config` | any | `v_dummy_config_sync_status`: applied, desired, `SYNC`/`PENDING`/`ERROR` |
| `POST /api/config` | admin | `{"target", "rate_ms"?, "amplitude"?, "is_active"?}`, partial update |
| `GET /api/config/requests/{id}` | any | One request and its error, if any |
| `GET /api/config/history?target=&limit=` | any | Applied-configuration audit |
| `GET /api/users` | admin | User directory (no hashes) |
| `POST /api/users` | admin | `{"username","password","role"}` |
| `DELETE /api/users/{username}` | admin | The last admin cannot be removed |

`POST /api/config` returns `200` when the request is applied, `422` when the controller rejects it (or it expired), `202` when it is still queued after `--config-timeout-ms`, and `503` when the Dispatcher already holds 500 waiters.

## Tests

```bash
cmake -S . -B build -G Ninja -DPGDEMO_BUILD_TESTS=ON && cmake --build build
ctest --test-dir build -LE db                  # unit: Argon2id, JWT, Dispatcher, EventHub

. run/demo.env                                 # database tests (controller stopped)
export PGDEMO_TEST_SERVER_CONNINFO="host=127.0.0.1 dbname=pgdemo user=demo_server password=$DEMO_SERVER_DB_PASSWORD"
export PGDEMO_TEST_CONTROLLER_CONNINFO="host=127.0.0.1 dbname=pgdemo user=demo_controller password=$DEMO_CONTROLLER_DB_PASSWORD"
ctest --test-dir build -L db

tests/e2e.sh                                   # fresh database, both processes, 38 checks
```

The database tests cover channel multiplexing, the registration and shutdown rules of the listener, partition routing, role enforcement inside PostgreSQL, the full NOTIFY-driven configuration loop through the Dispatcher, and request expiry. CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs everything on `ubuntu:26.04` against `postgres:18`, then brings up the compose stack.

## Scope

This is a demonstration of database architecture, not a product:

- **No TLS.** The HTTP API binds to loopback by default. Put it behind a TLS-terminating proxy before exposing it.
- **No reconnect.** If the listener connection drops, each process exits with status 1 so that a supervisor (compose `restart: unless-stopped`, systemd) restarts it. Queued work is not lost, because it lives in tables.
- **Small payloads.** NOTIFY payloads are limited to 8000 bytes, and these stay well below that.

## Licence

MIT, see [LICENSE](LICENSE). The libpqxx patch follows the libpqxx BSD 3-Clause licence; see [patches/README.md](patches/README.md).
# postgresql
