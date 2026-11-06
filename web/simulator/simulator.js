/**
 * simulator.js — 車両位置の疑似データを Mosquitto にパブリッシュする
 *
 * ESP32 実機がなくてもフロントエンドの開発・検証ができるシミュレータ。
 *
 * 使い方 (1プロセス = 1デバイス。ACL のため username と DEVICE_IDS を一致させる):
 *   cd web/simulator
 *   npm install
 *   MQTT_URL=mqtt://192.168.1.100:1883 MQTT_USER=car01 MQTT_PASS=xxx DEVICE_IDS=car01 node simulator.js
 *
 * 疑似車両が都心を走行します。Ctrl+C でオフライン発報して停止。
 */

import mqtt from 'mqtt';

const MQTT_URL = process.env.MQTT_URL || 'mqtt://localhost:1883';
const MQTT_USER = process.env.MQTT_USER;   // .env/環境変数必須 (ハードコード禁止)
const MQTT_PASS = process.env.MQTT_PASS;   // ハードコード禁止
const INTERVAL_MS = Number(process.env.INTERVAL_MS || 3000);
// マルチテナント対応: トピックは carpal/{tenant}/{device}/location
const TENANT_ID = process.env.TENANT_ID || 'default';
// 1プロセス = 1デバイス (ACL: username=device_id は自分の topic にしか publish できないため)。
// DEVICE_IDS でカンマ区切り指定可。未指定なら car01 のみ。
const DEVICE_IDS = (process.env.DEVICE_IDS || 'car01').split(',').map(s => s.trim());

// 車両ごとの初期位置と移動ルート (ランダムウォーク)
const ALL_VEHICLES = [
  { id: 'car01', lat: 35.681236, lng: 139.767125, bearing: 45,  speed: 40 }, // 東京駅
  { id: 'car02', lat: 35.658581, lng: 139.745433, bearing: 200, speed: 25 }, // 東京タワー
  { id: 'car03', lat: 35.710063, lng: 139.8107,   bearing: 310, speed: 55 }, // スカイツリー
];
const vehicles = ALL_VEHICLES.filter(v => DEVICE_IDS.includes(v.id));
if (vehicles.length === 0) {
  console.error(`[sim] no vehicles for DEVICE_IDS=${DEVICE_IDS}`);
  process.exit(1);
}

// clientId はプロセス間で衝突しないよう pid を含める
// (Date.now() のみだと同一ミリ秒起動で衝突し "session taken over" が無限ループする)
const client = mqtt.connect(MQTT_URL, {
  username: MQTT_USER,
  password: MQTT_PASS,
  clientId: `simulator-${DEVICE_IDS.join('-')}-${process.pid}-${Date.now()}`,
});

// interval の多重登録防止 (再接続で 'connect' が再発火しても1つだけにする)
let timerStarted = false;

client.on('connect', () => {
  console.log(`[sim] connected to ${MQTT_URL}`);
  // 契約 (docs/contract.md §3): status を Retain 付きで発報 (LWT 模擬)
  for (const v of vehicles) {
    client.publish(
      `carpal/${TENANT_ID}/${v.id}/status`,
      JSON.stringify({ id: v.id, online: true }),
      { retain: true, qos: 1 },
    );
  }
  if (!timerStarted) {
    timerStarted = true;
    setInterval(tick, INTERVAL_MS);
  }
});

// 終了時にオフラインを発報 (LWT の動作を模擬)
process.on('SIGINT', () => {
  for (const v of vehicles) {
    client.publish(
      `carpal/${TENANT_ID}/${v.id}/status`,
      JSON.stringify({ id: v.id, online: false }),
      { retain: true, qos: 1 },
    );
  }
  console.log('\n[sim] offline statuses published. bye.');
  setTimeout(() => process.exit(0), 300);
});

client.on('error', (err) => {
  console.error('[sim] error:', err.message);
  process.exit(1);
});

const EARTH_R = 6371000; // m

function move(v) {
  // 進行方向へ speed/3.6 * (interval/1000) メートル進む + ランダムな舵
  const dist = (v.speed / 3.6) * (INTERVAL_MS / 1000);
  const rad = Math.PI / 180;
  v.bearing = (v.bearing + (Math.random() - 0.5) * 20 + 360) % 360; // ±10度のゆらぎ
  v.lat += (dist * Math.cos(v.bearing * rad)) / EARTH_R / rad;
  v.lng += (dist * Math.sin(v.bearing * rad)) / (EARTH_R * Math.cos(v.lat * rad)) / rad;
  v.speed = Math.max(0, Math.min(80, v.speed + (Math.random() - 0.5) * 8)); // 速度ゆらぎ
}

function tick() {
  for (const v of vehicles) {
    move(v);
    const payload = {
      id: v.id,
      lat: Number(v.lat.toFixed(6)),
      lng: Number(v.lng.toFixed(6)),
      spd: Math.round(v.speed * 10) / 10,
      brg: Math.round(v.bearing),
      ts: Date.now(),
    };
    client.publish(`carpal/${TENANT_ID}/${v.id}/location`, JSON.stringify(payload));
    console.log(`[sim] ${v.id} -> ${payload.lat},${payload.lng} brg=${payload.brg} spd=${payload.spd}`);
  }
}
