/**
 * mqttProvision.mjs — Mosquitto dynsec プロビジョニング (mosquitto_ctrl 実行方式)
 *
 * dynsec の $CONTROL プロトコルは応答が要求元セッション専用に配信される特殊
 * ルーティングのため、MQTT クライアントからの直接実装は脆弱だった (実機検証)。
 * そこで公式ツール mosquitto_ctrl を execFile で実行する方式に統一した。
 * (api イメージに apk add mosquitto でインストール済み)
 *
 * - device.create → createRole + addRoleACL + createClient(secret) + enable
 * - device.rotate → setClientPassword
 * - suspend/disabled → disableClient (resume で enableClient)
 * - 30秒ループ: webuser / legacy clients / DB 全デバイスの状態を保証
 */

import { execFile } from 'node:child_process';

const MQTT_HOST = process.env.MQTT_HOST_INTERNAL || 'host.docker.internal';
const ADMIN_USER = process.env.DYNSEC_ADMIN_USER || 'admin';
const ADMIN_PASS = process.env.DYNSEC_ADMIN_PASSWORD || 'dynsec-admin-pass';
const WEBUSER_PASS = process.env.MQTT_PASS || 'webuserpass';
const LEGACY_MQTT_USERS = Object.fromEntries(
  (process.env.LEGACY_MQTT_USERS || '')
    .split(',').filter(s => s.includes(':'))
    .map(s => [s.split(':')[0].trim(), s.split(':').slice(1).join(':').trim()])
);

/** mosquitto_ctrl 実行 (dynsec モジュール) */
function ctrl(args, timeoutMs = 15000) {
  return new Promise((resolve, reject) => {
    execFile(
      'mosquitto_ctrl',
      ['-h', MQTT_HOST, '-u', ADMIN_USER, '-P', ADMIN_PASS, 'dynsec', ...args],
      { timeout: timeoutMs },
      (err, stdout, stderr) => {
        if (err) {
          // "already exists" は冪等成功として扱う
          const msg = `${stderr || ''}${stdout || ''}${err.message || ''}`;
          if (/already exist/i.test(msg)) return resolve({ exists: true });
          return reject(new Error(`ctrl ${args.join(' ')}: ${msg.slice(0, 200)}`));
        }
        resolve({ stdout });
      }
    );
  });
}

async function attachRole(username, role) {
  try { await ctrl(['addClientRole', username, role]); }
  catch (e) { console.error(`[provision] addClientRole ${username} -> ${role} failed:`, e.message); }
}

/** デバイスの dynsec role を作成 (自トピック publish のみ許可) */
async function ensureDeviceRole(tenantId, deviceId) {
  const role = `dev-${deviceId}`;
  const tenantPat = tenantId || '+';   // DB行なし legacy は全テナント許可
  try { await ctrl(['createRole', role]); } catch { /* already exists */ }
  try { await ctrl(['addRoleACL', role, 'publishClientSend', `carpal/${tenantPat}/${deviceId}/#`, 'allow']); } catch { /* already */ }
  return role;
}

const WEBVIEWER_ROLE = 'webviewer';

/** webuser と閲覧用 role の保証 */
async function ensureWebuser() {
  try { await ctrl(['createRole', WEBVIEWER_ROLE]); } catch { /* exists */ }
  try { await ctrl(['addRoleACL', WEBVIEWER_ROLE, 'subscribePattern', 'carpal/+/+/location', 'allow']); } catch { /* already */ }
  try { await ctrl(['addRoleACL', WEBVIEWER_ROLE, 'subscribePattern', 'carpal/+/+/status', 'allow']); } catch { /* already */ }
  try { await ctrl(['addRoleACL', WEBVIEWER_ROLE, 'subscribePattern', 'carpal/+/+/log', 'allow']); } catch { /* already */ }
  try { await ctrl(['addRoleACL', WEBVIEWER_ROLE, 'subscribePattern', 'carpal/+/+/#', 'allow']); } catch { /* already */ }
  try { await ctrl(['createClient', 'webuser', '-p', WEBUSER_PASS]); }
  catch { await ctrl(['setClientPassword', 'webuser', WEBUSER_PASS]); }
  await attachRole('webuser', WEBVIEWER_ROLE);
}

/** デバイス登録・ローテーション時に呼ぶ (secret が既知のタイミング) */
export async function provisionDeviceClient(tenantId, deviceId, secret) {
  const role = await ensureDeviceRole(tenantId, deviceId);
  // NOTE: 2.1.2 の createClient に -r は存在しない (付けても role 未接続になる)
  try {
    await ctrl(['createClient', deviceId, '-p', secret]);
  } catch {
    await ctrl(['setClientPassword', deviceId, secret]);   // positional password
  }
  await attachRole(deviceId, role);
  await ctrl(['enableClient', deviceId]);
  console.log(`[provision] mqtt client ready: ${deviceId}`);
}

/** 有効/無効化 (suspend・disabled 切替時) */
export async function setDeviceEnabled(deviceId, enabled) {
  await ctrl([enabled ? 'enableClient' : 'disableClient', deviceId]);
}

/** 定期ループ: webuser / legacy / DB 全デバイスの状態を保証 */
export function startProvisioning(pool) {
  const ensureAll = async () => {
    try {
      await ensureWebuser();

      // 過去資産: 環境変数で与えられた legacy パスワード (実機再構成後に削除)
      for (const [dev, pass] of Object.entries(LEGACY_MQTT_USERS)) {
        const { rows } = await pool.query(
          `SELECT d.tenant_id, d.disabled, t.status
           FROM devices d JOIN tenants t ON t.id = d.tenant_id WHERE d.id::text = $1`, [dev]);
        if (rows[0]) {
          const enabled = !rows[0].disabled && rows[0].status === 'active';
          const role = await ensureDeviceRole(rows[0].tenant_id, dev);
          try { await ctrl(['createClient', dev, '-p', pass]); }
          catch { await ctrl(['setClientPassword', dev, pass]); }
          await attachRole(dev, role);
          await ctrl([enabled ? 'enableClient' : 'disableClient', dev]);
        } else {
          // DB行なしの legacy デバイス → 作成して有効化 (実機の可能性)
          const role = await ensureDeviceRole('+', dev);
          try { await ctrl(['createClient', dev, '-p', pass]); }
          catch { await ctrl(['setClientPassword', dev, pass]); }
          await attachRole(dev, role);
          await ctrl(['enableClient', dev]);
        }
      }

      const { rows: devices } = await pool.query(
        `SELECT d.id, d.disabled, d.tenant_id, t.status FROM devices d
         JOIN tenants t ON t.id = d.tenant_id`);
      for (const d of devices) {
        if (LEGACY_MQTT_USERS[d.id]) continue;
        const enabled = !d.disabled && d.status === 'active';
        await ctrl([enabled ? 'enableClient' : 'disableClient', d.id]);
      }
    } catch (e) {
      console.error('[provision] ensureAll:', e.message);
    }
  };

  ensureAll();
  setInterval(ensureAll, 30000);
  console.log('[provision] loop started (30s, ctrl mode)');
}

export function stopProvisioning() { /* ctrl 方式では常設接続なし */ }
