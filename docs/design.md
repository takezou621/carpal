# carpal 全体設計書

LILYGO T-SIM7080-S3 を用いたリアルタイム車両追跡システム（最大3台・Uber風表示）

> **改訂 (2026-11)**: マルチテナント化（`docs/multi-tenant-design.md`）に伴い、
> トピック形式・認証・バックエンド方針を改訂済み。トピック/スキーマの正は
> **`docs/contract.md`**。本書と他書が食い違う場合は contract.md > multi-tenant-design.md > 本書 の優先順で従う。

## 1. システム全体アーキテクチャ

```
┌─────────────────────┐         ┌──────────────────────────────┐
│  車載デバイス ×3      │         │  自宅 Proxmox (Docker)        │
│  T-SIM7080-S3       │         │                              │
│  ┌───────────────┐  │  MQTT   │  ┌────────────────────────┐  │
│  │ GNSS (SIM7080G)│  │  TCP    │  │ Mosquitto              │  │
│  │ 緯度経度/速度    │  ├─1883──▶ │  │  :1883 mqtt  (デバイス) │  │
│  └───────┬───────┘  │ ACL分離  │  │  :9001 ws/wss (ブラウザ)│  │
│          ▼          │         │  └──────────┬─────────────┘  │
│  ┌───────────────┐  │         │             │                │
│  │ ファームウェア  │  │         │  ┌──────────▼─────────────┐  │
│  │ Arduino        │  │         │  │ (Phase 2) バックエンド   │  │
│  │ 適応送信/LWT    │  │         │  │ API(Fastify) + ingest   │  │
│  └───────────────┘  │         │  │ PostgreSQL・テナント管理 │  │
└─────────────────────┘         │  └────────────────────────┘  │
                                └──────────────┬───────────────┘
                                               │ WSS
                                 ┌─────────────▼───────────────┐
                                 │  ブラウザ (テナント利用者)     │
                                 │  Leaflet.js + 国土地理院タイル │
                                 │  車両表示・回転・軌跡・状態     │
                                 └─────────────────────────────┘
```

### 設計原則

1. **段階的アーキテクチャ**: Phase 1 は Mosquitto が中継を兼ねる最小構成。Phase 2 でテナント管理・履歴保存のため API + ingest + PostgreSQL を追加（`docs/multi-tenant-design.md` §4）。
2. **関心の分離**： デバイスは「測位して流すだけ」、表示はブラウザが担う。変換・集計は置かない（YAGNI）。
3. **契約駆動**： トピック・JSONスキーマ・時刻仕様は `docs/contract.md` に一元化。実デバイスとシミュレータは完全置換可能。
4. **テナント分離は ACL で成立させる**: Phase 1 から静的 ACL、Phase 2 で go-auth による動的 ACL へ（`docs/multi-tenant-design.md` §3）。

## 2. コンポーネント設計

### 2.1 車載デバイス（T-SIM7080-S3 ファームウェア）

| 項目 | 設計 |
|---|---|
| 測位 | SIM7080G 内蔵 GNSS。TinyGSM の `getGNSS()` で緯度経度・速度・衛星数を取得 |
| 測位判定 | lat/lng が (0,0) の間は「未測位」とし送信しない。HDOP/衛星数で品質判定 |
| 通信 | LTE Cat-M。APN 接続 → TinyGsmClient → PubSubClient (TCP 1883) |
| 送信ポリシー | **移動中 (≥5km/h): 3秒間隔 / 停車中: 30秒間隔**。帯域・電池・パケット課金の削減 |
| 進行方向 | デバイス側で前回測位とのベアリングを計算し `brg` に格納。同時にフロント側でも差分計算可能（フォールバック二段構え） |
| 死活監視 | MQTT LWT (Last Will) で `carpal/{tenant}/{device}/status` に `online:false` を自動発報。接続復帰時に `online:true` を Retain 付きで発報 |
| 再接続 | ネットワーク断・MQTT断とも loop 内で自動検出・再接続。KeepAlive 30秒 |

### 2.2 ブローカー（Mosquitto / Proxmox Docker）

| 項目 | 設計 |
|---|---|
| リスナー | TCP 1883 (デバイス用) / WebSockets 9001 (ブラウザ用) を分離 |
| 認証 | `password_file` によるユーザー認証（Mosquitto 2.x は匿名拒否がデフォルト）。**username = device_id**（car01 など。共用ユーザーは使わない）＋ `webuser`。Phase 2 で go-auth による動的認証へ移行 |
| TLS | **現状**: LAN 内は平文で許容。**Phase 2**: 外部公開時は Caddy/nginx で TLS 終端し 443 → 9001 を WSS としてプロキシ。デバイス側は 8883 をリスナー追加 |
| ACL | `acl_file` で**導入済み**: デバイス= `carpal/#/location`,`/status` へ publish のみ / webuser= `carpal/+/+/location`,`/status` を subscribe のみ（`docker/mosquitto/acl_file`） |
| 永続化 | Retain メッセージ用に volume マウントで persistence 有効 |

### 2.3 フロントエンド（Leaflet）

| 項目 | 設計 |
|---|---|
| 地図 | Leaflet 1.9 + 国土地理院 淡色地図 (`cyberjapandata.gsi.go.jp/xyz/pale`) |
| 購読 | `carpal/{tenant}/+/location` と `carpal/{tenant}/+/status` をワイルドカード購読（mqtt.js ブラウザ版。テナントは `window.CARPAL_TENANT_ID` で指定、既定 `default`） |
| 車両表示 | divIcon（▲SVG）。CSS transition で回転を補間しスムーズな方向転換 |
| 方向 | `brg` を優先、無ければ移動差分からベアリング計算 |
| 軌跡 | 車両ごとに直近100点のポリライン |
| 状態表示 | 60秒間更新が無い車両は「オフライン」表示（LWT受信でも更新） |
| 接続断対応 | mqtt.js の自動再接続 (3秒) に委譲。UI に接続状態を表示 |

### 2.4 シミュレータ（開発用）

Node.js + mqtt.js。実デバイスと同一トピック・同一スキーマで3台分の疑似走行データを発行。デバイス到着前のフロント開発、結合テストのリグレッションに使用。

## 3. データ設計（コントラクト）

> **正は `docs/contract.md`**。本節は概要のみ。

### 3.1 トピック設計

```
carpal/{tenant_id}/{device_id}/location   位置情報 (デバイス → ブローカー, QoS 0)
carpal/{tenant_id}/{device_id}/status     在線状態 (LWT含む, Retain=true, QoS 1)
```

- `tenant_id`: 現行は固定値 `default`（SaaS化時にUUID化）
- `device_id`: `car01` 〜 `car03`（Phase 2 でUUID化、`car01` は display_name に分離）
- 将来的に `carpal/{tenant}/{device}/cmd/...`（コマンド）等を追加可能な階層構造

### 3.2 メッセージスキーマ

**location**（`carpal/default/car01/location`）:

```json
{
  "id":   "car01",       // device_id (topic と一致。受信側は検証する)
  "lat":  35.681236,     // 緯度 (WGS84, 小数点以下6桁 ≒ 0.1m)
  "lng":  139.767125,    // 経度
  "spd":  42.5,          // 対地速度 km/h
  "brg":  90,            // 進行方向 (度, 0=北, 時計回り)。初回は省略可
  "sats": 8,             // 使用衛星数 (品質判定用・任意)
  "ts":   1730460000000  // 送信時刻 epoch ms。NITZ/GNSS UTC から生成。未同期時は省略
}
```

**status**（`carpal/default/car01/status`, Retain）:

```json
{ "id": "car01", "online": true }
```

### 3.3 契約上のルール

- フィールド追加は**後方互換**で行う（受け手は未知フィールドを無視）
- 緯度経度は (0,0) を送らない（未測位時は送信自体をしない）
- 受信側は `payload.id ≡ topic の device_id` と緯度経度の妥当性を検証し、不一致は破棄する
- 実デバイス・シミュレータともこのスキーマを厳守 → 完全置換可能

## 4. 通信シーケンス

### 4.1 通常走行

```
デバイス                    Mosquitto                  ブラウザ
   │ CONNECT (LWT設定)          │                          │
   ├───────────────────────────▶│                          │
   │ PUB status online:true(Retain)                        │
   ├───────────────────────────▶│──status─────────────────▶│ 「走行中」表示
   │ (3秒毎) PUB location        │                          │
   ├───────────────────────────▶│──location───────────────▶│ 地図更新・回転
```

### 4.2 デバイス異常終了（電源断・圏外持続）

```
   ✕ (KeepAlive 30秒超過 / TCP切断)
                              │ PUB status online:false (LWT)
                              ├─────────────────────────▶│ 「オフライン」表示
```

### 4.3 ブラウザ再接続

mqtt.js が 3秒間隔で自動再接続 → Retain された status を受信して即座に在線状態を復元。

## 5. セキュリティ設計

| 層 | 現状 (LAN) | 将来 (外部公開) |
|---|---|---|
| 通信 | 平文 MQTT / ws | WSS (Caddy で TLS 終端) / mTLS or TLS 8883 |
| 認証 | ユーザー/パス（username=device_id）＋ **ACL 導入済み** | go-auth で動的 ACL（JWT・テナント分離） |
| デバイス偽装 | ACL で publish は location/status のみに制限 | デバイス毎の専用ユーザー＋自トピック限定 ACL＋`id≡topic` 検証 |
| Web 公開 | なし（LAN限定） | リバースプロキシ＋JWT認証（mqtt.js への資格情報ハードコード禁止済み） |

混在コンテンツ注意: https ページから `ws://` はブラウザがブロック。外部公開時は必ず WSS。

## 6. 保守性・拡張性

- **Phase 2（履歴・再生・テナント管理）**: API (Fastify) + ingest + PostgreSQL を追加（`docs/multi-tenant-design.md` §4）。ingest は **QoS 1 + persistent session** で `carpal/+/+/location` を購読し DB へ保存。Mosquitto の `$SYS` トピックで統計も取得可能
- **台数拡張**: トピック階層とスキーマはそのまま。Mosquitto は数千クライアントまで余裕
- **障害切り分け**: デバイスはシリアルログ、ブローカーは `log_dest stdout`（docker logs）、フロントはブラウザコンソール。`mosquitto_sub -t 'carpal/#' -v` が万能観測点

## 7. リポジトリ構成

```
carpal/
├── docs/                         設計書一式
│   ├── design.md                 ← 本書 (全体設計)
│   ├── contract.md               トピック/スキーマ/時刻の契約 (正)
│   ├── multi-tenant-design.md    マルチテナント (SaaS化) 設計
│   ├── backend-design.md         バックエンド詳細設計 (Phase 2 実装仕様)
│   ├── admin-design.md           Admin 要件・仕様
│   ├── ui-design.md              ブラウザ UI 設計
│   ├── logging-design.md         ログ仕様
│   ├── operations-design.md      運用設計 (障害・バックアップ・監視)
│   ├── cicd-design.md            CI/CD 仕様
│   └── legal-compliance.md       法務・同意・SLA
├── firmware/carpal_tracker/      ESP32 ファームウェア (Arduino)
├── docker/mosquitto/             Mosquitto 構成 (compose, conf, acl_file)
├── scripts/check.sh              ローカル検証スクリプト
├── web/
│   ├── index.html                デモページ (Leaflet)
│   ├── mqtt.js                   MQTT 購読モジュール
│   └── simulator/                疑似車両シミュレータ (Node.js)
└── README.md
```

## 8. 開発フェーズと進捗

| Phase | 内容 | 状態 |
|---|---|---|
| 1-a | デモページ (Leaflet + 地図描画) | ✅ 完了 |
| 1-b | 複数車両のリアルタイム表示・回転アニメ | ✅ 完了 |
| 1-c | Mosquitto 構成 (Docker, ACL込み) | ✅ 設計済み・要実機セットアップ |
| 1-d | MQTT 購読モジュール + デモ統合 | ✅ 完了 (要 WSS 接続テスト) |
| 1-e | シミュレータ | ✅ 完了 (status Retain発報・SIGINTオフライン含む) |
| 1-f | マルチテナント化 (トピック新形式, ACL, 認証情報の外部化) | ✅ 完了 |
| 2-a | Mosquitto セットアップ + シミュレータ→ブラウザ E2E | ✅ 完了 (2026-11-02: ACL拒否テスト・WS購読込み。ブラウザ描画は目視確認待ち) |
| 2-b | バックエンド基盤 (PostgreSQL + API + ingest + Caddy) | 🔜 次 (multi-tenant §8 / backend-design) |
| 2-c | テナント/デバイス管理 API・JWT 認証・go-auth | 未着手 |
| 2-d | フロント UI (ログイン・テナントスイッチャー・ui-design M1〜M3) | 未着手 |
| 2-e | ESP32 実機ファーム検証（デバイス到着後） | 未着手 (ボード未入手) |
| 3 | 外部公開 (WSS)、Admin コンソール、OTA、SaaS 化 (課金) | 未着手 |

### リスクと対処

| リスク | 対処 |
|---|---|
| GNSS の初回測位 (TTFF) に数分かかる | 屋外テスト前提。ファームは「未測位でも送信しない」設計で誤表示を防止 |
| SIM の APN/契約が Cat-M 非対応 | 事前に契約プラン確認 (IIJmio, SORACOM 等は Cat-M 対応) |
| 自宅回線のグローバル IP 変動 | DDNS or Tailscale/VPN 経由で Mosquitto に接続 |
| ブラウザと Mosquitto の混在コンテンツ | LAN 内開発は http ページ + ws://、公開時は WSS |
