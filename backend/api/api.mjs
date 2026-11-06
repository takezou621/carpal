/**
 * api.mjs — carpal バックエンド API (Fastify)
 *
 * 実装範囲 (Phase 2-c ステップ1):
 *   - JWT認証 (HS256/15分) + リフレッシュトークン回転 (7日, DB保存)
 *   - テナント管理 (作成/一覧/suspend) [システム管理者]
 *   - デバイス管理 (登録時 secret 1回発行, ローテーション2本持ち) [owner]
 *   - 監査ログ (非read操作を記録, admin-design §5)
 *   - レート制限 (メモリ簡易版), /api/health
 *
 * 意図的なMVP遂脱 (docs/backend-design.md 冒頭に記載):
 *   - HS256 (設計はRS256 — go-auth 導入時に切替)
 *   - リフレッシュトークンはJSON応答 (本番はhttpOnly cookie + HTTPS)
 *   - メンバー招待 (メール送信) は未実装
 *   - ログイン試行の監査記録は未実装
 *
 * 環境変数: PGHOST/PGUSER/PGPASSWORD/PGDATABASE, PORT(3001),
 *   JWT_SECRET, ADMIN_EMAIL, ADMIN_PASSWORD (users空の時のみシード)
 */

import Fastify from 'fastify';
import pg from 'pg';
import crypto from 'node:crypto';
import jwt from 'jsonwebtoken';
import { startProvisioning, provisionDeviceClient, setDeviceEnabled } from './mqttProvision.mjs';

const PORT = Number(process.env.PORT || 3001);
const JWT_SECRET = process.env.JWT_SECRET;   // .env で必須 (GitGuardian対策: ハードコード禁止)
if (!JWT_SECRET) { console.error('[api] JWT_SECRET is required (.env)'); process.exit(1); }
const ACCESS_TTL_SEC = 15 * 60;
const REFRESH_TTL_SEC = 7 * 24 * 3600;
const ROTATION_OVERLAP_SEC = 24 * 3600;   // シークレット2本持ち期間 (multi-tenant §3.4)

const app = Fastify({ logger: false });
const pool = new pg.Pool({
  host: process.env.PGHOST || 'postgres',
  port: Number(process.env.PGPORT || 5432),
  user: process.env.PGUSER || 'carpal',
  password: process.env.PGPASSWORD || 'carpal-dev-pass',
  database: process.env.PGDATABASE || 'carpal',
});

// ---------- helpers ----------
const scryptHash = (secret) => {
  const salt = crypto.randomBytes(16).toString('hex');
  const hash = crypto.scryptSync(secret, salt, 32).toString('hex');
  return `${salt}:${hash}`;
};
const scryptVerify = (secret, stored) => {
  const [salt, hash] = String(stored).split(':');
  if (!salt || !hash) return false;
  const calc = crypto.scryptSync(secret, salt, 32).toString('hex');
  return crypto.timingSafeEqual(Buffer.from(hash, 'hex'), Buffer.from(calc, 'hex'));
};
const sha256 = (s) => crypto.createHash('sha256').update(s).digest('hex');
const uuid = () => crypto.randomUUID();

function signToken(user, tid) {
  return jwt.sign(
    { sub: user.id, tid: tid || null, role: user.role, sys: user.sys, ver: user.token_ver },
    JWT_SECRET,
    { expiresIn: ACCESS_TTL_SEC, algorithm: 'HS256' }
  );
}

// レート制限 (メモリ簡易: 1分あたり)
const rateMap = new Map(); // key -> {count, resetAt}
function rateLimit(key, max) {
  const now = Date.now();
  const e = rateMap.get(key);
  if (!e || now > e.resetAt) { rateMap.set(key, { count: 1, resetAt: now + 60000 }); return true; }
  e.count++;
  if (rateMap.size > 10000) rateMap.clear();
  return e.count <= max;
}

// 監査ログ (非read操作は必ず呼ぶ)
async function audit(req, { actorId, actorRole, tenantId, action, targetType, targetId, detail }) {
  await pool.query(
    `INSERT INTO audit_logs (actor_id, actor_role, tenant_id, action, target_type, target_id, detail, ip)
     VALUES ($1,$2,$3,$4,$5,$6,$7,$8)`,
    [actorId, actorRole, tenantId, action, targetType, targetId,
     detail ? JSON.stringify(detail) : null, req.ip]
  );
}

// 認証ミドルウェア
app.decorate('authenticate', async (req, reply) => {
  const auth = req.headers.authorization || '';
  const token = auth.startsWith('Bearer ') ? auth.slice(7) : null;
  if (!token) return reply.code(401).send({ error: { code: 'NO_TOKEN', message: 'Bearer token required' } });
  let payload;
  try { payload = jwt.verify(token, JWT_SECRET, { algorithms: ['HS256'] }); }
  catch { return reply.code(401).send({ error: { code: 'TOKEN_EXPIRED_OR_INVALID', message: 'invalid token' } }); }

  // 即時失効チェック: users.disabled / token_ver (multi-tenant §3.4)
  const { rows } = await pool.query('SELECT id, disabled, token_ver, sys, role FROM users WHERE id=$1', [payload.sub]);
  const u = rows[0];
  if (!u || u.disabled || u.token_ver !== payload.ver) {
    return reply.code(401).send({ error: { code: 'TOKEN_REVOKED', message: 'token revoked' } });
  }
  req.user = { id: payload.sub, tid: payload.tid, role: payload.role, sys: !!payload.sys };
});
app.decorateRequest('user', null);

const requireSys = async (req, reply) => {
  if (!req.user?.sys) return reply.code(403).send({ error: { code: 'FORBIDDEN', message: 'system admin only' } });
};
const requireOwner = async (req, reply) => {
  if (req.user?.sys) return; // システム管理者は全テナント操作可
  if (req.user?.role !== 'owner') return reply.code(403).send({ error: { code: 'FORBIDDEN', message: 'owner role required' } });
  // tid 一致確認
  const tid = req.params.tid || req.user.tid;
  if (tid !== req.user.tid) return reply.code(403).send({ error: { code: 'TENANT_MISMATCH', message: 'token tenant mismatch' } });
};

// ---------- 起動時マイグレーション & 管理者シード ----------
import { readFileSync } from 'node:fs';
async function migrate() {
  const sql = readFileSync(new URL('./migrations.sql', import.meta.url), 'utf8');
  await pool.query(sql);
  console.log('[api] migrations applied');

  const { rows } = await pool.query('SELECT count(*)::int AS n FROM users');
  if (rows[0].n === 0) {
    const email = process.env.ADMIN_EMAIL || 'admin@carpal.local';
    const pass = process.env.ADMIN_PASSWORD;
    if (!pass) { console.log('[api] ADMIN_PASSWORD 未設定のため管理者シードをスキップ'); return; }
    const sys = true;
    const { rows: [u] } = await pool.query(
      `INSERT INTO users (email, password_hash, role, sys) VALUES ($1,$2,'admin',true) RETURNING id`,
      [email, scryptHash(pass)]
    );
    // default テナント + owner membership
    const { rows: [t] } = await pool.query(`INSERT INTO tenants (name) VALUES ('default') RETURNING id`);
    await pool.query(`INSERT INTO memberships (user_id, tenant_id, role) VALUES ($1,$2,'owner')`, [u.id, t.id]);
    console.log(`[api] seeded admin user ${email} (password from env/default) + tenant 'default'`);
  }
}

// ---------- CORS (LANベンチ用 wildcard — 本番はCaddy配下でオリジン制限) ----------
app.addHook('onSend', async (req, reply) => {
  reply.header('Access-Control-Allow-Origin', req.headers.origin || '*');
  reply.header('Access-Control-Allow-Headers', 'Content-Type, Authorization');
  reply.header('Access-Control-Allow-Methods', 'GET, POST, PATCH, DELETE, OPTIONS');
  if (req.method === 'OPTIONS') reply.code(204).send();
});

// ---------- routes ----------
app.get('/api/health', async () => {
  let db = false;
  try { await pool.query('SELECT 1'); db = true; } catch {}
  return { ok: db, db, time: new Date().toISOString() };
});

// --- auth ---
app.post('/api/auth/login', async (req, reply) => {
  // レート制限: ベンチは30/min (本番では5/minへ締める)
  if (!rateLimit(`login:${req.ip}`, 30)) return reply.code(429).send({ error: { code: 'RATE_LIMITED', message: 'too many attempts' } });
  const { email, password } = req.body || {};
  if (!email || !password) return reply.code(400).send({ error: { code: 'BAD_REQUEST', message: 'email and password required' } });
  const { rows } = await pool.query('SELECT * FROM users WHERE email=$1', [email]);
  const u = rows[0];
  if (!u || u.disabled || !scryptVerify(password, u.password_hash)) {
    return reply.code(401).send({ error: { code: 'BAD_CREDENTIALS', message: 'invalid email or password' } });
  }
  // tid: 所属テナントの最初の1つ (MVP; テナントスイッチはUI側でtoken再発行)
  const { rows: ms } = await pool.query('SELECT tenant_id, role FROM memberships WHERE user_id=$1 ORDER BY tenant_id LIMIT 1', [u.id]);
  const tid = u.sys ? null : (ms[0]?.tenant_id || null);
  const token = signToken(u, tid);

  const refresh = crypto.randomBytes(32).toString('hex');
  await pool.query(
    `INSERT INTO refresh_tokens (user_id, token_hash, expires_at) VALUES ($1,$2, now() + interval '7 days')`,
    [u.id, sha256(refresh)]
  );
  return { access_token: token, token_type: 'Bearer', expires_in: ACCESS_TTL_SEC, refresh_token: refresh,
           tenant_id: tid, role: u.role, sys: u.sys };
});

app.post('/api/auth/refresh', async (req, reply) => {
  if (!rateLimit(`refresh:${req.ip}`, 30)) return reply.code(429).send({ error: { code: 'RATE_LIMITED', message: 'too many requests' } });
  const { refresh_token } = req.body || {};
  if (!refresh_token) return reply.code(400).send({ error: { code: 'BAD_REQUEST', message: 'refresh_token required' } });
  const { rows } = await pool.query(
    `SELECT rt.id, rt.user_id, rt.token_hash, rt.expires_at, rt.revoked, u.disabled, u.token_ver, u.role, u.sys
     FROM refresh_tokens rt JOIN users u ON u.id = rt.user_id
     WHERE rt.token_hash = $1`, [sha256(refresh_token)]
  );
  const rt = rows[0];
  if (!rt || rt.revoked || new Date(rt.expires_at) < new Date() || rt.disabled) {
    return reply.code(401).send({ error: { code: 'INVALID_REFRESH', message: 'invalid or expired refresh token' } });
  }
  // 回転: 使用済みを失効化し新しいもの発行
  await pool.query('UPDATE refresh_tokens SET revoked=true WHERE id=$1', [rt.id]);
  const newRefresh = crypto.randomBytes(32).toString('hex');
  await pool.query(
    `INSERT INTO refresh_tokens (user_id, token_hash, expires_at) VALUES ($1,$2, now() + interval '7 days')`,
    [rt.user_id, sha256(newRefresh)]
  );
  const { rows: [u] } = await pool.query('SELECT * FROM users WHERE id=$1', [rt.user_id]);
  const { rows: ms } = await pool.query('SELECT tenant_id FROM memberships WHERE user_id=$1 ORDER BY tenant_id LIMIT 1', [u.id]);
  const token = signToken(u, u.sys ? null : ms[0]?.tenant_id);
  return { access_token: token, token_type: 'Bearer', expires_in: ACCESS_TTL_SEC, refresh_token: newRefresh };
});

// --- テナント (システム管理者) ---
app.get('/api/admin/tenants', { preHandler: [app.authenticate, requireSys] }, async () => {
  const { rows } = await pool.query(
    `SELECT t.id, t.name, t.status, t.plan, t.created_at,
            (SELECT count(*)::int FROM devices d WHERE d.tenant_id=t.id) AS devices
     FROM tenants t ORDER BY t.created_at DESC`);
  return { tenants: rows };
});

app.post('/api/admin/tenants', { preHandler: [app.authenticate, requireSys] }, async (req, reply) => {
  const { name } = req.body || {};
  if (!name) return reply.code(400).send({ error: { code: 'BAD_REQUEST', message: 'name required' } });
  const { rows: [t] } = await pool.query(`INSERT INTO tenants (name) VALUES ($1) RETURNING *`, [name]);
  await audit(req, { actorId: req.user.id, actorRole: 'admin', tenantId: t.id, action: 'tenant.create', targetType: 'tenant', targetId: t.id });
  return reply.code(201).send({ tenant: t });
});

app.post('/api/admin/tenants/:tid/suspend', { preHandler: [app.authenticate, requireSys] }, async (req, reply) => {
  const status = req.body?.resume ? 'active' : 'suspended';
  const { rows } = await pool.query(`UPDATE tenants SET status=$1 WHERE id=$2 RETURNING id, name, status`, [status, req.params.tid]);
  if (!rows[0]) return reply.code(404).send({ error: { code: 'TENANT_NOT_FOUND', message: 'not found' } });
  // テナント全デバイスのMQTTクライアントを有効/無効化 (suspend強制)
  const { rows: devs } = await pool.query(`SELECT id FROM devices WHERE tenant_id=$1`, [req.params.tid]);
  for (const d of devs) {
    try { await setDeviceEnabled(d.id, status === 'active'); }
    catch (e) { console.error(`[api] mqtt ${d.id} enable/disable failed:`, e.message); }
  }
  await audit(req, { actorId: req.user.id, actorRole: 'admin', tenantId: req.params.tid,
    action: status === 'active' ? 'tenant.resume' : 'tenant.suspend', targetType: 'tenant', targetId: req.params.tid });
  return { tenant: rows[0] };
});

// --- デバイス (テナント管理者 owner) ---
app.get('/api/tenants/:tid/devices', { preHandler: [app.authenticate, requireOwner] }, async (req) => {
  const { rows } = await pool.query(
    `SELECT id, display_name, disabled, consent_confirmed, secret_rotated_at, created_at
     FROM devices WHERE tenant_id=$1 ORDER BY created_at`, [req.params.tid]);
  return { devices: rows };
});

app.post('/api/tenants/:tid/devices', { preHandler: [app.authenticate, requireOwner] }, async (req, reply) => {
  const { display_name, consent_confirmed } = req.body || {};
  if (!display_name) return reply.code(400).send({ error: { code: 'BAD_REQUEST', message: 'display_name required' } });
  if (consent_confirmed !== true) {
    // legal-compliance.md §2.2: 運転者同意の確認必須
    return reply.code(400).send({ error: { code: 'CONSENT_REQUIRED', message: 'consent_confirmed=true required (運転者の同意確認)' } });
  }
  const { rows: [t] } = await pool.query(`SELECT id, status FROM tenants WHERE id=$1`, [req.params.tid]);
  if (!t) return reply.code(404).send({ error: { code: 'TENANT_NOT_FOUND', message: 'not found' } });
  if (t.status !== 'active') return reply.code(409).send({ error: { code: 'TENANT_SUSPENDED', message: 'tenant suspended' } });

  const secret = crypto.randomBytes(32).toString('hex');
  const { rows: [d] } = await pool.query(
    `INSERT INTO devices (tenant_id, display_name, secret_hash, consent_confirmed)
     VALUES ($1,$2,$3,true) RETURNING id, display_name, created_at`,
    [req.params.tid, display_name, scryptHash(secret)]
  );
  await audit(req, { actorId: req.user.id, actorRole: req.user.sys ? 'admin' : 'owner', tenantId: req.params.tid,
    action: 'device.create', targetType: 'device', targetId: d.id, detail: { display_name } });
  // MQTT dynsec プロビジョニング (失敗時は500 — DB行は残るためローテーションで再プロビジョニング可)
  try {
    await provisionDeviceClient(req.params.tid, d.id, secret);
  } catch (e) {
    console.error('[api] mqtt provisioning failed:', e.message);
    return reply.code(500).send({ error: { code: 'MQTT_PROVISION_FAILED', message: e.message } });
  }
  // secret はこの応答1回のみ (admin-design T-1)
  return reply.code(201).send({ device: d, secret, mqtt: { username: d.id, tenant: req.params.tid } });
});

app.post('/api/tenants/:tid/devices/:did/rotate', { preHandler: [app.authenticate, requireOwner] }, async (req, reply) => {
  const secret = crypto.randomBytes(32).toString('hex');
  const { rows } = await pool.query(
    `UPDATE devices SET secret_prev_hash = secret_hash, secret_rotated_at = now(), secret_hash=$2
     WHERE id=$1 AND tenant_id=$3 RETURNING id, display_name`,
    [req.params.did, scryptHash(secret), req.params.tid]
  );
  if (!rows[0]) return reply.code(404).send({ error: { code: 'DEVICE_NOT_FOUND', message: 'not found' } });
  await audit(req, { actorId: req.user.id, actorRole: req.user.sys ? 'admin' : 'owner', tenantId: req.params.tid,
    action: 'device.rotate_secret', targetType: 'device', targetId: req.params.did });
  try {
    await provisionDeviceClient(req.params.tid, req.params.did, secret);   // setClientPassword 相当
  } catch (e) {
    console.error('[api] mqtt password update failed:', e.message);
  }
  // 旧シークレットは ROTATION_OVERLAP_SEC の重複期間後廃棄 (運用で24h後に disabled 化)
  return { device: rows[0], secret, previous_grace_hours: 24 };
});

app.patch('/api/tenants/:tid/devices/:did', { preHandler: [app.authenticate, requireOwner] }, async (req, reply) => {
  const { display_name, disabled } = req.body || {};
  const { rows } = await pool.query(
    `UPDATE devices SET
       display_name = COALESCE($2, display_name),
       disabled = COALESCE($3, disabled)
     WHERE id=$1 AND tenant_id=$4 RETURNING id, display_name, disabled`,
    [req.params.did, display_name ?? null, disabled ?? null, req.params.tid]
  );
  if (!rows[0]) return reply.code(404).send({ error: { code: 'DEVICE_NOT_FOUND', message: 'not found' } });
  if (disabled !== undefined) {
    try { await setDeviceEnabled(req.params.did, !disabled); }
    catch (e) { console.error('[api] mqtt enable/disable failed:', e.message); }
  }
  await audit(req, { actorId: req.user.id, actorRole: req.user.sys ? 'admin' : 'owner', tenantId: req.params.tid,
    action: 'device.update', targetType: 'device', targetId: req.params.did, detail: { display_name, disabled } });
  return { device: rows[0] };
});

app.get('/api/tenants/:tid/audit_logs', { preHandler: [app.authenticate, requireOwner] }, async (req) => {
  const { rows } = await pool.query(
    `SELECT action, target_type, target_id, detail, at FROM audit_logs
     WHERE tenant_id=$1 ORDER BY at DESC LIMIT 100`, [req.params.tid]);
  return { logs: rows };
});

// ---------- 起動 ----------
const start = async () => {
  while (true) {
    try { await pool.query('SELECT 1'); break; }
    catch (e) { console.error('[api] db not ready, retry in 3s:', e.message); await new Promise(r => setTimeout(r, 3000)); }
  }
  await migrate();
  startProvisioning(pool);
  await app.listen({ port: PORT, host: '0.0.0.0' });
  console.log(`[api] listening on :${PORT}`);
};
start();

process.on('SIGINT', async () => { await pool.end(); process.exit(0); });
