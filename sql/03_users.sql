/*
=============================================================================
03_users.sql - users, Argon2id password hashes and authorization roles
=============================================================================

app_users stores the PHC string produced by Argon2id
($argon2id$v=19$m=65536,t=3,p=1$<salt>$<hash>). Plain passwords never reach
the database.

The server's own login role cannot read this table. Login goes through
auth_fetch_user(), a SECURITY DEFINER function that returns one user's hash
and role. Everything else (list, create, delete) runs under
SET LOCAL ROLE demo_admin, which only sees the non-secret columns.
*/

SET ROLE demo_owner;

DO $$
BEGIN
    IF NOT EXISTS (SELECT FROM pg_type WHERE typname = 'app_role') THEN
        CREATE TYPE app_role AS ENUM ('admin', 'normal');
    END IF;
END
$$;

CREATE TABLE IF NOT EXISTS app_users (
    user_id        INT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username       TEXT NOT NULL UNIQUE CHECK (username ~ '^[a-z][a-z0-9_.-]{2,63}$'),
    password_hash  TEXT NOT NULL CHECK (password_hash LIKE '$argon2id$%'),
    role           app_role NOT NULL DEFAULT 'normal',
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON COLUMN app_users.password_hash IS 'Argon2id PHC string. Never returned by any API.';

-- Login lookup. The only object the server's base role may use on its own.
CREATE OR REPLACE FUNCTION auth_fetch_user(p_username text)
RETURNS TABLE (password_hash text, role text)
LANGUAGE sql
STABLE
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
    SELECT u.password_hash, u.role::text
    FROM public.app_users u
    WHERE u.username = p_username
$$;

REVOKE ALL ON FUNCTION auth_fetch_user(text) FROM PUBLIC;

-- The system must always keep at least one admin.
CREATE OR REPLACE FUNCTION protect_last_admin()
RETURNS trigger
LANGUAGE plpgsql
SET search_path = pg_catalog, public
AS $$
BEGIN
    IF OLD.role = 'admin'
       AND (TG_OP = 'DELETE' OR NEW.role <> 'admin')
       AND NOT EXISTS (SELECT 1 FROM public.app_users
                       WHERE role = 'admin' AND user_id <> OLD.user_id) THEN
        RAISE EXCEPTION 'cannot remove the last admin user' USING ERRCODE = 'check_violation';
    END IF;
    RETURN COALESCE(NEW, OLD);
END
$$;

CREATE OR REPLACE FUNCTION touch_updated_at()
RETURNS trigger
LANGUAGE plpgsql
AS $$
BEGIN
    NEW.updated_at := now();
    RETURN NEW;
END
$$;

CREATE OR REPLACE TRIGGER trg_app_users_last_admin
    BEFORE DELETE OR UPDATE OF role ON app_users
    FOR EACH ROW EXECUTE FUNCTION protect_last_admin();

CREATE OR REPLACE TRIGGER trg_app_users_touch
    BEFORE UPDATE ON app_users
    FOR EACH ROW EXECUTE FUNCTION touch_updated_at();

RESET ROLE;
