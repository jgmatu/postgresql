/*
=============================================================================
01_data.sql - data plane: three live tables and their monthly-partitioned history
=============================================================================

dummy1, dummy2 and dummy3 are deliberately separate tables. Each one has its
own trigger and its own NOTIFY channel (04_triggers.sql), so a single LISTEN
connection can multiplex them and dispatch each event to its own callback.

dummy3 stores a GPS point. PostGIS lives in schema extensions, not public,
so the later REVOKE on public does not hide ST_Project from the demo roles.

Each live row is a snapshot (one row per item_id, UPSERT). Every write also
appends a row to <table>_history in the same transaction. History tables are
range-partitioned by month on recorded_at:

  dummy1_history
    dummy1_history_y2026m11   [2026-11-01, 2026-12-01)  UTC
    dummy1_history_y2026m12   [2026-12-01, 2027-01-01)
    dummy1_history_default    rows outside every monthly partition

Queries with a recorded_at range only touch the matching partitions
(partition pruning). Retention drops whole partitions instead of DELETE.
*/

-- Superuser: the extension cannot be created by demo_owner.
CREATE SCHEMA IF NOT EXISTS extensions;
CREATE EXTENSION IF NOT EXISTS postgis SCHEMA extensions;
GRANT USAGE ON SCHEMA extensions TO demo_owner, demo_normal, demo_admin, demo_controller, demo_server;
SET search_path = public, extensions;

SET ROLE demo_owner;

-- ---------------------------------------------------------------------------
-- Live snapshots
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS dummy1 (
    item_id     INT PRIMARY KEY CHECK (item_id > 0),
    value       DOUBLE PRECISION NOT NULL,
    seq         BIGINT NOT NULL,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE TABLE IF NOT EXISTS dummy2 (
    item_id     INT PRIMARY KEY CHECK (item_id > 0),
    value       DOUBLE PRECISION NOT NULL,
    seq         BIGINT NOT NULL,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

-- GPS fix. amplitude on this target is the orbit radius in metres.
CREATE TABLE IF NOT EXISTS dummy3 (
    item_id     INT PRIMARY KEY CHECK (item_id > 0),
    position    extensions.geography(Point, 4326) NOT NULL,
    seq         BIGINT NOT NULL,
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

COMMENT ON TABLE dummy1 IS 'Live snapshot written by the controller producer thread for dummy1 (sine wave).';
COMMENT ON TABLE dummy2 IS 'Live snapshot written by the controller producer thread for dummy2 (counter with noise).';
COMMENT ON TABLE dummy3 IS 'Live GPS fix written by the controller producer thread for dummy3 (orbit around a fixed origin).';

-- ---------------------------------------------------------------------------
-- Partitioned history. The partition key must be part of the primary key.
-- History is append-only and has no foreign key to the live table, so it
-- outlives deleted items.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS dummy1_history (
    history_id   BIGINT GENERATED ALWAYS AS IDENTITY,
    item_id      INT NOT NULL,
    value        DOUBLE PRECISION NOT NULL,
    seq          BIGINT NOT NULL,
    recorded_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (history_id, recorded_at)
) PARTITION BY RANGE (recorded_at);

CREATE TABLE IF NOT EXISTS dummy2_history (
    history_id   BIGINT GENERATED ALWAYS AS IDENTITY,
    item_id      INT NOT NULL,
    value        DOUBLE PRECISION NOT NULL,
    seq          BIGINT NOT NULL,
    recorded_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (history_id, recorded_at)
) PARTITION BY RANGE (recorded_at);

CREATE TABLE IF NOT EXISTS dummy3_history (
    history_id   BIGINT GENERATED ALWAYS AS IDENTITY,
    item_id      INT NOT NULL,
    position     extensions.geography(Point, 4326) NOT NULL,
    seq          BIGINT NOT NULL,
    recorded_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (history_id, recorded_at)
) PARTITION BY RANGE (recorded_at);

CREATE TABLE IF NOT EXISTS dummy1_history_default PARTITION OF dummy1_history DEFAULT;
CREATE TABLE IF NOT EXISTS dummy2_history_default PARTITION OF dummy2_history DEFAULT;
CREATE TABLE IF NOT EXISTS dummy3_history_default PARTITION OF dummy3_history DEFAULT;

-- Declared on the parent; PostgreSQL creates it on every partition.
CREATE INDEX IF NOT EXISTS dummy1_history_item_time_idx ON dummy1_history (item_id, recorded_at DESC);
CREATE INDEX IF NOT EXISTS dummy2_history_item_time_idx ON dummy2_history (item_id, recorded_at DESC);
CREATE INDEX IF NOT EXISTS dummy3_history_item_time_idx ON dummy3_history (item_id, recorded_at DESC);
CREATE INDEX IF NOT EXISTS dummy3_position_gix ON dummy3 USING GIST (position);
CREATE INDEX IF NOT EXISTS dummy3_history_position_gix ON dummy3_history USING GIST (position);

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
    IF parent NOT IN ('public.dummy1_history'::regclass, 'public.dummy2_history'::regclass,
                      'public.dummy3_history'::regclass) THEN
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
    IF parent NOT IN ('public.dummy1_history'::regclass, 'public.dummy2_history'::regclass,
                      'public.dummy3_history'::regclass) THEN
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
-- same function at startup and once a day.
SELECT ensure_monthly_partitions('dummy1_history');
SELECT ensure_monthly_partitions('dummy2_history');
SELECT ensure_monthly_partitions('dummy3_history');

RESET ROLE;
