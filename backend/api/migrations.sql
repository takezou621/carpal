-- carpal API 初期スキーマ (起動時に冪等適用)
-- 設計: docs/multi-tenant-design.md §4.2 / docs/admin-design.md §5

CREATE TABLE IF NOT EXISTS tenants (
  id         uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  name       text NOT NULL,
  status     text NOT NULL DEFAULT 'active',   -- active | suspended
  plan       text NOT NULL DEFAULT 'free',
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS users (
  id            uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  email         text UNIQUE NOT NULL,
  password_hash text NOT NULL,                 -- scrypt (salt:hash)
  role          text NOT NULL DEFAULT 'viewer',-- admin | owner | viewer (admin=システム管理者)
  sys           boolean NOT NULL DEFAULT false,
  disabled      boolean NOT NULL DEFAULT false,
  token_ver     integer NOT NULL DEFAULT 1,
  created_at    timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS memberships (
  user_id   uuid REFERENCES users(id) ON DELETE CASCADE,
  tenant_id uuid REFERENCES tenants(id) ON DELETE CASCADE,
  role      text NOT NULL DEFAULT 'viewer',    -- owner | viewer
  PRIMARY KEY (user_id, tenant_id)
);

CREATE TABLE IF NOT EXISTS devices (
  id                uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  tenant_id         uuid REFERENCES tenants(id) ON DELETE CASCADE,
  display_name      text NOT NULL,
  secret_hash       text NOT NULL,             -- scrypt
  secret_prev_hash  text,                      -- ローテーション用 (active/previous 2本持ち)
  secret_rotated_at timestamptz,
  disabled          boolean NOT NULL DEFAULT false,
  consent_confirmed boolean NOT NULL DEFAULT false,  -- legal-compliance §2.2
  created_at        timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS refresh_tokens (
  id         bigserial PRIMARY KEY,
  user_id    uuid REFERENCES users(id) ON DELETE CASCADE,
  token_hash text NOT NULL,
  expires_at timestamptz NOT NULL,
  revoked    boolean NOT NULL DEFAULT false,
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS audit_logs (
  id          bigserial PRIMARY KEY,
  actor_id    text,
  actor_role  text,
  tenant_id   uuid,
  action      text NOT NULL,
  target_type text,
  target_id   text,
  detail      jsonb,
  ip          inet,
  at          timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS audit_logs_tenant_at_idx ON audit_logs (tenant_id, at DESC);
CREATE INDEX IF NOT EXISTS audit_logs_action_idx ON audit_logs (action);

CREATE TABLE IF NOT EXISTS app_keys (
  name  text PRIMARY KEY,
  value text NOT NULL
);
