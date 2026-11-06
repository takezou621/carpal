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
 *   MQTT_USER / MQTT_PASS (既定 webuser / webuserpass)
 *   PGHOST / PGPORT / PGUSER / PGPASSWORD / PGDATABASE
 *   PORT (APIポート, 既定 3000)
 */

import mqtt from 'mqtt';
import pg from 'pg';
import http from 'node:http';

const MQTT_URL = process.env.MQTT_URL || 'mqtt://host.docker.internal:1883';
const MQTT_USER = process.env.MQTT_USER || 'webuser';
const MQTT_PASS = process.env.MQTT_PASS || 'webuserpass';
const API_PORT = Number(process.env.PORT || 3000);

const TENANT_RE = /^[^+#]+$/; // トピック階層にワイルドカード混入を拒否

// ---------- PostgreSQL ----------
const pool = new pg.Pool({
  host: process.env.PGHOST || 'postgres',
  port: Number(process.env.PGPORT || 5432),
  user: process.env.PGUSER || 'carpal',
  password: process.env.PGPASSWORD || 'carpal-dev-pass',
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
});

mqttClient.on('message', async (topic, payload) => {
  const v = validate(topic, payload);
  if (v.skip) return;
  if (v.error) {
    // MVP: device_events テーブル整備まではログのみ (backend-design §2.2 フォローアップ)
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
mqttClient.on('error', (e) => console.error('[ingest] mqtt error:', e.message));

// ---------- 履歴 API ----------
const server = http.createServer(async (req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*');   // LANベンチ用 (本番は制限)
  const url = new URL(req.url, 'http://localhost');
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
  server.listen(API_PORT, () => console.log(`[ingest] history API on :${API_PORT}`));
};
start();

process.on('SIGINT', async () => { await pool.end(); process.exit(0); });
