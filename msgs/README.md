# ROS 2 interface API

The contract between the manager (`pgdemo-controller`) and its workers (`pgdemo-device`). PostgreSQL is not part of it: a worker has no database login, and the manager is the only process that writes samples. The structs live in [`msg/`](msg) and `rosidl` compiles them to `pgdemo_msgs::msg`.

ROS 2 discovers nodes as peers. Inside this system that does not grant authority. The manager publishes `pgdemo/manager/`. Workers publish `pgdemo/worker/`. A worker does not publish on the manager's topics, does not subscribe to another worker's telemetry, and ignores a manager message whose `device` is not its own name. One ROS domain runs one manager; a second controller on the same machine is stopped by `run/devices/.controller.lock`.

`hello` is the only transient-local topic (reliable, depth 1). A worker repeats it once a second until the challenge, so a manager that starts later still sees the announcement. Every other topic is reliable and volatile, depth 16.

```mermaid
flowchart LR
    workers[pgdemo_device workers]
    manager[pgdemo_controller manager]
    workers -->|"pgdemo/worker: hello, heartbeat, error, csr, proof, telemetry"| manager
    manager -->|"pgdemo/manager: bye, challenge, certificate, rejected, config"| workers
```

## Control

| Topic | Struct | Publisher | Fields |
|---|---|---|---|
| `pgdemo/worker/control/hello` | `Hello` | worker | `device`, `mode` (`RUN=0`, `ENROLL=1`, `ROTATE=2`), `request_id` (`0` when running) |
| `pgdemo/worker/control/heartbeat` | `Heartbeat` | worker | `device`. Sent every 5 s while paused. Refreshes the manager's silence timer. |
| `pgdemo/worker/control/error` | `Error` | worker | `device`, `code`, `detail`. The worker cannot continue. |
| `pgdemo/manager/control/bye` | `Bye` | manager | `device`, `reason`. The worker exits. |

## Security

The nonce and the signature are raw bytes. The text the worker signs is unchanged: `"pgdemo-device-auth-v1"`, a NUL, the nonce, a NUL, the name, a NUL, and the certificate's SHA-256 fingerprint. See [docs/DEVICE_PKI.md](../docs/DEVICE_PKI.md).

| Topic | Struct | Publisher | Fields |
|---|---|---|---|
| `pgdemo/worker/security/csr` | `Csr` | worker | `device`, `csr` (PEM) |
| `pgdemo/manager/security/certificate` | `Certificate` | manager | `device`, `cert` (PEM) |
| `pgdemo/manager/security/rejected` | `Rejected` | manager | `device`, `code`, `detail` |
| `pgdemo/manager/security/challenge` | `Challenge` | manager | `device`, `nonce` (`uint8[32]`) |
| `pgdemo/worker/security/proof` | `Proof` | worker | `device`, `cert` (PEM), `signature` (`uint8[96]`, ECDSA P-384 `r‖s`) |

## Configuration

`pgdemo/manager/config`, struct `Config`, published by the manager after the proof and again when applied configuration changes. Fields: `device`, `device_id`, `rate_ms`, `amplitude`, `is_active`. The worker applies it at once and re-arms its timer.

## Telemetry

A worker publishes only the topic that matches `--payload`.

| Topic | Struct | Fields |
|---|---|---|
| `pgdemo/worker/telemetry/scalar` | `ScalarSample` | `device`, `seq`, `value` |
| `pgdemo/worker/telemetry/gps` | `GpsSample` | `device`, `seq`, `origin_lon`, `origin_lat`, `radius_m`, `bearing_rad` |
| `pgdemo/worker/telemetry/xy` | `XySample` | `device`, `seq`, `x`, `y` |

The manager does not insert each sample on its own. It queues every worker's samples on the `io_context` thread and flushes them in one database transaction. That queue, and the rest of the process lifecycle, is [controller/README.md](../controller/README.md).
