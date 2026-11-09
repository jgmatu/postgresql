/*
=============================================================================
04_triggers.sql - reactive layer: NOTIFY channels
=============================================================================

  Channel            Source                         Listener     Meaning
  -----------------  -----------------------------  -----------  ----------------------------
  dummy1_events      dummy1 INSERT/UPDATE           server       new dummy1 sample
  dummy2_events      dummy2 INSERT/UPDATE           server       new dummy2 sample
  dummy3_events      dummy3 INSERT/UPDATE           server       new dummy3 GPS fix
  config_requested   dummy_config_pending INSERT    controller   new desired configuration
  config_events      dummy_config INSERT/UPDATE     server       configuration applied
  error_events       dummy_config_errors INSERT     server       configuration rejected

pg_notify() is transactional: the notification leaves only when the writing
transaction commits, so a listener never sees state that might roll back.
NOTIFY is a wake-up signal, not the message store. Payloads stay well under
the 8000-byte limit, and a listener that was down catches up by reading the
tables (the controller drains PENDING rows at startup).
*/

SET ROLE demo_owner;

-- ---------------------------------------------------------------------------
-- Data plane: one function and one channel per table
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION notify_dummy1_change()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_notify('dummy1_events', json_build_object(
        'table',   'dummy1',
        'action',  TG_OP,
        'item_id', NEW.item_id,
        'value',   NEW.value,
        'seq',     NEW.seq,
        'ts',      NEW.updated_at
    )::text);
    RETURN NULL;
END
$$;

CREATE OR REPLACE FUNCTION notify_dummy2_change()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_notify('dummy2_events', json_build_object(
        'table',   'dummy2',
        'action',  TG_OP,
        'item_id', NEW.item_id,
        'value',   NEW.value,
        'seq',     NEW.seq,
        'ts',      NEW.updated_at
    )::text);
    RETURN NULL;
END
$$;

CREATE OR REPLACE TRIGGER trg_dummy1_notify
    AFTER INSERT OR UPDATE ON dummy1
    FOR EACH ROW EXECUTE FUNCTION notify_dummy1_change();

CREATE OR REPLACE TRIGGER trg_dummy2_notify
    AFTER INSERT OR UPDATE ON dummy2
    FOR EACH ROW EXECUTE FUNCTION notify_dummy2_change();

CREATE OR REPLACE FUNCTION notify_dummy3_change()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_notify('dummy3_events', json_build_object(
        'table',   'dummy3',
        'action',  TG_OP,
        'item_id', NEW.item_id,
        'lon',     ST_X(NEW.position::geometry),
        'lat',     ST_Y(NEW.position::geometry),
        'seq',     NEW.seq,
        'ts',      NEW.updated_at
    )::text);
    RETURN NULL;
END
$$;

CREATE OR REPLACE TRIGGER trg_dummy3_notify
    AFTER INSERT OR UPDATE ON dummy3
    FOR EACH ROW EXECUTE FUNCTION notify_dummy3_change();

-- ---------------------------------------------------------------------------
-- Control plane
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION notify_config_request()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_notify('config_requested', json_build_object(
        'action',     TG_OP,
        'request_id', NEW.request_id,
        'target',     NEW.target
    )::text);
    RETURN NULL;
END
$$;

CREATE OR REPLACE FUNCTION notify_config_change()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_notify('config_events', json_build_object(
        'action',     TG_OP,
        'target',     NEW.target,
        'rate_ms',    NEW.rate_ms,
        'amplitude',  NEW.amplitude,
        'is_active',  NEW.is_active,
        'request_id', NEW.request_id,
        'applied_at', NEW.applied_at
    )::text);
    RETURN NULL;
END
$$;

CREATE OR REPLACE FUNCTION notify_config_error()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM pg_notify('error_events', json_build_object(
        'request_id',   NEW.request_id,
        'target',       NEW.target,
        'error_code',   NEW.error_code,
        'error_detail', NEW.error_detail,
        'occurred_at',  NEW.occurred_at
    )::text);
    RETURN NULL;
END
$$;

CREATE OR REPLACE TRIGGER trg_config_pending_notify
    AFTER INSERT ON dummy_config_pending
    FOR EACH ROW EXECUTE FUNCTION notify_config_request();

CREATE OR REPLACE TRIGGER trg_config_notify
    AFTER INSERT OR UPDATE ON dummy_config
    FOR EACH ROW EXECUTE FUNCTION notify_config_change();

CREATE OR REPLACE TRIGGER trg_config_error_notify
    AFTER INSERT ON dummy_config_errors
    FOR EACH ROW EXECUTE FUNCTION notify_config_error();

RESET ROLE;
