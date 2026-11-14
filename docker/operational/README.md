# Operational network

Project `pgdemo` is the stack that stays. [docker/README.md](../README.md) is the virtualization of both projects. This file is the network scheme of this one.

Three bridges, and no new one for the object store. `zone_a` and `zone_b` are declared in [`compose.yaml`](../compose.yaml). `zone_factory` is declared in [`compose.yaml`](compose.yaml). PostgreSQL and SeaweedFS join both zone bridges. Each store is one container with two interfaces.

```mermaid
flowchart LR
    subgraph zoneA ["zone_a bridge 172.28.10.0/24"]
        pgA["postgres 172.28.10.10"]
        swA["seaweed 172.28.10.11"]
        server["server 172.28.10.20"]
    end
    subgraph zoneB ["zone_b bridge internal 172.28.20.0/24"]
        pgB["postgres 172.28.20.10"]
        swB["seaweed 172.28.20.11"]
        controller["controller 172.28.20.20"]
    end
    subgraph zoneF ["zone_factory bridge internal 172.28.30.0/24"]
        factory["factory 172.28.30.10"]
        controllerF["controller 172.28.30.20"]
    end
    server --> pgA
    server --> swA
    controller --> pgB
    controller --> swB
    controllerF --> factory
```

`pgA` and `pgB` are the same database. `swA` and `swB` are the same `weed mini`. There is no edge between the bridges, and no edge between the two stores.

## Addresses

| Address | Bridge |
|---|---|
| `postgres` `172.28.10.10` | `zone_a` |
| `seaweed` `172.28.10.11` | `zone_a` |
| `server` `172.28.10.20` | `zone_a` |
| `postgres` `172.28.20.10` | `zone_b` |
| `seaweed` `172.28.20.11` | `zone_b` |
| `controller` `172.28.20.20` | `zone_b` |
| `factory` `172.28.30.10` | `zone_factory` |
| `controller` `172.28.30.20` | `zone_factory` |

The name `postgres` resolves, on each bridge, to the address of that bridge. The name `seaweed` does the same. The server uses the addresses on `zone_a`. The controller uses the addresses on `zone_b`.

## Isolation

`zone_b` and `zone_factory` are `internal: true`. `zone_b` has no route to `zone_a`. A container with two interfaces does not forward packets from its neighbors, so the server has no route to `172.28.20.0/24` and the controller has no route to `172.28.10.0/24`. Neither store is a gateway.

PostgreSQL is the path between the server and the controller. SeaweedFS is the object store on those same bridges. Its S3 gateway listens on port 8333. That port is not published. The host publishes one port, `127.0.0.1:8080`, to the server's 8443.

The filer keeps SeaweedFS's own index in the volume `pgdemo_seaweed`, mounted at `/data` in that container only. The filer is not pointed at the `pgdemo` database.

Workers the controller creates join `zone_b` and `zone_factory`. They are not on `zone_a`. The end-to-end project does not include this file, so it has no factory, no SeaweedFS and no `zone_factory`.

## Containers, traffic and volumes

Zone A is analysis and operations. Zone B is robotics. A rectangle is one interface of a container, and a cylinder is storage. `pgA` and `pgB` are the same database and the same mount. `swA` and `swB` are the same `weed mini` and the same mount. `controller` and `controllerF` are one process. `worker` and `workerF` are one device container, `pgdemo-worker-<name>`.

```mermaid
flowchart LR
    host["host 127.0.0.1:8080"]

    subgraph zoneA ["zone A, analysis and operations, 172.28.10.0/24"]
        init["init"]
        server["server .20"]
        pgA["postgres .10"]
        swA["seaweed .11"]
    end

    subgraph zoneB ["zone B, robotics, internal 172.28.20.0/24"]
        controller["controller .20"]
        worker["worker"]
        pgB["postgres .10"]
        swB["seaweed .11"]
    end

    subgraph zoneF ["zone_factory, internal 172.28.30.0/24"]
        factory["factory .10"]
        controllerF["controller .20"]
        workerF["worker"]
    end

    subgraph disks ["volumes"]
        pgdata[("pgdemo_pgdata")]
        seaweedVol[("pgdemo_seaweed")]
        demoRun[("pgdemo_demo-run")]
        caKeys[("pgdemo_ca-keys")]
        deviceKeys[("pgdemo_device-keys")]
        factoryKey[("pgdemo_factory-key")]
        factoryCert[("pgdemo_factory-cert")]
        managerKeys[("pgdemo_manager-keys")]
        puf[("pgdemo_puf_name")]
        sock[("docker.sock")]
    end

    host -->|"HTTPS 8443"| server
    init -->|"SQL"| pgA
    server -->|"SQL"| pgA
    pgA -.->|"NOTIFY"| server
    controller -->|"SQL"| pgB
    pgB -.->|"NOTIFY"| controller
    controller -->|"Engine API"| sock
    controller -->|"creates"| worker
    worker -->|"ROS 2"| controller
    controllerF -->|"9443"| factory
    workerF -->|"9443"| factory

    pgA -->|"/var/lib/postgresql"| pgdata
    pgB -->|"/var/lib/postgresql"| pgdata
    swA -->|"/data"| seaweedVol
    swB -->|"/data"| seaweedVol
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
    workerF -->|"ro /run/factory-cert"| factoryCert
    worker -->|"ro /run/manager.crt"| managerKeys
    worker -->|"/var/lib/pgdemo/puf"| puf
```

The host reaches analysis through the one published port. `init` loads the schema and exits. The server and the controller meet in the commits of PostgreSQL: each one uses the address of its own zone, and a dotted edge is the `NOTIFY` of that commit. A worker's traffic stays in robotics and on the factory bridge. On zone B it publishes ROS 2 to the controller at `172.28.20.20`. On `zone_factory` it asks the factory to sign, and the controller does the same at `172.28.30.10:9443`. The controller creates that container through the engine socket. `docker.sock` is that socket, not a project volume.

Each worker mounts only its own `pgdemo_puf_<name>` at `/var/lib/pgdemo/puf`, plus `root.crt` and `manager.crt` read only. `manager.key` stays on `pgdemo_manager-keys`. The two arrows into `pgdemo_pgdata` are one mount, and the two arrows into `pgdemo_seaweed` are one mount. SeaweedFS is addressed in both zones. No process opens port 8333 yet.
