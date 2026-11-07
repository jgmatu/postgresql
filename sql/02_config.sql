/*
=============================================================================
02_config.sql - control plane: desired vs applied configuration
=============================================================================

The database is the only channel between the server (zone A) and the
controller (zone B). Configuration changes travel as rows, not as messages:

  dummy_config_pending          Desired state. Durable queue of change requests
                                (status PENDING -> SUCCESS | ERROR | TIMEOUT).
  dummy_config                  Applied state. One row per target. Changes only
                                when the controller has really applied a request.
  dummy_config_errors           Why a request failed. One row per failed request.
  dummy_config_history          Audit of every applied configuration.
  dummy_config_pending_history  Audit of every enqueue. request_id is not a
                                foreign key, so it survives queue cleanup.

request_id comes from one SEQUENCE, so several server instances can enqueue
concurrently without coordinating.

The NOTIFY triggers live in 04_triggers.sql:

  INSERT dummy_config_pending   -> config_requested  (server  -> controller)
  UPSERT dummy_config           -> config_events     (controller -> server, SUCCESS)
  INSERT dummy_config_errors    -> error_events      (controller -> server, ERROR)
*/

SET ROLE demo_owner;

CREATE SEQUENCE IF NOT EXISTS request_id_seq AS BIGINT;

-- Applied state. CHECKs are a second line of defence behind the controller's
-- own validation: the applied state can never hold an invalid configuration.
CREATE TABLE IF NOT EXISTS dummy_config (
    target      TEXT PRIMARY KEY CHECK (target IN ('dummy1', 'dummy2')),
    rate_ms     INT NOT NULL CHECK (rate_ms BETWEEN 50 AND 10000),
    amplitude   DOUBLE PRECISION NOT NULL CHECK (amplitude > 0 AND amplitude <= 1000),
    is_active   BOOLEAN NOT NULL DEFAULT TRUE,
    request_id  BIGINT,
    applied_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

-- Desired state / persistent queue. Values are only loosely constrained on
-- purpose: an out-of-range request is accepted, travels the loop, and comes
-- back as ERROR with a reason.
CREATE TABLE IF NOT EXISTS dummy_config_pending (
    request_id           BIGINT PRIMARY KEY,
    target               TEXT NOT NULL REFERENCES dummy_config (target) ON DELETE CASCADE,
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
CREATE INDEX IF NOT EXISTS dummy_config_pending_queue_idx
    ON dummy_config_pending (requested_at) WHERE status = 'PENDING';
CREATE INDEX IF NOT EXISTS dummy_config_pending_target_idx
    ON dummy_config_pending (target, requested_at DESC);

CREATE TABLE IF NOT EXISTS dummy_config_errors (
    error_id      BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    request_id    BIGINT UNIQUE REFERENCES dummy_config_pending (request_id) ON DELETE CASCADE,
    target        TEXT NOT NULL,
    error_code    TEXT NOT NULL,
    error_detail  TEXT,
    occurred_at   TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX IF NOT EXISTS dummy_config_errors_target_idx
    ON dummy_config_errors (target, occurred_at DESC);

CREATE TABLE IF NOT EXISTS dummy_config_history (
    history_id   BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    target       TEXT NOT NULL,
    rate_ms      INT NOT NULL,
    amplitude    DOUBLE PRECISION NOT NULL,
    is_active    BOOLEAN NOT NULL,
    request_id   BIGINT,
    recorded_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX IF NOT EXISTS dummy_config_history_target_idx
    ON dummy_config_history (target, recorded_at DESC);

CREATE TABLE IF NOT EXISTS dummy_config_pending_history (
    history_id           BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    request_id           BIGINT NOT NULL,
    target               TEXT NOT NULL,
    requested_rate_ms    INT NOT NULL,
    requested_amplitude  DOUBLE PRECISION NOT NULL,
    requested_is_active  BOOLEAN NOT NULL,
    requested_by         TEXT NOT NULL,
    recorded_at          TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX IF NOT EXISTS dummy_config_pending_history_target_idx
    ON dummy_config_pending_history (target, recorded_at DESC);

-- Initial applied state, so the producers have something to start from.
INSERT INTO dummy_config (target, rate_ms, amplitude, is_active) VALUES
    ('dummy1', 500, 10.0, TRUE),
    ('dummy2', 1000, 5.0, TRUE)
ON CONFLICT (target) DO NOTHING;

INSERT INTO dummy_config_history (target, rate_ms, amplitude, is_active, request_id)
SELECT c.target, c.rate_ms, c.amplitude, c.is_active, NULL
FROM dummy_config c
WHERE NOT EXISTS (SELECT 1 FROM dummy_config_history h WHERE h.target = c.target);

RESET ROLE;
