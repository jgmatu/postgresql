# pgdemo-controller

Zone B process. It supervises one `pgdemo-device` child per device and is the process that writes samples and applied configuration. PostgreSQL is the only channel to `pgdemo-server`: the controller has no address for it. Device identity, the internal CA and the certificate checks live in [docs/DEVICE_PKI.md](../docs/DEVICE_PKI.md). The tables, channels and the queue are the contract in [sql/README.md](../sql/README.md) and [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md).

Sources:

| File | Role |
|---|---|
| [`src/main.cpp`](src/main.cpp) | Options, signal mask, database, listener, then the fleet |
| [`src/Fleet.hpp`](src/Fleet.hpp), [`src/Fleet.cpp`](src/Fleet.cpp) | Process table, sessions, restarts, queue |
| [`src/Spawn.cpp`](src/Spawn.cpp) | `fork` + `execv` of `pgdemo-device` |

## Startup

`main` blocks `SIGINT`, `SIGTERM` and `SIGCHLD` before any thread exists, so the listener and the ROS spin inherit the mask. Children start with an empty mask (`spawn_process`). `SIGPIPE` is ignored.

It then connects as `pgdemo-controller`, creates the current and upcoming history partitions, builds the fleet and takes `devices-dir/.controller.lock`. The four channels are registered, the subscriptions to `pgdemo/worker/` are active, and the listener loop starts **before** `Fleet::start()`. `boot()` therefore reads devices and open enrollments while both `LISTEN` and the topics are already active: a commit or a `hello` in between is either in that read or arrives as an event.

`Fleet::start()` spawns six coroutines on the `io_context` and `main` unblocks the signals so Asio's `signal_set` can receive them. `io.run()` returns when shutdown has finished. The listener then stops, and the process exits with `Fleet::exit_code()`: `0` after a signal, `1` when the listener connection was lost (a supervisor can restart it). A bad option exits `2`.

## Threads

The `io_context` runs with one thread. That thread owns the process table, the slots, the sessions and each node's sample queue, so none of them takes a lock. The libpqxx listener thread only posts into `notify()`, and the ROS executor only posts each worker message onto the same thread. Each device process has its own ROS executor for `pgdemo/worker/` and `pgdemo/manager/`. Database calls block, so `on_db()` runs them on the io thread, one at a time, on the single query connection opened at start. The `LISTEN` connection is separate. Workers never see that connection: they have no connection string, and `close_range` between `fork` and `execv` drops every descriptor it holds. While an `INSERT` runs, this thread does not reap children or take signals; the batch below keeps that pause to one transaction.

```mermaid
flowchart TB
    subgraph ctl [pgdemo-controller]
        listener[listener thread]
        ros[ROS spin]
        subgraph ioThread [io_context thread]
            fleet[Fleet slots, sessions and per-node queues]
        end
    end
    pg[(PostgreSQL)]
    devices[pgdemo-device children]

    listener -->|post notify| fleet
    ros -->|post hello and uplink| fleet
    fleet -->|one transaction at a time| pg
    listener -.->|LISTEN NOTIFY| pg
    fleet -->|fork plus execv| devices
    devices -->|own ROS executor| ros
```

## Command line

```text
pgdemo-controller [--conninfo STR] [--max-devices N]
                  [--device-bin PATH] [--devices-dir DIR]
                  [--enrollment-timeout-s N] [--pending-max-age-s N]
                  [--maintenance-interval-s N]
```

| Option | Default | Effect |
|---|---|---|
| `--conninfo` | `PG*` environment | libpq connection string. One query connection is opened; the listener connection is separate |
| `--max-devices` | 16 | Device processes run at once; the rest are `UNMANAGED` |
| `--device-bin` | Beside this binary, else `../device/pgdemo-device` | `pgdemo-device` executable. It must be executable |
| `--devices-dir` | `run/devices` | One key directory per device, mode `0700`. The lock file is `DIR/.controller.lock` |
| `--enrollment-timeout-s` | 120 | Open enrollments older than this become `EXPIRED` |
| `--pending-max-age-s` | 600 | `PENDING` configuration older than this becomes `TIMEOUT` |
| `--maintenance-interval-s` | 3600 | How often partition maintenance runs |

Device names cannot start with a dot, so `.controller.lock` never clashes with a key directory.

`acquire_lock()` takes an exclusive `fcntl` lock on that file. A second controller on this machine exits. The lock is per process and is not inherited across `fork`, so a device child does not release it. One ROS domain runs one controller.

## Process table

One `Slot` per device name, or per enrollment that has no device row yet. The interesting fields:

| Field | Meaning |
|---|---|
| `next`, `next_request` | How the next process starts: `Run`, `Enroll` or `Rotate`, and the enrollment id when it is not `Run` |
| `pid`, `running`, `running_request` | The live process, how it was started, and the enrollment it is still waiting on |
| `session` | The worker's session, once `hello` has matched |
| `wanted` | Cleared after `AUTH_FAILED`, until the identity changes |
| `stopping` | This controller asked the process to exit |
| `removed` | The device was deleted; the slot goes away with the process |
| `failures` | Consecutive exits this controller did not request, for the backoff |

`reconcile()` starts a process when the slot is wanted, has no pid, has no restart already scheduled, and the fleet is under `--max-devices`. A `Run` with no device row, or with an identity that is not `ACTIVE`, stays down (`DISABLED` in the second case). A slot that would pass the limit is written as `UNMANAGED`. An enrollment that would not fit fails with `CAPACITY` and, when there is no device yet, the slot is forgotten.

`reconcile_all()` walks slots by `device_id` (enrollments with no id last), so the oldest devices take the free slots first.

## Starting a process

`start_process()` creates `devices-dir/<name>` at mode `0700` and execs:

```text
pgdemo-device --name NAME --type TYPE --payload scalar|gps|xy|xyz --dir DIR
              [--enroll REQUEST_ID | --rotate REQUEST_ID]
```

`spawn_process()` builds `argv` before the fork. Between `fork` and `execv` the child only calls async-signal-safe functions: it restores default dispositions for `SIGINT`, `SIGTERM`, `SIGCHLD` and `SIGPIPE`, clears the signal mask, asks for `SIGTERM` when the forking thread dies (`PR_SET_PDEATHSIG`), exits `127` if that parent is already gone, closes every descriptor above stderr, then `execv`. The child is a new ROS node (`pgdemo_device_<name>`); it inherits the parent's ROS environment and nothing of its DDS participant, because `execv` replaces the image. The death signal is tied to the thread that forked, which is the io thread.

The pid is recorded before the child can publish. `set_state` writes `STARTING`. If no `hello` arrives within 5 s the child is killed.

## A connection

`on_hello()` and `on_uplink()` receive the worker topics. The fields are the structs in [msgs/README.md](../msgs/README.md). A `hello` opens the session. Anything else for a name with no session is kept until that `hello`, then delivered. Appearing in the ROS graph is not admission: only the manager accepts a `hello`, and a worker cannot supervise another worker.

1. The `hello` names a wanted slot and the mode that slot expects (`run`, `enroll` or `rotate`). A slot with no process yet takes this node and is not forked. A `hello` repeated while the handshake is in progress is ignored. A `hello` after the device is online gets `bye`.
2. A child that was just forked has 5 s to publish that `hello`.
3. `Enroll` and `Rotate` then wait up to 30 s for a `csr`, store it with `submit_csr`, and wait on the session's decision channel for up to `--enrollment-timeout-s`. `enrollment_events` delivers the outcome into that channel. `ISSUED` is forwarded as `cert` and the process continues as `Run`; anything else is forwarded as `rejected` and the session ends. A process that exits before the certificate is delivered fails the enrollment with `DEVICE_EXITED`.
4. Challenge-response, 5 s for the proof. The controller re-reads the device, the active roots and the CRL, so a revocation committed a moment ago counts. The checks themselves are [docs/DEVICE_PKI.md](../docs/DEVICE_PKI.md#5-authentication). An identity that is not `ACTIVE` ends the session as `DISABLED`. A failed proof ends it as `AUTH_FAILED` and clears `wanted`: the process is left down until the identity changes.
5. `serve()` marks the session online **before** reading the applied configuration, so a configuration committed from here on is published after the initial `pgdemo/manager/config`. It writes `ONLINE` and reads samples until the session closes.

`read_samples()` refreshes `last_frame` on every message, including `heartbeat`, and pushes samples onto that node's queue. It does not open a database session. The manager publishes `pgdemo/manager/` (`config`, `bye`, the handshake) with the device name stamped on it. The inbox holds 64 uplink messages; one that does not fit is dropped.

When the session ends, the slot drops it and writes `OFFLINE` if it was `ONLINE` or `STARTING`. A child is then sent `SIGTERM`. A node this controller did not fork only gets `bye`.

```mermaid
sequenceDiagram
    participant F as Fleet
    participant D as pgdemo-device
    F->>D: fork and execv
    D->>F: hello on pgdemo/worker/control/hello
    opt enroll or rotate
        D->>F: csr
        F->>D: cert or rejected
    end
    F->>D: challenge
    D->>F: proof
    F->>D: config
    loop while online
        D->>F: sample or heartbeat
        F->>D: config or bye
    end
```

## Supervision

`reap_loop()` waits on `SIGCHLD` and collects every exited child with `waitpid(-1, WNOHANG)`.

An exit the controller did not request, while the next launch is `Run`, waits `1 << min(failures, 5)` seconds before the next start, and never more than 30 s. The first such exit waits 1 s. A process that stayed online for 60 s resets the count. A stop this controller requested (`stopping`, set by `stop_process`) is reconciled at once, which is how a rotation or a lifted hold replaces the process without waiting. `AUTH_FAILED` leaves `wanted` false, so the restart is not scheduled. `DISABLED` is reconciled back into the same state.

`stop_process()` publishes `bye` with the reason. A child is then sent `SIGTERM` (`stopping` is set, so the reaper starts the next one at once). A node this controller did not create is released.

`heartbeat_loop()` runs every 10 s. An online device with no message for 30 s (the device heartbeats every 5 s while paused) is `SIGKILL`ed when it is a child, and only released when it is not. Devices that spoke are touched in `device_session.last_seen`.

`maintenance_loop()` runs every 30 s. It expires enrollments older than `--enrollment-timeout-s`, sweeps (re-reads every device and every pending enrollment, drops slots whose device row is gone, reconciles), and, every `--maintenance-interval-s`, creates the upcoming history partitions. The sweep is what notices a certificate that expired without a write. If `listener_alive()` is false it shuts down with exit code 1.

Shutdown, from `SIGINT` or `SIGTERM`, publishes `bye` and sends `SIGTERM` to every child, waits up to 5 s, and `SIGKILL`s whoever is left. It flushes every sample queue, releases the lock, resets every session, and stops the `io_context`.

## Telemetry queues

Each device node has its own sample queue on its slot. A node publishes one payload, so the queue holds that payload's samples. A `heartbeat`, a CSR, a proof and a session write do not enter it: those call `on_db()` at once, on this same thread.

A queue holds 256 samples. Past that, the sample that just arrived is dropped and counted, and the session coroutine does not wait. Forgetting a slot moves its samples aside so the next flush still writes them. `telemetry_loop()` flushes when 64 samples are waiting across every queue, or 100 ms have passed. One flush is one transaction: it inserts every row into `telemetry_scalar`, `telemetry_gps`, `telemetry_xy` or `telemetry_xyz` (one statement per payload present) and upserts `device_state` once per device with the highest `seq` in the batch. The statement trigger notifies the server once per insert. The server reads what it has not consumed yet and advances `telemetry_read_cursor` in that same read. A flush that fails is logged and those samples are not retried. Shutdown flushes whatever remains after the workers have been closed, before it resets `device_session`.

## What changes the fleet

`notify()` reads `{"channel", "payload"}` and spawns one coroutine:

| Channel | What the fleet does |
|---|---|
| `enrollment_requested` | Load the `PENDING` row. `ENROLL` creates a slot and sets `next` to `Enroll`. `ROTATE` requires a slot that already has a device. A running process is stopped so the next one starts in the new mode. |
| `enrollment_events` | If that process is waiting in `obtain_certificate()`, the outcome is delivered on its decision channel. A refusal that arrived earlier stops the process, or forgets a slot that never had a device. |
| `device_events` | `DELETE` marks the slot removed, stops the process, and `forget()` deletes `devices-dir/<name>`. `INSERT`, `ROTATED`, `REVOKED` and `UNREVOKED` reload the device. An identity that is not `ACTIVE` stops a `Run` process and writes `DISABLED`. `ACTIVE` again, or a new fingerprint, sets `wanted` and starts the process when it is down. |
| `config_requested` | `drain()` takes every `PENDING` row (`FOR UPDATE SKIP LOCKED`), validates `rate_ms` in `[50, 10000]` and `amplitude` in `(0, 1000]`, and commits `SUCCESS`, `ERROR` or `TIMEOUT`. Each applied configuration is published on `pgdemo/manager/config` to that device's online session. The same drain runs once from `boot()`. |

Session states written to `device_session`, and shown by `GET /api/devices/NAME` as `session` and `process`:

| State | When |
|---|---|
| `STARTING` | Process spawned, not online yet |
| `ONLINE` | Authenticated and streaming |
| `OFFLINE` | Session ended, or the process could not be spawned |
| `UNMANAGED` | Wanted, but `--max-devices` is already full |
| `AUTH_FAILED` | `hello` or the proof was rejected; stays down until the identity changes |
| `DISABLED` | The identity is not `ACTIVE` (revoked, held, or expired) |
