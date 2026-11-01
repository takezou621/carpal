# Carpal

LILYGO T-SIM7080-S3（ESP32-S3 + SIM7080G）で車両の位置を追跡し、
外付け加速度センサーとカメラで駐車中の衝撃を記録する最小構成です。

- **位置表示**: GNSS → LTE-M / NB-IoT → Python API → 地図。
  複数車両のマーカー、速度、オンライン状態を 2 秒ごとに更新します。
  端末の送信周期は最短 10 秒で、測位・接続時間によって長くなります。
- **駐車監視**: 画面で監視を有効化すると、端末が ADXL345 を 50 Hz で読み、
  重力・取り付け方向を除いた加速度変化が閾値を超えた時に JPEG を撮影します。
  イベントと写真は SQLite に保存され、画面から確認できます。
- **認証**: 位置・写真・監視設定 API は共通の API トークンで保護します。
  端末は証明書検証付き HTTPS のみ使用します。

## 必要なハードウェア

- T-SIM7080-S3、GNSS / LTE アンテナ、対応 SIM と契約済み APN
- ADXL345 の **3.3 V 対応** I²C モジュール
- ボードの公式カメラコネクターに対応した JPEG 対応カメラ（例: OV2640）
- 安定した電源。車載電源への接続には適切な降圧・保護回路が必要です。
  車両の 12 V / 24 V をボードへ直接接続しないでください。

### 配線

電源を切って配線してください。ADXL345 は外付けで、ボード内蔵ではありません。

| ADXL345 | 接続先 |
| --- | --- |
| VCC / GND | ボードの 3.3 V / GND |
| SDA / SCL | GPIO15 / GPIO7（PMU と共有） |
| CS | 3.3 V（I²C モード） |
| SDO / ALT ADDRESS | GND（アドレス `0x53`） |

GPIO15 / GPIO7 の取り出し可否は基板・コネクターを確認してください。
プルアップは 3.3 V に接続し、センサーを車体に固定してください。
カメラは公式コネクターを使います。一般的な ESP32-CAM の配線は使えません。

ファームウェアの GPIO / 電源設定は
[LILYGO 公式サンプル](https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G/tree/39f7886a9436c8a460eb50c86e2d6c8e21de9b1e/examples)
に基づいています。基板リビジョンとカメラを実装前に照合してください。

| 機能 | GPIO / 電源 |
| --- | --- |
| モデム RX / TX / PWR / DTR | 4 / 5 / 41 / 42 |
| PMU I²C SDA / SCL | 15 / 7 |
| モデム / UART / GNSS アンテナ電源 | DC3 3.0 V / BLDO1 3.3 V / BLDO2 3.3 V |
| カメラ D0–D7 | 14, 47, 48, 21, 13, 11, 10, 9 |
| カメラ XCLK / PCLK / VSYNC / HREF | 8 / 12 / 16 / 17 |
| カメラ SCCB SDA / SCL / RESET | 2 / 1 / 18 |
| カメラ電源 | ALDO1 1.8 V / ALDO2 2.8 V / ALDO4 3.0 V |

## サーバーと画面

Python 3.10 以降のみで動作します。追加 Python パッケージは不要です。
リポジトリのルートで実行します。

```sh
export CARPAL_TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')"
python3 server.py
```

`http://127.0.0.1:8080` を開き、同じトークンを入力して接続します。
トークンは画面のメモリー内だけで保持し、切断・再読み込みで消去します。
車両は最初の端末通信後に表示されます。車両を選ぶと地図を移動し、
監視イベントを表示します。「駐車監視を有効化」は**次の端末通信で反映**されます。
最終通信または測位から 30 秒経過すると、オフラインまたは古い位置として表示します。
GNSS が未測位の場合、架空の `(0, 0)` を表示しません。

DB は既定で `carpal.sqlite3`。`--database`, `--host`, `--port` で変更できます。
DB には機微な位置情報・写真が含まれるため、アクセス権とバックアップを管理してください。

### インターネット公開

**Python サーバーの平文 HTTP ポートを直接公開しないでください。**
これは小規模なプロトタイプ用サービスです。Caddy / nginx 等の HTTPS リバースプロキシで
独自ドメイン・有効な証明書を設定し、API も画面も同じ HTTPS オリジンで公開します。
Python は既定の `127.0.0.1` にバインドしたまま運用してください。

- `Authorization` を上流に転送し、レスポンスの `Content-Length` を保持してください。
  ファームウェアは chunked レスポンス・リダイレクトに対応しません。
- リクエスト上限を 300 KB、レート制限・接続数制限・上流タイムアウトを設定してください。
- トークンは 32 文字以上のランダム ASCII 値とし、ログや URL に含めないでください。
  複数ユーザー向けの権限分離はありません。トークン所有者は全車両を操作できます。
- DB の保持期限・容量監視・古い写真の削除を運用側で設定してください。
  一覧は最新 50 件ですが DB 自体は自動削除しません。
- 地図は Leaflet 1.9.4 と OpenStreetMap タイルを使用します。外部ネット接続が必要です。
  タイル取得先に閲覧範囲が伝わります。公開運用ではタイルの利用ポリシーを確認してください。

## ファームウェア

PlatformIO Core をインストールして、次を実行します。

```sh
cp firmware/include/config.example.h firmware/include/config.h
# config.h の VEHICLE_ID, APN, SERVER_HOST, API_TOKEN などを設定
pio run --project-dir firmware
pio run --project-dir firmware --target upload
pio device monitor --project-dir firmware
```

`config.h` は git 管理対象外です。端末ごとに一意の `VEHICLE_ID`
（英数字・`_`・`-`、1～64 文字）を指定します。
`API_TOKEN` はサーバーの `CARPAL_TOKEN` と同じ値です。
PSRAM と 16 MB フラッシュを前提にしています。カメラなしで試す場合は
`CAMERA_ENABLED = false` とします。ADXL345 は必須です。

### SIM7080 の TLS 設定

`MODEM_CA_FILE` は PEM の内容ではなく、**SIM7080 のファイルシステムに
事前配置した CA 証明書のファイル名**です。SIMCom の対象モデムファームウェアの
SSL / ファイル操作手順に従って、サーバー証明書の発行元 CA を登録してください。
ESP32 のフラッシュへのファイル配置だけでは登録されません。
携帯網でモデムの時計を同期し、証明書の有効期間内であることを確認してください。

CA 登録には、メーカーの手順に従って `AT+CFSINIT`、
`AT+CFSWFILE=3,"carpal-ca.pem",0,<PEMの正確なバイト数>,10000` を実行し、
`DOWNLOAD` 後に PEM バイト列を送信します。`OK` を確認して
`AT+CSSLCFG="convert",2,"carpal-ca.pem"`、`AT+CFSTERM` を実行します。
[公式の証明書登録例](https://github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G/blob/39f7886a9436c8a460eb50c86e2d6c8e21de9b1e/examples/ModemMqttsAuthExample/ModemMqttsAuthExample.ino)
も参照してください。例示されている証明書・秘密鍵は使用しないでください。

ファームウェアは `AT+CSSLCFG="authmode",0,1` でサーバー認証を要求し、
`AT+CSSLCFG="ignorertctime",0,0` による日付検証を要求します。
TinyGSM の `setCertificate()` で CA を指定し、接続時の CA 設定失敗は接続を拒否します。
これらを受け付けないモデム、未登録 CA、期限切れ・不一致の証明書の場合は送信しません。
本番投入前に、正常な証明書では通信でき、未知 CA・異なるホスト名・期限切れでは
必ず接続を拒否することを実機で検証してください。検証を無効化して回避しないでください。

### 動作と制限

- SIM7080 の公式例に合わせて GNSS と携帯通信を切り替えます。
  測位ウィンドウは 15 秒、最短送信間隔は 10 秒。初回測位や圏外では長くなります。
  常時低遅延のライブ動画や、Uber 相当のサービス全体を実装するものではありません。
- 加速度変化の閾値は既定 `0.35 g`、同一衝撃の連続記録を抑える間隔は 30 秒です。
  `SHOCK_THRESHOLD_G`, `EVENT_COOLDOWN_MS` を取り付け状態に合わせて調整してください。
  これは衝撃監視であり、映像解析・人物検出・衝撃を伴わない盗難検出は行いません。
- 監視設定は端末 NVS に保持します。通信中・圏外でも加速度の読み取りは続きます。
  カメラ失敗時は写真なしで衝撃イベントを送信します。
- JPEG は VGA、最大 200 KB。未送信イベントは **RAM に 3 件まで**保持し再送します。
  満杯時は新規イベントを破棄し、再起動・電源断で未送信データは失われます。
  長期間の圏外記録には SD 等の永続キューを別途設計する必要があります。
- イベント画面の時刻は**サーバー受信時刻**であり、圏外時の撮影時刻ではありません。
- 実機・SIM・キャリアを使った測位、電源、衝撃、撮影、TLS の検証が必要です。
  このリポジトリのソフトウェアテストだけではハードウェア動作を保証できません。
  車内・周辺の撮影はプライバシーと地域の法令に配慮してください。

## API

すべての `/api/` 呼び出しで `Authorization` ヘッダーに
`Bearer`、半角スペース、API トークンをこの順に設定してください。
POST は `Content-Type: application/json` と `Content-Length` を指定します。

| メソッド / パス | 内容 |
| --- | --- |
| `POST /api/telemetry` | `{"vehicle":"car-1","position":{"lat":35.68,"lon":139.76,"speed_kmh":0,"age_seconds":0}}`。未測位は `position: null`。監視設定を返す |
| `GET /api/vehicles` | 最終位置・受信時刻・監視設定 |
| `POST /api/vehicles/car-1/parking` | `{"parking_armed":true}` または `false` |
| `POST /api/events` | `vehicle`, `event_id`, `acceleration_g`、任意の Base64 `image_jpeg` |
| `GET /api/events?vehicle=car-1` | 最新 50 件（画像本体を除く） |
| `GET /api/events/1/image` | 認証付き JPEG 取得 |

イベント送信前にテレメトリーを送信してください。`vehicle` と `event_id` の組で
重複を防止するため、同一イベントの再送時は ID を変更しないでください。

## ソフトウェア検証

```sh
python3 -m unittest discover -s tests -v
node --check web/app.js
pio run --project-dir firmware
```

API テストは一時 DB と実 HTTP サーバーで、認証、座標・画像検証、監視の切り替え、
測位欠落時の位置保持、永続化、イベントの重複防止を確認します。