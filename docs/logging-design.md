# carpal ログ仕様書

対応: `docs/design.md` / `docs/ui-design.md`

## 1. 目的

- 障害切り分けを「どの層の・どのログを見るか」で即座に判断できるようにする
- デバイス・ブローカー・フロントエンドで**同一の形式・重大度・用語**を使う
- 後からログ収集（Phase 2 のバックエンド導入時）に発展できる形式にする

## 2. ログレベル（全層共通）

| Level | 意味 | 出力例 |
|---|---|---|
| `ERROR` | 機能が動作しない。即対応 | MQTT 接続失敗、GNSS モジュール応答なし |
| `WARN` | 異常だが自動復帰 or 継続可能 | 再接続実行、測位ロスト、信号弱 |
| `INFO` | 正常な状態変化・主要イベント | 接続確立、測位成功、送信開始 |
| `DEBUG` | 詳細診断（開発時のみ有効化） | 生のAT応答、受信JSON全文 |

既定で出すのは `INFO` 以上。`DEBUG` はコンパイルフラグ / 環境変数で有効化。

## 3. ログ形式

### 3.1 共通フィールド

```
<ISO8601時刻> <LEVEL> <COMPONENT> <event_id> <メッセージ> <key=value...>
```

- `COMPONENT`: `DEV`(デバイス) / `BRK`(ブローカー) / `WEB`(フロント) / `SIM`(シミュレータ)
- `event_id`: 後述のイベントID。ログの検索・集計キー

### 3.2 デバイス (シリアル 115200bps)

```
2026-11-01T23:30:12 INFO  DEV  NET_CONNECTED  LTE接続確立  rssi=-72
2026-11-01T23:30:15 INFO  DEV  GNSS_FIX       測位成功      sats=8 lat=35.681236 lng=139.767125
2026-11-01T23:30:18 INFO  DEV  PUB_SENT       送信完了      topic=carpal/default/car01/location brg=90
2026-11-01T23:31:02 WARN  DEV  GNSS_LOST      測位ロスト    age_s=65
2026-11-01T23:32:00 ERROR DEV  MQTT_FAIL      MQTT接続失敗  state=-2
```

- デバイスは RTC 未同期の可能性があるため、起動からの経過秒 `+<s>` を併記（例: `+180s INFO ...`）
- `DEBUG` 有効時は AT コマンド生応答を出力

### 3.3 ブローカー (Mosquitto / docker logs)

`mosquitto.conf` の `log_dest stdout` ＋ `log_type` を以下に設定:

```
log_type error
log_type warning
log_type notice
log_type information   # 開発時のみ。接続/購読の記録
```

| イベント | ログ例 |
|---|---|
| 接続 | `New client connected from 192.168.1.5:51000 as car01 (p2, c1, k30, u 'car01')` |
| 切断(LWT) | `Client car01 disconnected due to protocol error` / `Socket error on client car01` |
| 認証失敗 | `Client car01 disconnected, not authorised` |

※ username はデバイスごと（username = device_id）。go-auth 導入後も同様。

### 3.4 フロントエンド (ブラウザ Console)

`console[level]('[carpal]', event_id, message, data)` 形式。プレフィックス `[carpal]` でフィルタ可能にする。

```
[carpal] INFO  WS_CONNECTED  connected to wss://... 
[carpal] INFO  LOC_RECEIVED  car01  lat=35.681 lng=139.767 brg=90
[carpal] WARN  VEHICLE_STALE car01  62s since last location
[carpal] ERROR WS_ERROR      connection refused
```

- 本番では `DEBUG` を出力しない（`localStorage.carpal_debug=1` で有効化）

### 3.5 シミュレータ

デバイスと同一の event_id を使用（テスト時のログ突合のため）。`COMPONENT=SIM`。

## 4. イベントID一覧

### デバイス (DEV)

| event_id | Level | 意味 |
|---|---|---|
| `BOOT` | INFO | 起動・ファームバージョン表示 |
| `MODEM_OK` / `MODEM_FAIL` | INFO/ERROR | モデム初期化 成否 |
| `NET_CONNECTED` / `NET_LOST` | INFO/WARN | LTE 接続 確立/喪失 |
| `GNSS_FIX` / `GNSS_LOST` | INFO/WARN | 測位 成功/喪失 |
| `MQTT_CONNECTED` / `MQTT_FAIL` / `MQTT_LOST` | INFO/ERROR/WARN | MQTT 接続/失敗/喪失 |
| `PUB_SENT` / `PUB_FAIL` | INFO/ERROR | パブリッシュ 成功/失敗 |
| `MODE_CHANGE` | DEBUG | 送信間隔の切替 (移動⇔停車) |

### ブローカー (BRK)

| event_id（整理キー） | 対応Mosquittoログ |
|---|---|
| `CLIENT_CONNECT` / `CLIENT_DISCONNECT` | New client connected / disconnected |
| `AUTH_FAIL` | not authorised |
| `BROKER_START` | mosquitto version x.x running |

### フロント (WEB)

| event_id | Level | 意味 |
|---|---|---|
| `WS_CONNECTED` / `WS_ERROR` / `WS_RECONNECT` | INFO/ERROR/WARN | WebSocket 接続状態 |
| `SUBSCRIBED` | INFO | トピック購読完了 |
| `LOC_RECEIVED` / `STATUS_RECEIVED` | INFO | メッセージ受信 |
| `VEHICLE_STALE` | WARN | 60秒無更新 → オフライン補正 |
| `MSG_INVALID` | WARN | JSONパース失敗 |

## 5. 障害切り分けフロー（ログベース）

症状「地図に車両が出ない」の場合:

```
1. ブラウザ Console: WS_ERROR / SUBSCRIBED 無し？
   → ある: 通信層はOK。3へ
   → 無し: WSS接続問題 (認証/ポート/TLS)。BRK の AUTH_FAIL を確認

2. BRK に CLIENT_CONNECT (car01) はある？
   → 無し: デバイス側の問題。シリアルログで NET_CONNECTED/MQTT_FAIL を確認

3. BRK / mosquitto_sub -t 'carpal/#' -v に location は流れている？
   → 流れている: フロントの描画バグ。Console の LOC_RECEIVED と照合
   → 流れていない: DEV シリアルログで GNSS_FIX があるか確認
      GNSS_LOST続行 → 屋外/アンテナ/TTFF 待ち
      GNSS_FIXあるのにPUB_FAIL → ネットワーク帯域・broker側
```

万能観測点（これだけ覚える）:

```bash
mosquitto_sub -h <host> -u device -P <pass> -t 'carpal/#' -v
```

## 6. ログの保持・収集（現状と将来）

| 層 | 現状 | Phase 2 以降 |
|---|---|---|
| デバイス | シリアルモニタのみ（揮発） | `carpal/{tenant}/{device}/log` トピックへ WARN 以上を送信（任意） |
| ブローカー | docker logs（コンテナ再作成で消失） | Docker logging driver で json-file 回転保持 |
| フロント | Console のみ | 想定なし（必要ならエラー時に `carpal/{tenant}/{device}/log` と同形式のテナント側ログ経路へ送信） |
| 位置履歴 | なし | バックエンドが location を購読し DB へ保存（=位置の恒久ログ） |

## 7. 実装タスク

- [ ] ファーム: `LOG_*` マクロで level 出力（`CARPAL_DEBUG` 定義で DEBUG 有効）
- [ ] Mosquitto: `log_type` 設定を `docker/mosquitto/mosquitto.conf` に追記
- [ ] フロント: `web/logger.js` として `[carpal]` プレフィックスのロガーを作成し `mqtt.js` に組込み
- [ ] シミュレータ: event_id を揃える
