/**
 * ingest.mjs — MQTT の位置メッセージを PostgreSQL に保存する軽量ブリッジ
 *
 * 設計: docs/backend-design.md §2
 *   - 購読: carpal/+/+/location (QoS 1, persistent session)
 *   - 検証: docs/contract.md §2 (id≡topic, 座標範囲, ts 範囲)
 *   - 冪等化: UNIQUE(device_id, recorded_at) で UPSERT
 *   - API: GET /api/history?hours=N で直近N時間の履歴を返す (CORS許可)
 *
 * 環境変数:
 *   MQTT_URL   (既定 mqtt://host.docker.internal:1883)
 *   MQTT_USER / MQTT_PASS (.env で必須)
 *   PGHOST / PGPORT / PGUSER / PGPASSWORD / PGDATABASE
 *   PORT (APIポート, 既定 3000)
 */

import mqtt from 'mqtt';
import pg from 'pg';
import http from 'node:http';

const MQTT_URL = process.env.MQTT_URL || 'mqtt://host.docker.internal:1883';
const MQTT_USER = process.env.MQTT_USER || 'webuser';
const MQTT_PASS = process.env.MQTT_PASS;   // .env 必須
const API_PORT = Number(process.env.PORT || 3000);
// 保持期間 (docs/operations-design.md §2: location 90日 / ログ730日)
const RETENTION_DAYS = Number(process.env.RETENTION_DAYS || 90);
const EVENT_RETENTION_DAYS = Number(process.env.EVENT_RETENTION_DAYS || 730);

const TENANT_RE = /^[^+#]+$/; // トピック階層にワイルドカード混入を拒否

// ---------- PostgreSQL ----------
const pool = new pg.Pool({
  host: process.env.PGHOST || 'postgres',
  port: Number(process.env.PGPORT || 5432),
  user: process.env.PGUSER || 'carpal',
  password: process.env.PGPASSWORD,
  database: process.env.PGDATABASE || 'carpal',
});

async function initDb() {
  // MVP: 単一テーブル + インデックス (月次パーティション化は Volume 増大時のフォローアップ)
  await pool.query(`
    CREATE TABLE IF NOT EXISTS locations (
      id          bigserial,
      device_id   text NOT NULL,
      tenant_id   text NOT NULL DEFAULT 'default',
      lat         double precision NOT NULL,
      lng         double precision NOT NULL,
      speed       real,
      bearing     real,
      recorded_at timestamptz NOT NULL,
      PRIMARY KEY (id, recorded_at)
    )`);
  await pool.query(`
    CREATE UNIQUE INDEX IF NOT EXISTS locations_dev_time_uidx
      ON locations (device_id, recorded_at)`);
  await pool.query(`
    CREATE INDEX IF NOT EXISTS locations_query_idx
      ON locations (tenant_id, device_id, recorded_at DESC)`);
  await pool.query(`
    CREATE TABLE IF NOT EXISTS device_events (
      id        bigserial PRIMARY KEY,
      device_id text NOT NULL,
      tenant_id text NOT NULL DEFAULT 'default',
      level     text NOT NULL,
      event_id  text NOT NULL,
      message   text,
      at        timestamptz NOT NULL DEFAULT now()
    )`);
  await pool.query(`
    CREATE INDEX IF NOT EXISTS device_events_query_idx
      ON device_events (device_id, at DESC)`);
  console.log('[ingest] db schema ready');
}

// ---------- 契約検証 (docs/contract.md §2) ----------
function validate(topic, payload) {
  const parts = topic.split('/');
  if (parts.length !== 4 || parts[0] !== 'carpal') return { error: 'bad_topic' };
  const [, tenantId, deviceId, kind] = parts;
  if (kind !== 'location') return { skip: true };
  if (!TENANT_RE.test(tenantId) || !TENANT_RE.test(deviceId)) return { error: 'bad_topic' };

  let data;
  try { data = JSON.parse(payload.toString()); } catch { return { error: 'MSG_INVALID', why: 'json' }; }
  if (data.id !== deviceId) return { error: 'MSG_INVALID', why: 'id_topic_mismatch' };

  const lat = Number(data.lat), lng = Number(data.lng);
  if (!Number.isFinite(lat) || !Number.isFinite(lng) ||
      (lat === 0 && lng === 0) || lat < -90 || lat > 90 || lng < -180 || lng > 180) {
    return { error: 'MSG_INVALID', why: 'coords' };
  }

  // ts: 妥当範囲 (2020年以降〜現在+10分) 外なら受信時刻へフォールバック
  let ts = Number(data.ts);
  const now = Date.now();
  if (!Number.isFinite(ts) || ts < 1577836800000 || ts > now + 600000) ts = now;

  return {
    ok: true,
    tenantId, deviceId,
    lat, lng,
    speed: Number.isFinite(Number(data.spd)) ? Number(data.spd) : null,
    bearing: Number.isFinite(Number(data.brg)) ? Number(data.brg) : null,
    recordedAt: new Date(ts),
  };
}

// ---------- MQTT ----------
const mqttClient = mqtt.connect(MQTT_URL, {
  username: MQTT_USER,
  password: MQTT_PASS,
  clientId: 'ingest-1',
  clean: false,             // persistent session (backend-design §2.1)
  protocolVersion: 4,
});

mqttClient.on('connect', async () => {
  console.log(`[ingest] connected to ${MQTT_URL}`);
  mqttClient.subscribe('carpal/+/+/location', { qos: 1 });
  mqttClient.subscribe('carpal/+/+/log', { qos: 1 });
});

mqttClient.on('message', async (topic, payload) => {
  const parts = topic.split('/');
  if (parts.length !== 4 || parts[0] !== 'carpal') return;
  const [, tenantId, deviceId, kind] = parts;

  if (kind === 'log') {
    // 診断ログ (contract.md §3.1) を device_events に保存
    try {
      const d = JSON.parse(payload.toString());
      if (!TENANT_RE.test(deviceId) || !d.level || !d.event) return;
      await pool.query(
        `INSERT INTO device_events (device_id, tenant_id, level, event_id, message)
         VALUES ($1,$2,$3,$4,$5)`,
        [deviceId, tenantId, String(d.level).slice(0, 10), String(d.event).slice(0, 40),
         d.msg ? String(d.msg).slice(0, 200) : null]
      );
    } catch (e) {
      console.error('[ingest] log insert failed:', e.message);
    }
    return;
  }

  const v = validate(topic, payload);
  if (v.skip) return;
  if (v.error) {
    console.warn(`[ingest] ${v.error} (${v.why || ''}): ${topic}`);
    return;
  }
  try {
    await pool.query(
      `INSERT INTO locations (device_id, tenant_id, lat, lng, speed, bearing, recorded_at)
       VALUES ($1,$2,$3,$4,$5,$6,$7)
       ON CONFLICT (device_id, recorded_at) DO UPDATE
         SET lat=EXCLUDED.lat, lng=EXCLUDED.lng,
             speed=EXCLUDED.speed, bearing=EXCLUDED.bearing`,
      [v.deviceId, v.tenantId, v.lat, v.lng, v.speed, v.bearing, v.recordedAt]
    );
  } catch (e) {
    console.error('[ingest] db write failed:', e.message);
  }
});

mqttClient.on('reconnect', () => console.log('[ingest] reconnecting...'));
// NOTE: mqtt.js は CONNACK で拒否された場合 (例: webuser 未プロビジョニングの起動直後)
// 自動再接続しないケースがあるため、未接続なら5秒間隔で明示的に再接続する。
let mqttRetryTimer = null;
mqttClient.on('error', (e) => {
  console.error('[ingest] mqtt error:', e.message);
  if (!mqttClient.connected && !mqttRetryTimer) {
    mqttRetryTimer = setTimeout(() => {
      mqttRetryTimer = null;
      try { mqttClient.reconnect(); } catch { /* already connecting */ }
    }, 5000);
  }
});

// ---------- 履歴 API ----------
const server = http.createServer(async (req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*');   // LANベンチ用 (本番は制限)
  const url = new URL(req.url, 'http://localhost');

  // ログ照会: /api/logs?hours=24&device=car01&level=WARN
  if (url.pathname === '/api/logs') {
    const hours = Math.min(Math.max(Number(url.searchParams.get('hours')) || 24, 1), 24 * 30);
    const device = url.searchParams.get('device');
    const level = url.searchParams.get('level');
    const conds = ['at > now() - ($1 || \' hours\')::interval'];
    const params = [String(hours)];
    if (device) { params.push(device); conds.push(`device_id = $${params.length}`); }
    if (level) { params.push(level.toUpperCase()); conds.push(`level = $${params.length}`); }
    try {
      const { rows } = await pool.query(
        `SELECT device_id, level, event_id, message,
                EXTRACT(EPOCH FROM at) * 1000 AS ts
         FROM device_events
         WHERE ${conds.join(' AND ')}
         ORDER BY at DESC LIMIT 200`,
        params
      );
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ logs: rows, count: rows.length }));
    } catch (e) {
      res.writeHead(500, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: e.message }));
    }
    return;
  }

  if (url.pathname !== '/api/history') {
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'not_found' }));
    return;
  }
  const hours = Math.min(Math.max(Number(url.searchParams.get('hours')) || 24, 1), 24 * 30);
  const limit = Math.min(Number(url.searchParams.get('limit')) || 5000, 20000);
  try {
    const { rows } = await pool.query(
      `SELECT device_id, lat, lng, speed, bearing,
              EXTRACT(EPOCH FROM recorded_at) * 1000 AS ts
       FROM locations
       WHERE recorded_at > now() - ($1 || ' hours')::interval
       ORDER BY recorded_at ASC
       LIMIT $2`,
      [String(hours), limit]
    );
    const devices = {};
    for (const r of rows) {
      (devices[r.device_id] ||= []).push({
        lat: r.lat, lng: r.lng,
        spd: r.speed, brg: r.bearing, ts: Number(r.ts),
      });
    }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ devices, count: rows.length }));
  } catch (e) {
    res.writeHead(500, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: e.message }));
  }
});

// ---------- 起動 ----------
const start = async () => {
  while (true) {
    try { await initDb(); break; }
    catch (e) { console.error('[ingest] db not ready, retry in 3s:', e.message); await new Promise(r => setTimeout(r, 3000)); }
  }
  startCleanup();
  server.listen(API_PORT, () => console.log(`[ingest] history API on :${API_PORT}`));
};
start();

process.on('SIGINT', async () => { await pool.end(); process.exit(0); });

// ---------- 保持期間クリーンアップ (毎日 1回) ----------
async function cleanup() {
  try {
    const r1 = await pool.query(
      `DELETE FROM locations WHERE recorded_at < now() - ($1 || ' days')::interval`, [String(RETENTION_DAYS)]);
    const r2 = await pool.query(
      `DELETE FROM device_events WHERE at < now() - ($1 || ' days')::interval`, [String(EVENT_RETENTION_DAYS)]);
    console.log(`[ingest] retention cleanup: locations=${r1.rowCount} events=${r2.rowCount}`);
  } catch (e) {
    console.error('[ingest] retention cleanup failed:', e.message);
  }
}
let cleanupTimer = null;
const startCleanup = () => {
  cleanup();
  cleanupTimer = setInterval(cleanup, 24 * 3600 * 1000);
};
