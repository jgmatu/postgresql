/*
=============================================================================
05_views.sql - views and the privilege matrix
=============================================================================

  Object                          demo_normal  demo_admin        demo_controller   demo_server (base)
  ------------------------------  -----------  ----------------  ----------------  ------------------
  device_type                     SELECT       SELECT            SELECT            -
  device                          SELECT       SELECT/DELETE     SELECT            SELECT device_id, name
  device_state                    SELECT       SELECT            SELECT/INS/UPD    -
  telemetry_*                     SELECT       SELECT            INSERT            SELECT
  telemetry_read_cursor           -            -                 -                 SELECT/UPDATE
  device_config                   SELECT       SELECT            SELECT/INS/UPD    -
  device_config_pending           SELECT       SELECT/INSERT     SELECT/UPDATE     -
  device_config_errors            SELECT       SELECT            INSERT            -
  device_config_history           SELECT       SELECT            INSERT            -
  device_config_pending_history   -            INSERT            -                 -
  root_ca                         SELECT       SELECT            SELECT            SELECT
  device_certificate              SELECT       SELECT/DELETE     SELECT            -
  crl                             SELECT       SELECT/INS/UPD/DEL SELECT           -
  device_enrollment               SELECT       SELECT/INSERT     SELECT/UPD status SELECT
  device_session                  SELECT       SELECT            SELECT/INS/UPD    -
  request_id_seq                  -            USAGE             -                 -
  app_users                       -            non-secret cols   -                 -
  v_devices, v_device_identity    SELECT       SELECT            v_device_identity -
  v_device_config_sync_status     SELECT       SELECT            -                 -
  v_users_directory               -            SELECT            -                 -
  auth_fetch_user()               -            -                 -                 EXECUTE
  issue_device_certificate()      -            -                 -                 EXECUTE
  reject_enrollment()             -            -                 -                 EXECUTE
  ensure_monthly_partitions()     -            -                 EXECUTE           -

Nobody may INSERT into device: a device exists only through
issue_device_certificate(), together with its certificate. The crl triggers
keep every reason but certificateHold permanent.

demo_admin inherits the demo_normal column. The JWT role decides which group
the server switches into (SET LOCAL ROLE) for each request, so the database
enforces the same authorization as the HTTP layer.
*/

SET search_path = public, extensions;
SET ROLE demo_owner;

-- Per device: its current certificate and what that certificate allows.
--   ACTIVE       valid, not revoked, CA active
--   SUSPENDED    current certificate on hold (certificateHold), reversible
--   REVOKED      current certificate revoked for any other reason
--   CA_INACTIVE  issuing CA disabled or expired
--   EXPIRED      outside the certificate's validity period
-- Only ACTIVE devices get a running process from the controller.
CREATE OR REPLACE VIEW v_device_identity AS
SELECT
    d.device_id,
    d.name                AS device,
    c.cert_id,
    c.serial,
    c.fingerprint_sha256  AS fingerprint,
    r.name                AS ca,
    r.fingerprint_sha256  AS ca_fingerprint,
    c.not_before,
    c.not_after,
    c.issued_at,
    x.reason              AS revocation_reason,
    x.revoked_at,
    CASE
        WHEN x.reason = 'certificateHold' THEN 'SUSPENDED'
        WHEN x.serial IS NOT NULL THEN 'REVOKED'
        WHEN NOT r.is_active OR r.not_after <= now() THEN 'CA_INACTIVE'
        WHEN c.not_after <= now() OR c.not_before > now() THEN 'EXPIRED'
        ELSE 'ACTIVE'
    END                   AS identity,
    (SELECT count(*) FROM device_certificate h WHERE h.device_id = d.device_id) AS certificates,
    s.state               AS session,
    s.pid,
    s.connected_at,
    s.last_seen,
    s.last_error
FROM device d
JOIN device_certificate c ON c.cert_id = d.cert_id
JOIN root_ca r ON r.ca_id = c.ca_id
LEFT JOIN crl x ON x.ca_id = c.ca_id AND x.serial = c.serial
LEFT JOIN device_session s ON s.device_id = d.device_id;

-- Every device with its type, last sample and identity.
-- GPS devices expose lon and lat. Scatter devices expose x and y (point
-- subscripts). Cloud devices expose x, y and z (ST_X/ST_Y/ST_Z of PointZ).
CREATE OR REPLACE VIEW v_devices AS
SELECT
    d.device_id,
    d.name                         AS device,
    d.type_id,
    t.payload,
    s.value,
    ST_X(s.position::geometry)     AS lon,
    ST_Y(s.position::geometry)     AS lat,
    s.seq,
    s.updated_at,
    d.created_at,
    i.identity,
    i.session,
    COALESCE((s.xy)[0], ST_X(s.xyz)) AS x,
    COALESCE((s.xy)[1], ST_Y(s.xyz)) AS y,
    ST_Z(s.xyz)                    AS z
FROM device d
JOIN device_type t USING (type_id)
LEFT JOIN device_state s USING (device_id)
LEFT JOIN v_device_identity i USING (device_id);

-- Per device: applied configuration, latest desired request, sync status and
-- latest error. A device without applied state yet is UNINITIALISED.
CREATE OR REPLACE VIEW v_device_config_sync_status AS
SELECT
    d.device_id,
    d.name               AS device,
    d.type_id,
    c.rate_ms            AS applied_rate_ms,
    c.amplitude          AS applied_amplitude,
    c.is_active          AS applied_is_active,
    c.request_id         AS applied_request_id,
    c.applied_at,
    p.request_id         AS desired_request_id,
    p.requested_rate_ms  AS desired_rate_ms,
    p.requested_amplitude AS desired_amplitude,
    p.requested_is_active AS desired_is_active,
    p.status             AS desired_status,
    p.requested_by,
    p.requested_at,
    CASE
        WHEN p.status = 'PENDING' THEN 'PENDING'
        WHEN p.status IN ('ERROR', 'TIMEOUT') THEN 'ERROR'
        WHEN c.device_id IS NULL THEN 'UNINITIALISED'
        ELSE 'SYNC'
    END                  AS sync,
    e.error_code         AS last_error_code,
    e.error_detail       AS last_error_detail,
    e.request_id         AS last_error_request_id,
    e.occurred_at        AS last_error_at
FROM device d
LEFT JOIN device_config c USING (device_id)
LEFT JOIN LATERAL (
    SELECT * FROM device_config_pending q
    WHERE q.device_id = d.device_id
    ORDER BY q.requested_at DESC, q.request_id DESC
    LIMIT 1
) p ON TRUE
LEFT JOIN LATERAL (
    SELECT * FROM device_config_errors x
    WHERE x.device_id = d.device_id
    ORDER BY x.occurred_at DESC
    LIMIT 1
) e ON TRUE;

CREATE OR REPLACE VIEW v_users_directory AS
SELECT user_id, username, role::text AS role, created_at, updated_at
FROM app_users;

RESET ROLE;

-- ---------------------------------------------------------------------------
-- Privileges (run as superuser; objects are owned by demo_owner)
-- ---------------------------------------------------------------------------
REVOKE ALL ON ALL TABLES IN SCHEMA public FROM PUBLIC;
REVOKE ALL ON ALL SEQUENCES IN SCHEMA public FROM PUBLIC;
REVOKE EXECUTE ON ALL FUNCTIONS IN SCHEMA public FROM PUBLIC;
REVOKE CREATE ON SCHEMA public FROM PUBLIC;

GRANT USAGE ON SCHEMA public TO demo_normal, demo_admin, demo_controller, demo_server;

-- demo_normal: read-only monitoring
GRANT SELECT ON device_type, device, device_state, telemetry_scalar, telemetry_gps, telemetry_xy, telemetry_xyz TO demo_normal;
GRANT SELECT ON device_config, device_config_pending, device_config_errors, device_config_history TO demo_normal;
GRANT SELECT ON v_devices, v_device_config_sync_status, v_device_identity TO demo_normal;
GRANT SELECT ON root_ca, device_certificate, crl, device_enrollment, device_session TO demo_normal;
GRANT SELECT ON manager_certificate, manager_revocation, manager_crl_signature TO demo_normal;

-- demo_admin: inherits demo_normal (00_roles.sql), plus enrollment, revocation,
-- removal, enqueue and user management. Identity columns need no sequence privilege.
GRANT DELETE ON device, device_certificate TO demo_admin;
GRANT INSERT (request_id, kind, name, type_id, device_id, requested_by) ON device_enrollment TO demo_admin;
GRANT INSERT (ca_id, serial, fingerprint_sha256, device_name, cert_pem, reason, revoked_by) ON crl TO demo_admin;
GRANT UPDATE (reason, revoked_at, revoked_by) ON crl TO demo_admin;
GRANT DELETE ON crl TO demo_admin;
GRANT INSERT ON device_config_pending, device_config_pending_history TO demo_admin;
GRANT USAGE ON SEQUENCE request_id_seq TO demo_admin;
GRANT SELECT (user_id, username, role, created_at, updated_at) ON app_users TO demo_admin;
GRANT INSERT (username, password_hash, role) ON app_users TO demo_admin;
GRANT UPDATE (password_hash, role) ON app_users TO demo_admin;
GRANT DELETE ON app_users TO demo_admin;
GRANT SELECT ON v_users_directory TO demo_admin;

-- demo_server base role: login lookup, the certificate authority, and the
-- telemetry read cursor. The cursor is not granted to the JWT groups.
GRANT SELECT ON telemetry_scalar, telemetry_gps, telemetry_xy, telemetry_xyz TO demo_server;
GRANT SELECT (device_id, name) ON device TO demo_server;
GRANT SELECT, UPDATE ON telemetry_read_cursor TO demo_server;
GRANT EXECUTE ON FUNCTION auth_fetch_user(text) TO demo_server;
GRANT SELECT ON root_ca TO demo_server;
GRANT SELECT ON manager_certificate, manager_revocation, manager_crl_signature TO demo_server;
GRANT EXECUTE ON FUNCTION register_factory_root(text, text, text, timestamptz, timestamptz) TO demo_server;
GRANT SELECT (request_id, kind, name, type_id, status, csr_pem, requested_by) ON device_enrollment TO demo_server;
GRANT EXECUTE ON FUNCTION issue_device_certificate(bigint, text, text, text, text, timestamptz, timestamptz, text)
    TO demo_server;
GRANT EXECUTE ON FUNCTION reject_enrollment(bigint, text, text) TO demo_server;

-- demo_controller: zone B worker
GRANT SELECT ON device_type, device TO demo_controller;
GRANT SELECT, INSERT, UPDATE ON device_state, device_config TO demo_controller;
GRANT INSERT ON telemetry_scalar, telemetry_gps, telemetry_xy, telemetry_xyz TO demo_controller;
GRANT SELECT, UPDATE ON device_config_pending TO demo_controller;
GRANT INSERT ON device_config_history, device_config_errors TO demo_controller;
GRANT SELECT ON root_ca, device_certificate, crl, device_enrollment, v_device_identity TO demo_controller;
GRANT SELECT ON manager_certificate, manager_revocation, manager_crl_signature TO demo_controller;
GRANT EXECUTE ON FUNCTION register_factory_root(text, text, text, timestamptz, timestamptz) TO demo_controller;
GRANT EXECUTE ON FUNCTION register_manager_certificate(text, text, text, text, timestamptz, timestamptz, text)
    TO demo_controller;
GRANT EXECUTE ON FUNCTION replace_manager_crl(text, text) TO demo_controller;
GRANT EXECUTE ON FUNCTION add_manager_revocation(text, text, text, text) TO demo_controller;
GRANT EXECUTE ON FUNCTION register_factory_worker(text, text, text, text, timestamptz, timestamptz, text)
    TO demo_controller;
GRANT UPDATE (status, csr_pem, csr_at, error_code, error_detail, decided_at) ON device_enrollment TO demo_controller;
GRANT SELECT, INSERT, UPDATE ON device_session TO demo_controller;
GRANT EXECUTE ON FUNCTION ensure_monthly_partitions(regclass, date, int) TO demo_controller;
