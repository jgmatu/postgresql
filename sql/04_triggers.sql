/*
=============================================================================
04_triggers.sql - reactive layer: NOTIFY channels
=============================================================================

Channels are per topic, never per device: the device travels in the payload,
so the LISTEN set stays the same however many devices are registered.

  Channel            Source                          Listener     Meaning
  -----------------  ------------------------------  -----------  ------------------------------
  telemetry_scalar      device_state INSERT/UPDATE       server       new sample of a scalar device
  telemetry_gps         device_state INSERT/UPDATE       server       new fix of a GPS device
  device_events         device INSERT/DELETE             controller,  INSERT, DELETE
                        device UPDATE OF cert_id         server       ROTATED
                        crl INSERT/UPDATE/DELETE                      REVOKED, UNREVOKED
                        device_session state                          STARTING, ONLINE, OFFLINE,
                                                                      UNMANAGED, AUTH_FAILED, DISABLED
  enrollment_requested  device_enrollment INSERT         controller   start a device process to enroll
  enrollment_csr        status -> CSR_SUBMITTED          server       sign the CSR
  enrollment_events     status -> ISSUED/REJECTED/EXPIRED controller, enrollment finished
                                                         server
  config_requested      device_config_pending INSERT     controller   new desired configuration
  config_events         device_config INSERT/UPDATE      server       configuration applied
  error_events          device_config_errors INSERT      server       configuration rejected
                        device_session AUTH_FAILED                    device failed authentication

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
        'action',    CASE TG_OP WHEN 'UPDATE' THEN 'ROTATED' ELSE TG_OP END,
        'device_id', d.device_id,
        'device',    d.name,
        'type_id',   d.type_id,
        'cert_id',   d.cert_id
    )::text);
    RETURN NULL;
END
$$;

CREATE OR REPLACE TRIGGER trg_device_notify
    AFTER INSERT OR DELETE ON device
    FOR EACH ROW EXECUTE FUNCTION notify_device_change();

CREATE OR REPLACE TRIGGER trg_device_rotate_notify
    AFTER UPDATE OF cert_id ON device
    FOR EACH ROW WHEN (OLD.cert_id IS DISTINCT FROM NEW.cert_id)
    EXECUTE FUNCTION notify_device_change();

-- ---------------------------------------------------------------------------
-- Device identity
-- ---------------------------------------------------------------------------

-- `current` says whether the certificate is the device's current one: only
-- then does the revocation change what the device may do.
CREATE OR REPLACE FUNCTION notify_crl_change()
RETURNS trigger
LANGUAGE plpgsql
AS $$
DECLARE
    r           record;
    dev_id      int;
    is_current  boolean;
BEGIN
    IF TG_OP = 'DELETE' THEN r := OLD; ELSE r := NEW; END IF;
    SELECT c.device_id, (d.cert_id = c.cert_id) INTO dev_id, is_current
    FROM public.device_certificate c
    LEFT JOIN public.device d ON d.device_id = c.device_id
    WHERE c.ca_id = r.ca_id AND c.serial = r.serial;
    PERFORM pg_notify('device_events', json_build_object(
        'action',    CASE TG_OP WHEN 'DELETE' THEN 'UNREVOKED' ELSE 'REVOKED' END,
        'device_id', dev_id,
        'device',    r.device_name,
        'serial',    r.serial,
        'reason',    r.reason,
        'current',   COALESCE(is_current, FALSE)
    )::text);
    RETURN NULL;
END
$$;

CREATE OR REPLACE TRIGGER trg_crl_notify
    AFTER INSERT OR UPDATE OR DELETE ON crl
    FOR EACH ROW EXECUTE FUNCTION notify_crl_change();

CREATE OR REPLACE FUNCTION notify_session_change()
RETURNS trigger
LANGUAGE plpgsql
AS $$
DECLARE
    dev_name text;
BEGIN
    IF TG_OP = 'UPDATE' AND OLD.state = NEW.state THEN RETURN NULL; END IF;
    SELECT d.name INTO dev_name FROM public.device d WHERE d.device_id = NEW.device_id;
    PERFORM pg_notify('device_events', json_build_object(
        'action',     NEW.state,
        'device_id',  NEW.device_id,
        'device',     dev_name,
        'pid',        NEW.pid,
        'last_error', NEW.last_error
    )::text);
    IF NEW.state = 'AUTH_FAILED' THEN
        PERFORM pg_notify('error_events', json_build_object(
            'device_id',    NEW.device_id,
            'device',       dev_name,
            'error_code',   'AUTH_FAILED',
            'error_detail', NEW.last_error,
            'occurred_at',  NEW.updated_at
        )::text);
    END IF;
    RETURN NULL;
END
$$;

CREATE OR REPLACE TRIGGER trg_device_session_notify
    AFTER INSERT OR UPDATE OF state ON device_session
    FOR EACH ROW EXECUTE FUNCTION notify_session_change();

CREATE OR REPLACE FUNCTION notify_enrollment()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    IF TG_OP = 'INSERT' THEN
        PERFORM pg_notify('enrollment_requested', json_build_object(
            'request_id', NEW.request_id,
            'kind',       NEW.kind,
            'device',     NEW.name,
            'device_id',  NEW.device_id
        )::text);
    ELSIF NEW.status = 'CSR_SUBMITTED' THEN
        PERFORM pg_notify('enrollment_csr', json_build_object(
            'request_id', NEW.request_id,
            'device',     NEW.name
        )::text);
    ELSE
        PERFORM pg_notify('enrollment_events', json_build_object(
            'request_id', NEW.request_id,
            'kind',       NEW.kind,
            'device',     NEW.name,
            'status',     NEW.status,
            'device_id',  NEW.device_id,
            'cert_id',    NEW.cert_id,
            'error_code', NEW.error_code
        )::text);
    END IF;
    RETURN NULL;
END
$$;

CREATE OR REPLACE TRIGGER trg_device_enrollment_notify
    AFTER INSERT ON device_enrollment
    FOR EACH ROW EXECUTE FUNCTION notify_enrollment();

CREATE OR REPLACE TRIGGER trg_device_enrollment_status_notify
    AFTER UPDATE OF status ON device_enrollment
    FOR EACH ROW WHEN (OLD.status IS DISTINCT FROM NEW.status)
    EXECUTE FUNCTION notify_enrollment();

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
