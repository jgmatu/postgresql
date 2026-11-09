/*
=============================================================================
05_views.sql - views and the privilege matrix
=============================================================================

  Object                          demo_normal  demo_admin        demo_controller   demo_server (base)
  ------------------------------  -----------  ----------------  ----------------  ------------------
  device_type                     SELECT       SELECT            SELECT            -
  device                          SELECT       SELECT/INS/DEL    SELECT            -
  device_state                    SELECT       SELECT            SELECT/INS/UPD    -
  telemetry_scalar, telemetry_gps SELECT       SELECT            INSERT            -
  device_config                   SELECT       SELECT            SELECT/INS/UPD    -
  device_config_pending           SELECT       SELECT/INSERT     SELECT/UPDATE     -
  device_config_errors            SELECT       SELECT            INSERT            -
  device_config_history           SELECT       SELECT            INSERT            -
  device_config_pending_history   -            INSERT            -                 -
  request_id_seq                  -            USAGE             -                 -
  app_users                       -            non-secret cols   -                 -
  v_devices                       SELECT       SELECT            -                 -
  v_device_config_sync_status     SELECT       SELECT            -                 -
  v_users_directory               -            SELECT            -                 -
  auth_fetch_user()               -            -                 -                 EXECUTE
  ensure_monthly_partitions()     -            -                 EXECUTE           -

demo_admin inherits the demo_normal column. The JWT role decides which group
the server switches into (SET LOCAL ROLE) for each request, so the database
enforces the same authorization as the HTTP layer.
*/

SET search_path = public, extensions;
SET ROLE demo_owner;

-- Every device with its type and last sample. GPS devices expose lon and lat.
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
    d.created_at
FROM device d
JOIN device_type t USING (type_id)
LEFT JOIN device_state s USING (device_id);

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
GRANT SELECT ON device_type, device, device_state, telemetry_scalar, telemetry_gps TO demo_normal;
GRANT SELECT ON device_config, device_config_pending, device_config_errors, device_config_history TO demo_normal;
GRANT SELECT ON v_devices, v_device_config_sync_status TO demo_normal;

-- demo_admin: inherits demo_normal (00_roles.sql), plus device registry, enqueue
-- and user management. Identity columns need no sequence privilege.
GRANT INSERT (name, type_id) ON device TO demo_admin;
GRANT DELETE ON device TO demo_admin;
GRANT INSERT ON device_config_pending, device_config_pending_history TO demo_admin;
GRANT USAGE ON SEQUENCE request_id_seq TO demo_admin;
GRANT SELECT (user_id, username, role, created_at, updated_at) ON app_users TO demo_admin;
GRANT INSERT (username, password_hash, role) ON app_users TO demo_admin;
GRANT UPDATE (password_hash, role) ON app_users TO demo_admin;
GRANT DELETE ON app_users TO demo_admin;
GRANT SELECT ON v_users_directory TO demo_admin;

-- demo_server base role: login lookup only
GRANT EXECUTE ON FUNCTION auth_fetch_user(text) TO demo_server;

-- demo_controller: zone B worker
GRANT SELECT ON device_type, device TO demo_controller;
GRANT SELECT, INSERT, UPDATE ON device_state, device_config TO demo_controller;
GRANT INSERT ON telemetry_scalar, telemetry_gps TO demo_controller;
GRANT SELECT, UPDATE ON device_config_pending TO demo_controller;
GRANT INSERT ON device_config_history, device_config_errors TO demo_controller;
GRANT EXECUTE ON FUNCTION ensure_monthly_partitions(regclass, date, int) TO demo_controller;
