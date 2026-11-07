/*
=============================================================================
00_roles.sql - cluster roles (run as a superuser, once per cluster)
=============================================================================

Role model

  demo_owner       NOLOGIN   Owns every object. SECURITY DEFINER functions run as it.
  demo_admin       NOLOGIN   Group behind the JWT role "admin".
  demo_normal      NOLOGIN   Group behind the JWT role "normal" (read only).
  demo_server      LOGIN     Zone A process. NOINHERIT: on its own it can only call
                             auth_fetch_user(). Every API request runs under
                             SET LOCAL ROLE demo_admin | demo_normal, chosen from
                             the verified JWT claim.
  demo_controller  LOGIN     Zone B process. Least privilege on the queue, the
                             applied configuration and the data tables. No access
                             to app_users.

Passwords for the LOGIN roles are set by scripts/db-init.sh, never here.
*/

DO $$
BEGIN
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'demo_owner') THEN
        CREATE ROLE demo_owner NOLOGIN;
    END IF;
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'demo_admin') THEN
        CREATE ROLE demo_admin NOLOGIN;
    END IF;
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'demo_normal') THEN
        CREATE ROLE demo_normal NOLOGIN;
    END IF;
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'demo_server') THEN
        CREATE ROLE demo_server LOGIN NOINHERIT;
    END IF;
    IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'demo_controller') THEN
        CREATE ROLE demo_controller LOGIN;
    END IF;
END
$$;

-- Admin includes everything a normal user can read.
GRANT demo_normal TO demo_admin;

-- The server may switch into either group but inherits neither.
GRANT demo_admin  TO demo_server WITH INHERIT FALSE, SET TRUE;
GRANT demo_normal TO demo_server WITH INHERIT FALSE, SET TRUE;
