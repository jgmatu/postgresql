/*
=============================================================================
01_data.sql - data plane: devices, their last sample and monthly-partitioned telemetry
=============================================================================

A device is a row, not a table. Registering one adds a row to device; the
schema and the code do not change. Tables are per data shape instead:

  device_type        what a device produces (payload scalar | gps) and its
                     default configuration
  device             the registered devices. Each one points at its current
                     X.509 certificate (cert_id, NOT NULL); 03_identity.sql
                     adds that foreign key and is the only way to insert one
  device_state       the last sample of each device (one row per device, UPSERT)
  telemetry_scalar   every scalar sample, range-partitioned by month
  telemetry_gps      every GPS fix, range-partitioned by month

Every sample UPSERTs device_state and appends to the telemetry table of its
payload in the same transaction. The telemetry tables have no foreign key to
device, so the history outlives a deregistered device.

  telemetry_scalar
    telemetry_scalar_y2026m11   [2026-11-01, 2026-12-01)  UTC
    telemetry_scalar_y2026m12   [2026-12-01, 2027-01-01)
    telemetry_scalar_default    rows outside every monthly partition

PostGIS lives in schema extensions, not public, so the later REVOKE on public
does not hide ST_Project from the demo roles.
*/

-- Superuser: the extension cannot be created by demo_owner.
CREATE SCHEMA IF NOT EXISTS extensions;
CREATE EXTENSION IF NOT EXISTS postgis SCHEMA extensions;
GRANT USAGE ON SCHEMA extensions TO demo_owner, demo_normal, demo_admin, demo_controller, demo_server;
SET search_path = public, extensions;

SET ROLE demo_owner;

-- ---------------------------------------------------------------------------
-- Devices
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS device_type (
    type_id            TEXT PRIMARY KEY,
    payload            TEXT NOT NULL CHECK (payload IN ('scalar', 'gps')),
    default_rate_ms    INT NOT NULL CHECK (default_rate_ms BETWEEN 50 AND 10000),
    default_amplitude  DOUBLE PRECISION NOT NULL CHECK (default_amplitude > 0 AND default_amplitude <= 1000),
    description        TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS device (
    device_id   INT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE CHECK (name ~ '^[a-z][a-z0-9_.-]{1,63}$'),
    type_id     TEXT NOT NULL REFERENCES device_type (type_id),
    cert_id     INT NOT NULL UNIQUE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS device_type_idx ON device (type_id);

-- Last sample. Exactly one of value / position, matching the device's payload.
CREATE TABLE IF NOT EXISTS device_state (
    device_id   INT PRIMARY KEY REFERENCES device (device_id) ON DELETE CASCADE,
    value       DOUBLE PRECISION,
    position    extensions.geography(Point, 4326),
    seq         BIGINT NOT NULL,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CHECK ((value IS NULL) <> (position IS NULL))
);

CREATE INDEX IF NOT EXISTS device_state_position_gix ON device_state USING GIST (position);

COMMENT ON TABLE device IS 'Registered devices, each bound to its current certificate. Created only by issue_device_certificate().';
COMMENT ON TABLE device_state IS 'Last sample of each device, written by the controller producer for that device.';

-- amplitude: peak value for the scalar types, orbit radius in metres for gps_orbit.
INSERT INTO device_type (type_id, payload, default_rate_ms, default_amplitude, description) VALUES
    ('sine',      'scalar', 500,  10.0,  'Sine wave'),
    ('sawtooth',  'scalar', 1000, 5.0,   'Sawtooth with noise'),
    ('gps_orbit', 'gps',    1000, 250.0, 'GPS fix orbiting a fixed origin (ST_Project)')
ON CONFLICT (type_id) DO NOTHING;

-- ---------------------------------------------------------------------------
-- Partitioned telemetry. The partition key must be part of the primary key.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS telemetry_scalar (
    history_id   BIGINT GENERATED ALWAYS AS IDENTITY,
    device_id    INT NOT NULL,
    value        DOUBLE PRECISION NOT NULL,
    seq          BIGINT NOT NULL,
    recorded_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (history_id, recorded_at)
) PARTITION BY RANGE (recorded_at);

CREATE TABLE IF NOT EXISTS telemetry_gps (
    history_id   BIGINT GENERATED ALWAYS AS IDENTITY,
    device_id    INT NOT NULL,
    position     extensions.geography(Point, 4326) NOT NULL,
    seq          BIGINT NOT NULL,
    recorded_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (history_id, recorded_at)
) PARTITION BY RANGE (recorded_at);

CREATE TABLE IF NOT EXISTS telemetry_scalar_default PARTITION OF telemetry_scalar DEFAULT;
CREATE TABLE IF NOT EXISTS telemetry_gps_default PARTITION OF telemetry_gps DEFAULT;

-- Declared on the parent; PostgreSQL creates it on every partition.
CREATE INDEX IF NOT EXISTS telemetry_scalar_device_time_idx ON telemetry_scalar (device_id, recorded_at DESC);
CREATE INDEX IF NOT EXISTS telemetry_gps_device_time_idx ON telemetry_gps (device_id, recorded_at DESC);
CREATE INDEX IF NOT EXISTS telemetry_gps_position_gix ON telemetry_gps USING GIST (position);

-- ---------------------------------------------------------------------------
-- Partition maintenance
-- ---------------------------------------------------------------------------

-- Creates the monthly partitions [from_month, from_month + months_ahead] that
-- do not exist yet and returns the names it created. Month boundaries are UTC.
-- If the DEFAULT partition already holds rows for a new month, they are moved
-- into the new partition in the same transaction.
-- SECURITY DEFINER: creating a partition requires owning the parent, and the
-- controller must not own tables.
CREATE OR REPLACE FUNCTION ensure_monthly_partitions(
    parent        regclass,
    from_month    date DEFAULT NULL,
    months_ahead  int  DEFAULT 2
)
RETURNS SETOF text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public, pg_temp
SET TimeZone = 'UTC'
AS $$
DECLARE
    parent_name  text;
    default_name text;
    month_start  date;
    lower_ts     timestamptz;
    upper_ts     timestamptz;
    child        text;
    moved        bigint;
BEGIN
    IF parent NOT IN ('public.telemetry_scalar'::regclass, 'public.telemetry_gps'::regclass) THEN
        RAISE EXCEPTION 'ensure_monthly_partitions: unsupported parent %', parent;
    END IF;
    IF months_ahead < 0 OR months_ahead > 24 THEN
        RAISE EXCEPTION 'ensure_monthly_partitions: months_ahead must be in [0, 24]';
    END IF;

    SELECT c.relname INTO parent_name FROM pg_class c WHERE c.oid = parent;
    default_name := parent_name || '_default';
    from_month := date_trunc('month', COALESCE(from_month, now()::date))::date;

    -- Serialise concurrent callers on the same parent.
    PERFORM pg_advisory_xact_lock(parent::oid::bigint);

    FOR i IN 0..months_ahead LOOP
        month_start := (from_month + make_interval(months => i))::date;
        lower_ts := month_start::timestamptz;
        upper_ts := (month_start + interval '1 month')::timestamptz;
        child := format('%s_y%sm%s', parent_name, to_char(month_start, 'YYYY'), to_char(month_start, 'MM'));

        CONTINUE WHEN to_regclass(format('public.%I', child)) IS NOT NULL;

        -- Rows for this month that already landed in DEFAULT block the new
        -- partition. Park them, create the partition, and route them back.
        EXECUTE format('CREATE TEMP TABLE pgdemo_moved (LIKE %s) ON COMMIT DROP', parent);
        EXECUTE format(
            'WITH d AS (DELETE FROM public.%I WHERE recorded_at >= %L AND recorded_at < %L RETURNING *) '
            'INSERT INTO pg_temp.pgdemo_moved SELECT * FROM d',
            default_name, lower_ts, upper_ts);
        GET DIAGNOSTICS moved = ROW_COUNT;

        EXECUTE format('CREATE TABLE public.%I PARTITION OF %s FOR VALUES FROM (%L) TO (%L)',
                       child, parent, lower_ts, upper_ts);

        EXECUTE format('INSERT INTO %s OVERRIDING SYSTEM VALUE SELECT * FROM pg_temp.pgdemo_moved', parent);
        DROP TABLE pg_temp.pgdemo_moved;

        IF moved > 0 THEN
            RAISE NOTICE 'ensure_monthly_partitions: moved % row(s) from % into %', moved, default_name, child;
        END IF;
        RETURN NEXT child;
    END LOOP;
END
$$;

-- Drops every monthly partition whose whole month ends on or before cutoff and
-- returns the names it dropped. Retention without DELETE or VACUUM.
CREATE OR REPLACE FUNCTION drop_monthly_partitions_before(parent regclass, cutoff date)
RETURNS SETOF text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
SET TimeZone = 'UTC'
AS $$
DECLARE
    part   record;
    ym     text[];
    m      date;
BEGIN
    IF parent NOT IN ('public.telemetry_scalar'::regclass, 'public.telemetry_gps'::regclass) THEN
        RAISE EXCEPTION 'drop_monthly_partitions_before: unsupported parent %', parent;
    END IF;

    PERFORM pg_advisory_xact_lock(parent::oid::bigint);

    FOR part IN
        SELECT c.relname
        FROM pg_inherits i
        JOIN pg_class c ON c.oid = i.inhrelid
        WHERE i.inhparent = parent
        ORDER BY c.relname
    LOOP
        ym := regexp_match(part.relname, '_y(\d{4})m(\d{2})$');
        CONTINUE WHEN ym IS NULL;
        m := make_date(ym[1]::int, ym[2]::int, 1);
        IF (m + interval '1 month')::date <= cutoff THEN
            EXECUTE format('DROP TABLE public.%I', part.relname);
            RETURN NEXT part.relname;
        END IF;
    END LOOP;
END
$$;

-- Current month plus two ahead at install time. The controller calls the
-- same function at startup and every --maintenance-interval-s.
SELECT ensure_monthly_partitions('telemetry_scalar');
SELECT ensure_monthly_partitions('telemetry_gps');

RESET ROLE;
