/*
=============================================================================
02_config.sql - control plane: desired vs applied configuration, per device
=============================================================================

The database is the only channel between the server (zone A) and the
controller (zone B). Configuration changes travel as rows, not as messages:

  device_config_pending          Desired state. Durable queue of change requests
                                 (status PENDING -> SUCCESS | ERROR | TIMEOUT).
  device_config                  Applied state. One row per device. Written only
                                 by the controller: the seed rows below, the
                                 defaults of a newly registered device, and
                                 every request it really applied.
  device_config_errors           Why a request failed. One row per failed request.
  device_config_history          Audit of every applied configuration.
  device_config_pending_history  Audit of every enqueue. Neither audit has a
                                 foreign key, so both survive queue cleanup and
                                 device deregistration.

request_id comes from one SEQUENCE, so several server instances can enqueue
concurrently without coordinating.

The NOTIFY triggers live in 04_triggers.sql:

  INSERT device_config_pending   -> config_requested  (server  -> controller)
  UPSERT device_config           -> config_events     (controller -> server, SUCCESS)
  INSERT device_config_errors    -> error_events      (controller -> server, ERROR)
*/

SET ROLE demo_owner;

CREATE SEQUENCE IF NOT EXISTS request_id_seq AS BIGINT;

-- Applied state. CHECKs are a second line of defence behind the controller's
-- own validation: the applied state can never hold an invalid configuration.
CREATE TABLE IF NOT EXISTS device_config (
    device_id   INT PRIMARY KEY REFERENCES device (device_id) ON DELETE CASCADE,
    rate_ms     INT NOT NULL CHECK (rate_ms BETWEEN 50 AND 10000),
    amplitude   DOUBLE PRECISION NOT NULL CHECK (amplitude > 0 AND amplitude <= 1000),
    is_active   BOOLEAN NOT NULL DEFAULT TRUE,
    request_id  BIGINT,
    applied_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

-- Desired state / persistent queue. Values are only loosely constrained on
-- purpose: an out-of-range request is accepted, travels the loop, and comes
-- back as ERROR with a reason.
CREATE TABLE IF NOT EXISTS device_config_pending (
    request_id           BIGINT PRIMARY KEY,
    device_id            INT NOT NULL REFERENCES device (device_id) ON DELETE CASCADE,
    requested_rate_ms    INT NOT NULL CHECK (requested_rate_ms > 0),
    requested_amplitude  DOUBLE PRECISION NOT NULL,
    requested_is_active  BOOLEAN NOT NULL,
    status               TEXT NOT NULL DEFAULT 'PENDING'
                         CHECK (status IN ('PENDING', 'SUCCESS', 'ERROR', 'TIMEOUT')),
    requested_by         TEXT NOT NULL,
    requested_at         TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    completed_at         TIMESTAMPTZ
);

-- The queue scan only reads PENDING rows in arrival order.
CREATE INDEX IF NOT EXISTS device_config_pending_queue_idx
    ON device_config_pending (requested_at) WHERE status = 'PENDING';
CREATE INDEX IF NOT EXISTS device_config_pending_device_idx
    ON device_config_pending (device_id, requested_at DESC);

CREATE TABLE IF NOT EXISTS device_config_errors (
    error_id      BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    request_id    BIGINT UNIQUE REFERENCES device_config_pending (request_id) ON DELETE CASCADE,
    device_id     INT NOT NULL,
    error_code    TEXT NOT NULL,
    error_detail  TEXT,
    occurred_at   TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX IF NOT EXISTS device_config_errors_device_idx
    ON device_config_errors (device_id, occurred_at DESC);

CREATE TABLE IF NOT EXISTS device_config_history (
    history_id   BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    device_id    INT NOT NULL,
    rate_ms      INT NOT NULL,
    amplitude    DOUBLE PRECISION NOT NULL,
    is_active    BOOLEAN NOT NULL,
    request_id   BIGINT,
    recorded_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX IF NOT EXISTS device_config_history_device_idx
    ON device_config_history (device_id, recorded_at DESC);

CREATE TABLE IF NOT EXISTS device_config_pending_history (
    history_id           BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    request_id           BIGINT NOT NULL,
    device_id            INT NOT NULL,
    requested_rate_ms    INT NOT NULL,
    requested_amplitude  DOUBLE PRECISION NOT NULL,
    requested_is_active  BOOLEAN NOT NULL,
    requested_by         TEXT NOT NULL,
    recorded_at          TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX IF NOT EXISTS device_config_pending_history_device_idx
    ON device_config_pending_history (device_id, recorded_at DESC);

-- Applied state of the seeded devices, so the API can enqueue changes for them
-- before the controller has ever run. Devices registered later get theirs from
-- the controller when it receives device_events.
INSERT INTO device_config (device_id, rate_ms, amplitude, is_active)
SELECT d.device_id, t.default_rate_ms, t.default_amplitude, TRUE
FROM device d
JOIN device_type t USING (type_id)
WHERE d.name IN ('dummy1', 'dummy2', 'dummy3')
ON CONFLICT (device_id) DO NOTHING;

INSERT INTO device_config_history (device_id, rate_ms, amplitude, is_active, request_id)
SELECT c.device_id, c.rate_ms, c.amplitude, c.is_active, NULL
FROM device_config c
WHERE NOT EXISTS (SELECT 1 FROM device_config_history h WHERE h.device_id = c.device_id);

RESET ROLE;
