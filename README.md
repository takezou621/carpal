# carpal

LILYGO T-SIM7080-S3 を使ったリアルタイム車両追跡システム（Uber風）。

## 構成

```
ESP32 (T-SIM7080-S3) ──LTE/MQTT(TCP 1883)──▶ Mosquitto (Proxmox Docker)
                                                 │ WebSockets (9001 / WSS)
                                                 ▼
                                    ブラウザ Leaflet.js + 国土地理院タイル
```

## ディレクトリ

| パス | 内容 |
|---|---|
| `AGENTS.md` | **AI エージェントの開発運用ルール**（作業開始時に必読） |
| `docs/` | 設計書一式（`contract.md` がトピック/スキーマの正。詳細は docs/design.md を参照） |
| `firmware/carpal_tracker/` | ESP32 ファームウェア (Arduino) |
| `docker/mosquitto/` | Mosquitto の Docker Compose 構成 |
| `web/mqtt.js` | フロントエンドの MQTT 購読モジュール |
| `web/index.html` | 動作確認用デモページ（Leaflet 地図） |
| `web/simulator/` | ESP32 なしで動かす疑似車両シミュレータ |

## ボードなしでの開発・検証フロー

```bash
# 1. Mosquitto 起動 (docker/mosquitto/ で docker compose up -d)

# 2. シミュレータ起動 (1プロセス = 1デバイス。ACLのため username と DEVICE_IDS を一致させる)
cd web/simulator && npm install
MQTT_URL=mqtt://<host>:1883 MQTT_USER=car01 MQTT_PASS=car01pass DEVICE_IDS=car01 node simulator.js &
MQTT_URL=mqtt://<host>:1883 MQTT_USER=car02 MQTT_PASS=car02pass DEVICE_IDS=car02 node simulator.js &
MQTT_URL=mqtt://<host>:1883 MQTT_USER=car03 MQTT_PASS=car03pass DEVICE_IDS=car03 node simulator.js

# 3. デモページ表示 (ローカルサーバ推奨。file:// では ES module が動かない)
cd web && python3 -m http.server 8000
# → http://localhost:8000 を開く
```

注意: ブラウザページが http (localhost含む) なら `ws://` で OK。`web/index.html` で `window.CARPAL_MQTT_URL` / `CARPAL_MQTT_USER` / `CARPAL_MQTT_PASS` / `CARPAL_TENANT_ID` を設定する（web/mqtt.js へのハードコードは禁止）。トピック・JSONの仕様は `docs/contract.md` が正。

## Mosquitto のセットアップ

```bash
cd docker/mosquitto
# ユーザ作成 (デバイスごとに username=device_id で発行する。共用ユーザーは作らない)
docker run --rm -it -v "$PWD":/mosquitto -v "$PWD":/mosquitto/config \
  eclipse-mosquitto:2 mosquitto_passwd -c /mosquitto/config/passwd car01
docker run --rm -it -v "$PWD":/mosquitto -v "$PWD":/mosquitto/config \
  eclipse-mosquitto:2 mosquitto_passwd -b /mosquitto/config/passwd car02 <password>
docker run --rm -it -v "$PWD":/mosquitto -v "$PWD":/mosquitto/config \
  eclipse-mosquitto:2 mosquitto_passwd -b /mosquitto/config/passwd car03 <password>
docker run --rm -it -v "$PWD":/mosquitto -v "$PWD":/mosquitto/config \
  eclipse-mosquitto:2 mosquitto_passwd -b /mosquitto/config/passwd webuser <password>
docker compose up -d
```

権限は `acl_file` で分離: デバイスユーザーは publish のみ、`webuser` は subscribe のみ。

動作確認（デバイスなしでフロントをテスト可能）:

```bash
mosquitto_pub -h <host> -u car01 -P <pass> -t carpal/default/car01/location \
  -m '{"id":"car01","lat":35.681236,"lng":139.767125,"spd":40,"brg":90,"ts":1793000000000}'
```

※ `ts` は epoch ms（docs/contract.md §2）。省略も可（受信側が受信時刻で代用）。

## ファームウェア

`firmware/carpal_tracker/carpal_tracker.ino` を Arduino IDE で開き、APN / MQTT_HOST / VEHICLE_ID を変更。
必要ライブラリ: TinyGSM, PubSubClient, ArduinoJson。ボードは「ESP32S3 Dev Module」。

### 注意点

- **getGNSS のシグネチャ**: TinyGSM のバージョンにより `modem.getGNSS()` の引数が異なります。コンパイルエラー時はシグネチャに合わせて調整してください。
- **WSS**: https ページから `ws://` はブロックされる（混在コンテンツ）ため、本番はリバースプロキシ（Caddy/nginx）で TLS 終端し `wss://` 経由にしてください。LAN 内 http ページからの `ws://192.168.x.x:9001` なら直接接続可。
