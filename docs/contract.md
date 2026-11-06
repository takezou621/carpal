# carpal メッセージ契約（Single Source of Truth）

トピック形式・JSONスキーマ・時刻仕様の**唯一の正**。他の設計書・コードは本書に従う。
変更時は本書を先に改訂し、実装（firmware / simulator / web / ingest）を追従させる。

## 1. トピック形式

```
carpal/{tenant_id}/{device_id}/location
carpal/{tenant_id}/{device_id}/status
carpal/{tenant_id}/{device_id}/log          (診断ログ。実装開始済み — §2.1 参照)
carpal/{tenant_id}/{device_id}/cmd/ota      (Phase 3)
```

- `tenant_id`: テナント識別子。現行は固定値 `default`、SaaS化時にUUID化
- `device_id`: デバイス識別子。現行は `car01`〜、Phase 2 でUUID化（`car01` は display_name に分離）
- ワイルドカード購読は `carpal/{自テナント}/+/location` のみ許可（ACL: docs/multi-tenant-design.md §3.2）

## 2. location スキーマ

```json
{
  "id":   "car01",        // device_id。topic の device_id と一致すること（受信側は検証する）
  "lat":  35.681236,      // WGS84, 小数6桁
  "lng":  139.767125,
  "spd":  42.5,           // 対地速度 km/h
  "brg":  90,             // 進行方向 度(0=北,時計回り)。初回は省略可
  "sats": 8,              // 使用衛星数 (任意)
  "ts":   1730460000000   // 送信時刻 epoch ms。取得不可の場合は省略し、受信側が受信時刻で代用
}
```

- **時刻仕様**: `ts` は必ず epoch ms。デバイスは NITZ/GNSS の UTC から生成する（`millis()` 禁止: 周回・相対時刻のため）。未同期時はフィールドごと省略
- **ts の範囲検証**（監査 N-7）: 受信側は `ts` が妥当な範囲（例: 2020-01-01 以降、かつ現在時刻+10分以内）であることを検証し、範囲外なら**破棄せず受信時刻にフォールバック**して recorded_at を決める
- **検証ルール**（受信側: mqtt.js / ingest 共通）:
  1. `payload.id === topic内のdevice_id`（不一致は破棄＋ログ `MSG_INVALID`）
  2. `lat`/`lng` が有限数かつ (0,0) でない
  3. `lat ∈ [-90,90]`, `lng ∈ [-180,180]`

## 3. status スキーマ（Retain）

```json
{ "id": "car01", "online": true }
```

- LWT (online:false) と接続時 (online:true) のみ。Retain=true

## 3.1 log スキーマ（診断用・QoS 0・Retain なし）

```json
{
  "id":    "car01",          // device_id
  "level": "INFO",           // DEBUG | INFO | WARN | ERROR (logging-design §2)
  "event": "GNSS_WAITING",   // イベントID (logging-design §4)
  "msg":   "elapsed=120s",   // 自由記述
  "ts":    1794000000000     // 任意。時刻未同期時は省略
}
```

- 受信側 (ingest) は WARN 以上を `device_events` に記録
- 用途例: GNSS fix 待ちの進捗報告（GPS未取得時は `ts` 省略されるため受信時刻で判定）

## 4. MQTT 通信パラメータ

| 項目 | 値 | 理由 |
|---|---|---|
| location QoS | 0（デバイス→ブローカー） | リアルタイム性優先。欠落は次の送信で自己修復 |
| status QoS | 1 + Retain | 死活状態の確実な配信・後続参加者への復元 |
| ingest の購読 | **QoS 1 + persistent session (cleanSession=false)** | DB書込の欠落防止（H-4 対応）。再接続時に未取得メッセージを再配信 |
| KeepAlive | 30秒 | LWT検出時間の上限 |

- ingest の再配信による重複は、`locations` への Upsert（キー: `device_id + ts[または受信時刻]`）で冪等化する

## 5. シークレット・認証

- MQTT username = `device_id`、password = デバイスシークレット
- シークレットは2本持ち（active / previous）で、ローテーション時は previous を「失効予定」として重複期間を許容（詳細は multi-tenant-design.md §3.4）
