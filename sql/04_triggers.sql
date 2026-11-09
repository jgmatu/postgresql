/*
=============================================================================
04_triggers.sql - reactive layer: NOTIFY channels
=============================================================================

Channels are per topic, never per device: the device travels in the payload,
so the LISTEN set stays the same however many devices are registered.

  Channel            Source                          Listener     Meaning
  -----------------  ------------------------------  -----------  ------------------------------
  telemetry_scalar   device_state INSERT/UPDATE      server       new sample of a scalar device
  telemetry_gps      device_state INSERT/UPDATE      server       new fix of a GPS device
  device_events      device INSERT/DELETE            controller,  device registered or removed
                                                     server
  config_requested   device_config_pending INSERT    controller   new desired configuration
  config_events      device_config INSERT/UPDATE     server       configuration applied
  error_events       device_config_errors INSERT     server       configuration rejected

pg_notify() is transactional: the notification leaves only when the writing
transaction commits, so a listener never sees state that might roll back.
NOTIFY is a wake-up signal, not the message store. Payloads stay well under
the 8000-byte limit, and a listener that was down catches up by reading the
tables (the controller drains PENDING rows and starts every device at startup).

One notification per sample is fine for a demo. Committing a NOTIFY takes a
cluster-wide lock on the notification queue, so a fleet writing thousands of
samples per second needs batched wake-ups, logical decoding or a broker
(see sql/README.md, Scaling notes).
*/

SET ROLE demo_owner;

-- ---------------------------------------------------------------------------
-- Data plane: one trigger, one channel per payload
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION notify_device_sample()
RETURNS trigger
LANGUAGE plpgsql
AS $$
DECLARE
    dev_name text;
BEGIN
    SELECT d.name INTO dev_name FROM public.device d WHERE d.device_id = NEW.device_id;
    IF NEW.position IS NULL THEN
        PERFORM pg_notify('telemetry_scalar', json_build_object(
            'device_id', NEW.device_id,
            'device',    dev_name,
            'action',    TG_OP,
            'value',     NEW.value,
            'seq',       NEW.seq,
            'ts',        NEW.updated_at
        )::text);
    ELSE
        PERFORM pg_notify('telemetry_gps', json_build_object(
            'device_id', NEW.device_id,
            'device',    dev_name,
            'action',    TG_OP,
            'lon',       extensions.ST_X(NEW.position::extensions.geometry),
            'lat',       extensions.ST_Y(NEW.position::extensions.geometry),
            'seq',       NEW.seq,
            'ts',        NEW.updated_at
        )::text);
    END IF;
    RETURN NULL;
END
$$;

CREATE OR REPLACE TRIGGER trg_device_state_notify
    AFTER INSERT OR UPDATE ON device_state
    FOR EACH ROW EXECUTE FUNCTION notify_device_sample();

-- ---------------------------------------------------------------------------
-- Device registry
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION notify_device_change()
RETURNS trigger
LANGUAGE plpgsql
AS $$
DECLARE
    d record;
BEGIN
    IF TG_OP = 'DELETE' THEN d := OLD; ELSE d := NEW; END IF;
    PERFORM pg_notify('device_events', json_build_object(
        'action',    TG_OP,
        'device_id', d.device_id,
        'device',    d.name,
        'type_id',   d.type_id
    )::text);
    RETURN NULL;
END
$$;

CREATE OR REPLACE TRIGGER trg_device_notify
    AFTER INSERT OR DELETE ON device
    FOR EACH ROW EXECUTE FUNCTION notify_device_change();

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
        'device_id',  NEW.device_id
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
        'device_id',  NEW.device_id,
        'device',     (SELECT d.name FROM public.device d WHERE d.device_id = NEW.device_id),
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
        'device_id',    NEW.device_id,
        'device',       (SELECT d.name FROM public.device d WHERE d.device_id = NEW.device_id),
        'error_code',   NEW.error_code,
        'error_detail', NEW.error_detail,
        'occurred_at',  NEW.occurred_at
    )::text);
    RETURN NULL;
END
$$;

CREATE OR REPLACE TRIGGER trg_config_pending_notify
    AFTER INSERT ON device_config_pending
    FOR EACH ROW EXECUTE FUNCTION notify_config_request();

CREATE OR REPLACE TRIGGER trg_config_notify
    AFTER INSERT OR UPDATE ON device_config
    FOR EACH ROW EXECUTE FUNCTION notify_config_change();

CREATE OR REPLACE TRIGGER trg_config_error_notify
    AFTER INSERT ON device_config_errors
    FOR EACH ROW EXECUTE FUNCTION notify_config_error();

RESET ROLE;
