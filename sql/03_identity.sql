/*
=============================================================================
03_identity.sql - device identity: root CA, X.509 certificates, CRL, enrollment
=============================================================================

Every device is bound to an X.509 certificate (ECDSA P-384) issued by the
internal root CA. The CA private key never reaches the database: the server
holds it in a file and signs CSRs; only public material is stored here.

  root_ca             trusted CA certificates (public part only). The row
                      named factory is the simulation root; its private key
                      stays in run/factory and never reaches this database.
  device_certificate  every certificate a device has ever had (1..n per device).
                      A worker issued by the factory is a row here, ca_id pointing
                      at that root.
  crl                 revoked certificates of the CA that issued them. The
                      manager's own list is manager_revocation, not this table.
  manager_certificate the manager's own root. It signs the CRL and does not
                      reference root_ca. The factory root is the other certificate
                      the manager keeps.
  manager_revocation  workers the manager has revoked. No foreign key to
                      device_certificate: the row outlives the certificate.
  manager_crl_signature  the manager's signature over that list
  device_enrollment   queue of enrollments and rotations
                      (PENDING -> CSR_SUBMITTED -> ISSUED | REJECTED | EXPIRED)
  device_session      what the controller knows about each device process

Binding rules, enforced by foreign keys:

  device.(cert_id, device_id) -> device_certificate.(cert_id, device_id)
      NOT NULL: a device always has a current certificate, and it is its own.
      ON DELETE CASCADE: deleting the current certificate deletes the device
      (with its state, configuration, queue and session).
  device_certificate.device_id -> device.device_id
      ON DELETE CASCADE: deleting a device deletes its certificates. Deleting
      a previous (not current) certificate touches nothing else.

Both keys are DEFERRABLE INITIALLY DEFERRED, so issue_device_certificate()
can insert the device and its first certificate in one transaction.

A revoked current certificate is fine: the device stays, but the controller
stops it. That is how a device is taken out of service without deleting it.
certificateHold is the reversible form: only those CRL rows may be deleted.

The NOTIFY triggers live in 04_triggers.sql:

  INSERT device_enrollment             -> enrollment_requested  (server -> controller)
  status CSR_SUBMITTED                 -> enrollment_csr        (controller -> server)
  status ISSUED | REJECTED | EXPIRED   -> enrollment_events     (both)
  crl, device_session, device.cert_id  -> device_events
*/

SET ROLE demo_owner;

CREATE TABLE IF NOT EXISTS root_ca (
    ca_id               INT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name                TEXT NOT NULL UNIQUE,
    subject             TEXT NOT NULL,
    fingerprint_sha256  TEXT NOT NULL UNIQUE CHECK (fingerprint_sha256 ~ '^[0-9a-f]{64}$'),
    cert_pem            TEXT NOT NULL CHECK (cert_pem LIKE '-----BEGIN CERTIFICATE-----%'),
    not_before          TIMESTAMPTZ NOT NULL,
    not_after           TIMESTAMPTZ NOT NULL,
    is_active           BOOLEAN NOT NULL DEFAULT TRUE,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (not_before < not_after)
);

CREATE TABLE IF NOT EXISTS device_certificate (
    cert_id             INT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    device_id           INT NOT NULL,
    ca_id               INT NOT NULL REFERENCES root_ca (ca_id) ON DELETE RESTRICT,
    serial              TEXT NOT NULL CHECK (serial ~ '^[0-9a-f]{1,40}$'),
    fingerprint_sha256  TEXT NOT NULL UNIQUE CHECK (fingerprint_sha256 ~ '^[0-9a-f]{64}$'),
    subject_cn          TEXT NOT NULL,
    not_before          TIMESTAMPTZ NOT NULL,
    not_after           TIMESTAMPTZ NOT NULL,
    cert_pem            TEXT NOT NULL CHECK (cert_pem LIKE '-----BEGIN CERTIFICATE-----%'),
    issued_at           TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (ca_id, serial),
    UNIQUE (cert_id, device_id),
    CONSTRAINT device_certificate_device_fk FOREIGN KEY (device_id)
        REFERENCES device (device_id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED
);

CREATE INDEX IF NOT EXISTS device_certificate_device_idx
    ON device_certificate (device_id, issued_at DESC);

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'device_current_cert_fk') THEN
        ALTER TABLE device ADD CONSTRAINT device_current_cert_fk FOREIGN KEY (cert_id, device_id)
            REFERENCES device_certificate (cert_id, device_id)
            ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED;
    END IF;
END
$$;

-- The certificate keeps its PEM so the server can rebuild a signed X.509 CRL
-- even after the certificate row is gone.
CREATE TABLE IF NOT EXISTS crl (
    ca_id               INT NOT NULL REFERENCES root_ca (ca_id),
    serial              TEXT NOT NULL,
    fingerprint_sha256  TEXT NOT NULL UNIQUE,
    device_name         TEXT NOT NULL,
    cert_pem            TEXT NOT NULL,
    reason              TEXT NOT NULL CHECK (reason IN ('unspecified', 'keyCompromise', 'affiliationChanged',
                                                        'superseded', 'cessationOfOperation', 'certificateHold')),
    revoked_at          TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    revoked_by          TEXT NOT NULL,
    PRIMARY KEY (ca_id, serial)
);

CREATE TABLE IF NOT EXISTS device_enrollment (
    request_id    BIGINT PRIMARY KEY DEFAULT nextval('request_id_seq'),
    kind          TEXT NOT NULL CHECK (kind IN ('ENROLL', 'ROTATE')),
    name          TEXT NOT NULL CHECK (name ~ '^[a-z][a-z0-9_.-]{1,63}$'),
    type_id       TEXT NOT NULL REFERENCES device_type (type_id),
    -- ROTATE: the device being re-keyed. ENROLL: set when the device is created.
    -- SET NULL keeps the audit row when the device is removed.
    device_id     INT REFERENCES device (device_id) ON DELETE SET NULL,
    status        TEXT NOT NULL DEFAULT 'PENDING'
                  CHECK (status IN ('PENDING', 'CSR_SUBMITTED', 'ISSUED', 'REJECTED', 'EXPIRED')),
    csr_pem       TEXT CHECK (csr_pem LIKE '-----BEGIN CERTIFICATE REQUEST-----%'),
    cert_id       INT,
    error_code    TEXT,
    error_detail  TEXT,
    requested_by  TEXT NOT NULL,
    requested_at  TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    csr_at        TIMESTAMPTZ,
    decided_at    TIMESTAMPTZ
);

-- At most one open enrollment or rotation per name.
CREATE UNIQUE INDEX IF NOT EXISTS device_enrollment_open_idx
    ON device_enrollment (name) WHERE status IN ('PENDING', 'CSR_SUBMITTED');
CREATE INDEX IF NOT EXISTS device_enrollment_queue_idx
    ON device_enrollment (requested_at) WHERE status = 'PENDING';

CREATE TABLE IF NOT EXISTS device_session (
    device_id     INT PRIMARY KEY REFERENCES device (device_id) ON DELETE CASCADE,
    state         TEXT NOT NULL CHECK (state IN ('STARTING', 'ONLINE', 'OFFLINE', 'UNMANAGED',
                                                 'AUTH_FAILED', 'DISABLED')),
    pid           INT,
    connected_at  TIMESTAMPTZ,
    last_seen     TIMESTAMPTZ,
    last_error    TEXT,
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

COMMENT ON TABLE root_ca IS 'Trusted CA certificates. The private key stays in the server''s files.';
COMMENT ON TABLE device_certificate IS 'Every certificate of every device; device.cert_id marks the current one.';
COMMENT ON TABLE crl IS 'Revoked certificates. Survives certificate deletion; only certificateHold rows may be removed.';
COMMENT ON TABLE device_enrollment IS 'Enrollment and rotation queue: the device process submits a CSR, the server signs it.';
COMMENT ON TABLE device_session IS 'Process state of each device, written by the controller.';

-- ---------------------------------------------------------------------------
-- Invariants
-- ---------------------------------------------------------------------------

-- The certificate names the device it belongs to.
CREATE OR REPLACE FUNCTION check_certificate_subject()
RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, public
AS $$
DECLARE
    dev_name text;
BEGIN
    SELECT d.name INTO dev_name FROM public.device d WHERE d.device_id = NEW.device_id;
    IF dev_name IS DISTINCT FROM NEW.subject_cn THEN
        RAISE EXCEPTION 'certificate CN % does not name device %', NEW.subject_cn, NEW.device_id
            USING ERRCODE = 'check_violation';
    END IF;
    RETURN NEW;
END
$$;

CREATE OR REPLACE TRIGGER trg_device_certificate_subject
    BEFORE INSERT ON device_certificate
    FOR EACH ROW EXECUTE FUNCTION check_certificate_subject();

-- Revocation is permanent, except certificateHold, which may be lifted
-- (DELETE) or made permanent (UPDATE to another reason).
CREATE OR REPLACE FUNCTION protect_crl()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    IF OLD.reason <> 'certificateHold' THEN
        RAISE EXCEPTION 'revocation of serial % is permanent (%)', OLD.serial, OLD.reason
            USING ERRCODE = 'check_violation';
    END IF;
    IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
    RETURN NEW;
END
$$;

CREATE OR REPLACE TRIGGER trg_crl_protect
    BEFORE UPDATE OR DELETE ON crl
    FOR EACH ROW EXECUTE FUNCTION protect_crl();

-- New requests: an enrollment needs a free name; a rotation takes name and
-- type from its device. Status always starts at PENDING.
CREATE OR REPLACE FUNCTION check_enrollment()
RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, public
AS $$
BEGIN
    IF NEW.kind = 'ENROLL' THEN
        IF EXISTS (SELECT 1 FROM public.device d WHERE d.name = NEW.name) THEN
            RAISE EXCEPTION 'device % already exists', NEW.name USING ERRCODE = 'unique_violation';
        END IF;
        NEW.device_id := NULL;
    ELSE
        SELECT d.name, d.type_id INTO NEW.name, NEW.type_id
        FROM public.device d WHERE d.device_id = NEW.device_id;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'rotation of unknown device %', NEW.device_id
                USING ERRCODE = 'foreign_key_violation';
        END IF;
    END IF;
    NEW.status := 'PENDING';
    NEW.csr_pem := NULL;
    NEW.cert_id := NULL;
    RETURN NEW;
END
$$;

CREATE OR REPLACE TRIGGER trg_device_enrollment_check
    BEFORE INSERT ON device_enrollment
    FOR EACH ROW EXECUTE FUNCTION check_enrollment();

-- PENDING -> CSR_SUBMITTED | REJECTED | EXPIRED, CSR_SUBMITTED -> ISSUED | REJECTED | EXPIRED.
CREATE OR REPLACE FUNCTION check_enrollment_transition()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    IF OLD.status = NEW.status THEN RETURN NEW; END IF;
    IF (OLD.status = 'PENDING' AND NEW.status IN ('CSR_SUBMITTED', 'REJECTED', 'EXPIRED'))
       OR (OLD.status = 'CSR_SUBMITTED' AND NEW.status IN ('ISSUED', 'REJECTED', 'EXPIRED')) THEN
        RETURN NEW;
    END IF;
    RAISE EXCEPTION 'enrollment % cannot go from % to %', OLD.request_id, OLD.status, NEW.status
        USING ERRCODE = 'check_violation';
END
$$;

CREATE OR REPLACE TRIGGER trg_device_enrollment_transition
    BEFORE UPDATE OF status ON device_enrollment
    FOR EACH ROW EXECUTE FUNCTION check_enrollment_transition();

-- ---------------------------------------------------------------------------
-- Issuing
-- ---------------------------------------------------------------------------

-- Records a certificate the server has just signed for an enrollment in
-- CSR_SUBMITTED, all in one transaction:
--   ENROLL: creates the device, its first certificate and its type defaults
--           in device_config (+ history).
--   ROTATE: adds the certificate, makes it current, and revokes the previous
--           one as superseded (unless it was already revoked).
-- The server verified the CSR; this function checks the CN, the CA and the
-- state of the request again. SECURITY DEFINER: the server may call this but
-- may not insert into device or device_certificate.
CREATE OR REPLACE FUNCTION issue_device_certificate(
    p_request_id      bigint,
    p_ca_fingerprint  text,
    p_serial          text,
    p_fingerprint     text,
    p_subject_cn      text,
    p_not_before      timestamptz,
    p_not_after       timestamptz,
    p_cert_pem        text
)
RETURNS TABLE (issued_device_id int, issued_cert_id int)
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
DECLARE
    e       public.device_enrollment;
    v_ca    int;
    v_cert  int;
    v_dev   int;
    v_old   int;
BEGIN
    SELECT * INTO e FROM public.device_enrollment x WHERE x.request_id = p_request_id FOR UPDATE;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'no enrollment %', p_request_id USING ERRCODE = 'no_data_found';
    END IF;
    IF e.status <> 'CSR_SUBMITTED' THEN
        RAISE EXCEPTION 'enrollment % is %, not CSR_SUBMITTED', p_request_id, e.status
            USING ERRCODE = 'check_violation';
    END IF;
    IF p_subject_cn <> e.name THEN
        RAISE EXCEPTION 'CN % does not match enrollment name %', p_subject_cn, e.name
            USING ERRCODE = 'check_violation';
    END IF;
    SELECT r.ca_id INTO v_ca FROM public.root_ca r
    WHERE r.fingerprint_sha256 = p_ca_fingerprint AND r.is_active;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'issuer % is not an active root CA', p_ca_fingerprint
            USING ERRCODE = 'check_violation';
    END IF;

    IF e.kind = 'ENROLL' THEN
        v_cert := nextval(pg_get_serial_sequence('public.device_certificate', 'cert_id'));
        INSERT INTO public.device (name, type_id, cert_id)
        VALUES (e.name, e.type_id, v_cert)
        RETURNING device_id INTO v_dev;
        INSERT INTO public.device_certificate
            (cert_id, device_id, ca_id, serial, fingerprint_sha256, subject_cn, not_before, not_after, cert_pem)
        OVERRIDING SYSTEM VALUE
        VALUES (v_cert, v_dev, v_ca, p_serial, p_fingerprint, p_subject_cn, p_not_before, p_not_after, p_cert_pem);
        INSERT INTO public.device_config (device_id, rate_ms, amplitude, is_active)
        SELECT v_dev, t.default_rate_ms, t.default_amplitude, TRUE
        FROM public.device_type t WHERE t.type_id = e.type_id;
        INSERT INTO public.device_config_history (device_id, rate_ms, amplitude, is_active)
        SELECT c.device_id, c.rate_ms, c.amplitude, c.is_active
        FROM public.device_config c WHERE c.device_id = v_dev;
    ELSE
        v_dev := e.device_id;
        SELECT d.cert_id INTO v_old FROM public.device d WHERE d.device_id = v_dev FOR UPDATE;
        IF NOT FOUND THEN
            RAISE EXCEPTION 'device of rotation % no longer exists', p_request_id
                USING ERRCODE = 'foreign_key_violation';
        END IF;
        INSERT INTO public.device_certificate
            (device_id, ca_id, serial, fingerprint_sha256, subject_cn, not_before, not_after, cert_pem)
        VALUES (v_dev, v_ca, p_serial, p_fingerprint, p_subject_cn, p_not_before, p_not_after, p_cert_pem)
        RETURNING cert_id INTO v_cert;
        UPDATE public.device d SET cert_id = v_cert WHERE d.device_id = v_dev;
        INSERT INTO public.crl (ca_id, serial, fingerprint_sha256, device_name, cert_pem, reason, revoked_by)
        SELECT c.ca_id, c.serial, c.fingerprint_sha256, e.name, c.cert_pem, 'superseded', e.requested_by
        FROM public.device_certificate c WHERE c.cert_id = v_old
        ON CONFLICT (ca_id, serial) DO NOTHING;
    END IF;

    UPDATE public.device_enrollment x
    SET status = 'ISSUED', cert_id = v_cert, device_id = v_dev, decided_at = clock_timestamp()
    WHERE x.request_id = p_request_id;

    issued_device_id := v_dev;
    issued_cert_id := v_cert;
    RETURN NEXT;
END
$$;

-- The server refuses a CSR. Returns false if the request was no longer open.
CREATE OR REPLACE FUNCTION reject_enrollment(p_request_id bigint, p_code text, p_detail text)
RETURNS boolean
LANGUAGE sql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
    UPDATE public.device_enrollment
    SET status = 'REJECTED', error_code = p_code, error_detail = p_detail, decided_at = clock_timestamp()
    WHERE request_id = p_request_id AND status IN ('PENDING', 'CSR_SUBMITTED')
    RETURNING TRUE
$$;

REVOKE ALL ON FUNCTION issue_device_certificate(bigint, text, text, text, text, timestamptz, timestamptz, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION reject_enrollment(bigint, text, text) FROM PUBLIC;

-- ---------------------------------------------------------------------------
-- Factory root, the manager certificate, and the manager's own revocation list.
-- Private keys are not stored. crl stays the revocation list of the issuing CA.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS manager_certificate (
    cert_id             INT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    serial              TEXT NOT NULL CHECK (serial ~ '^[0-9a-f]{1,40}$'),
    fingerprint_sha256  TEXT NOT NULL UNIQUE CHECK (fingerprint_sha256 ~ '^[0-9a-f]{64}$'),
    subject_cn          TEXT NOT NULL,
    not_before          TIMESTAMPTZ NOT NULL,
    not_after           TIMESTAMPTZ NOT NULL,
    cert_pem            TEXT NOT NULL CHECK (cert_pem LIKE '-----BEGIN CERTIFICATE-----%'),
    issued_at           TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CHECK (not_before < not_after)
);

-- A database created while this certificate still hung off root_ca drops that link.
ALTER TABLE manager_certificate DROP CONSTRAINT IF EXISTS manager_certificate_ca_id_fkey;
ALTER TABLE manager_certificate DROP CONSTRAINT IF EXISTS manager_certificate_ca_id_serial_key;
ALTER TABLE manager_certificate DROP COLUMN IF EXISTS ca_id;

CREATE UNIQUE INDEX IF NOT EXISTS manager_certificate_singleton ON manager_certificate ((TRUE));

CREATE TABLE IF NOT EXISTS manager_revocation (
    serial              TEXT PRIMARY KEY CHECK (serial ~ '^[0-9a-f]{1,40}$'),
    fingerprint_sha256  TEXT NOT NULL UNIQUE CHECK (fingerprint_sha256 ~ '^[0-9a-f]{64}$'),
    device_name         TEXT NOT NULL,
    cert_pem            TEXT NOT NULL CHECK (cert_pem LIKE '-----BEGIN CERTIFICATE-----%'),
    revoked_at          TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE TABLE IF NOT EXISTS manager_crl_signature (
    id          BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK (id),
    payload     TEXT NOT NULL,
    signature   TEXT NOT NULL CHECK (signature ~ '^[0-9a-f]+$'),
    signed_at   TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

-- Public certificate of the factory root. The name is the row the handshake selects.
CREATE OR REPLACE FUNCTION register_factory_root(
    p_subject     text,
    p_fingerprint text,
    p_cert_pem    text,
    p_not_before  timestamptz,
    p_not_after   timestamptz
) RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
BEGIN
    INSERT INTO public.root_ca (name, subject, fingerprint_sha256, cert_pem, not_before, not_after)
    VALUES ('factory', p_subject, p_fingerprint, p_cert_pem, p_not_before, p_not_after)
    ON CONFLICT (name) DO UPDATE
        SET subject = EXCLUDED.subject,
            fingerprint_sha256 = EXCLUDED.fingerprint_sha256,
            cert_pem = EXCLUDED.cert_pem,
            not_before = EXCLUDED.not_before,
            not_after = EXCLUDED.not_after
        WHERE root_ca.fingerprint_sha256 IS DISTINCT FROM EXCLUDED.fingerprint_sha256;
END
$$;

-- The manager's own root. A later call replaces the single row.
DROP FUNCTION IF EXISTS register_manager_certificate(text, text, text, text, timestamptz, timestamptz, text);

CREATE OR REPLACE FUNCTION register_manager_certificate(
    p_serial         text,
    p_fingerprint    text,
    p_subject_cn     text,
    p_not_before     timestamptz,
    p_not_after      timestamptz,
    p_cert_pem       text
) RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
BEGIN
    IF EXISTS (SELECT 1 FROM public.manager_certificate) THEN
        UPDATE public.manager_certificate
           SET serial = p_serial, fingerprint_sha256 = p_fingerprint,
               subject_cn = p_subject_cn, not_before = p_not_before, not_after = p_not_after,
               cert_pem = p_cert_pem, issued_at = clock_timestamp();
        RETURN;
    END IF;
    INSERT INTO public.manager_certificate
        (serial, fingerprint_sha256, subject_cn, not_before, not_after, cert_pem)
    VALUES (p_serial, p_fingerprint, p_subject_cn, p_not_before, p_not_after, p_cert_pem);
END
$$;

-- One signature over the canonical list of manager_revocation serials.
CREATE OR REPLACE FUNCTION replace_manager_crl(p_payload text, p_signature text)
RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
BEGIN
    INSERT INTO public.manager_crl_signature (id, payload, signature)
    VALUES (TRUE, p_payload, p_signature)
    ON CONFLICT (id) DO UPDATE
        SET payload = EXCLUDED.payload, signature = EXCLUDED.signature, signed_at = clock_timestamp();
END
$$;

-- A worker the manager has revoked. The signature is replaced separately.
CREATE OR REPLACE FUNCTION add_manager_revocation(
    p_serial      text,
    p_fingerprint text,
    p_device_name text,
    p_cert_pem    text
) RETURNS void
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
BEGIN
    INSERT INTO public.manager_revocation (serial, fingerprint_sha256, device_name, cert_pem)
    VALUES (p_serial, p_fingerprint, p_device_name, p_cert_pem)
    ON CONFLICT (serial) DO NOTHING;
END
$$;

-- Store a factory-signed worker certificate on an existing device.
-- The certificate already marked by device.cert_id stays the current one.
-- Returns false when no device has that name.
CREATE OR REPLACE FUNCTION register_factory_worker(
    p_subject_cn     text,
    p_ca_fingerprint text,
    p_serial         text,
    p_fingerprint    text,
    p_not_before     timestamptz,
    p_not_after      timestamptz,
    p_cert_pem       text
) RETURNS boolean
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
DECLARE
    v_dev  int;
    v_ca   int;
    v_cert int;
    v_owner int;
BEGIN
    SELECT d.device_id INTO v_dev FROM public.device d WHERE d.name = p_subject_cn;
    IF NOT FOUND THEN
        RETURN FALSE;
    END IF;
    SELECT r.ca_id INTO v_ca FROM public.root_ca r
    WHERE r.name = 'factory' AND r.fingerprint_sha256 = p_ca_fingerprint AND r.is_active;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'factory root % is not registered', p_ca_fingerprint
            USING ERRCODE = 'no_data_found';
    END IF;
    SELECT c.cert_id, c.device_id INTO v_cert, v_owner
    FROM public.device_certificate c WHERE c.fingerprint_sha256 = p_fingerprint;
    IF FOUND THEN
        IF v_owner <> v_dev THEN
            RAISE EXCEPTION 'certificate belongs to another device' USING ERRCODE = 'unique_violation';
        END IF;
    ELSE
        INSERT INTO public.device_certificate
            (device_id, ca_id, serial, fingerprint_sha256, subject_cn, not_before, not_after, cert_pem)
        VALUES (v_dev, v_ca, p_serial, p_fingerprint, p_subject_cn, p_not_before, p_not_after, p_cert_pem)
        RETURNING cert_id INTO v_cert;
    END IF;
    RETURN TRUE;
END
$$;

REVOKE ALL ON FUNCTION register_factory_root(text, text, text, timestamptz, timestamptz) FROM PUBLIC;
REVOKE ALL ON FUNCTION register_manager_certificate(text, text, text, timestamptz, timestamptz, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION replace_manager_crl(text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION add_manager_revocation(text, text, text, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION register_factory_worker(text, text, text, text, timestamptz, timestamptz, text) FROM PUBLIC;

-- The seeded devices enroll like any other: the controller starts their
-- processes, they generate keys and CSRs, and the server signs them.
INSERT INTO device_enrollment (kind, name, type_id, requested_by)
SELECT 'ENROLL', s.name, s.type_id, 'db-init'
FROM (VALUES ('dummy1', 'sine'), ('dummy2', 'sawtooth'), ('dummy3', 'gps_orbit')) AS s (name, type_id)
WHERE NOT EXISTS (SELECT 1 FROM device d WHERE d.name = s.name)
  AND NOT EXISTS (SELECT 1 FROM device_enrollment e
                  WHERE e.name = s.name AND e.status IN ('PENDING', 'CSR_SUBMITTED'));

RESET ROLE;
