/**
 * mqtt.js — Leaflet フロントエンドから MQTT (WebSockets) 経由で車両位置を購読する
 *
 * 使い方:
 *   import { startVehicleSubscription } from './mqtt.js';
 *   startVehicleSubscription(map, (vehicleId, data) => { /* 描画コールバック *\/ });
 *
 * 前提: mqtt.js ライブラリを読み込むこと
 *   <script src="https://unpkg.com/mqtt/dist/mqtt.min.js"></script>
 *
 * 注意: ブラウザから WSS 接続する場合、Mosquitto 側に TLS (リバースプロキシ等) が必須。
 *       混在コンテンツ (https ページから ws://) はブラウザがブロックします。
 */

// 接続先・認証情報はハードコードしない (監査 C-2 対応)。
// window.CARPAL_MQTT_URL / CARPAL_MQTT_USER / CARPAL_MQTT_PASS / CARPAL_TENANT_ID を
// index.html 側で設定する。JWT 認証 (Phase 2) 導入まで、この画面を公開ネットワークに出さないこと。
const MQTT_URL = window.CARPAL_MQTT_URL
  || (location.protocol === 'https:' ? 'wss://' + location.hostname + ':9001'
                                     : 'ws://' + location.hostname + ':9001');
const MQTT_OPTIONS = {
  username: window.CARPAL_MQTT_USER || 'webuser',
  password: window.CARPAL_MQTT_PASS || '',  // 未設定なら Mosquitto 側で認証拒否される
  reconnectPeriod: 3000,      // 自動再接続 3秒
  keepalive: 30,
};

export function startVehicleSubscription(onUpdate, onStatus) {
  const client = mqtt.connect(MQTT_URL, MQTT_OPTIONS);

  client.on('connect', () => {
    console.log('[carpal]', 'WS_CONNECTED', 'connected');
    // 複数車両をまとめて購読 (+ はワイルドカード)
    // マルチテナント形式: carpal/{tenant_id}/{device_id}/location
    const TENANT_ID = window.CARPAL_TENANT_ID || 'default';
    client.subscribe(`carpal/${TENANT_ID}/+/location`, { qos: 0 });
    client.subscribe(`carpal/${TENANT_ID}/+/status`,   { qos: 0 });
  });

  client.on('message', (topic, payload) => {
    let data;
    try {
      data = JSON.parse(payload.toString());
    } catch (e) {
      console.warn('[MQTT] invalid JSON:', topic);
      return;
    }

    // topic 例: carpal/default/car01/location
    const parts = topic.split('/');
    const [, , deviceId, kind] = parts;

    // 契約検証 (docs/contract.md §2): id ≡ topic の device_id
    if (data.id && data.id !== deviceId) {
      console.warn('[carpal] MSG_INVALID id/topic mismatch:', topic, data.id);
      return;
    }

    if (kind === 'location' && onUpdate) {
      const lat = Number(data.lat), lng = Number(data.lng);
      // 契約検証: 数値・範囲 (0,0) 除外
      if (!Number.isFinite(lat) || !Number.isFinite(lng) ||
          (lat === 0 && lng === 0) ||
          lat < -90 || lat > 90 || lng < -180 || lng > 180) {
        console.warn('[carpal] MSG_INVALID coords:', topic, lat, lng);
        return;
      }
      onUpdate(deviceId, {
        lat, lng,
        speed: Number(data.spd) || 0,
        bearing: data.brg != null ? Number(data.brg) : null,  // 無い場合はフロントで差分計算にフォールバック
        ts: data.ts,
      });
    } else if (kind === 'status' && onStatus) {
      onStatus(deviceId, { online: !!data.online });
    }
  });

  client.on('reconnect', () => console.log('[carpal]', 'WS_RECONNECT', 'reconnecting...'));
  client.on('error', (err) => console.error('[carpal]', 'WS_ERROR', err.message));

  return client;
}
