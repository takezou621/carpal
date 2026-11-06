# carpal マルチテナント（テナント型）設計書

目的: 個人プロジェクトとして開始しつつ、将来の SaaS 化を見据えて**複数テナント（利用者/事業所）がそれぞれ自分の車両群のみを管理・閲覧できる**構造にする。

関連: `docs/design.md`（システム全体） / `docs/ui-design.md` / `docs/logging-design.md` / `docs/cicd-design.md`

## 1. SaaS 化を見据えた基本方針

| 方針 | 内容 |
|---|---|
| **Shared Broker / Logical Isolation** | Mosquitto は全テナント共有。分離はトピック階層＋ACLで論理的に行う。テナント数が増えるまでは物理分離（ブローカー増設）しない |
| **バックエンドを Phase 2 から前倒し** | テナント・ユーザー・車両の登録は HTTP API で行う必要があるため、履歴保存と同時にバックエンドを導入する（当初「サーバレス」方針を改訂） |
| **IDはUUID化** | vehicle_id 等に連番ではなく UUID を使い、テナント跨ぎの推測・衝突を防ぐ。表示名（`car01`）は別フィールド |
| **スキーマは最初からテナント込み** | トピックとJWTを後から変更するとデバイスの再書き込みが必要。**今のうちに確定させる** |
| **単一テナント運用は「default テナント」に集約** | 今は `default` テナント 1つで運用し、SaaS 化時のコード変更を最小化 |

## 2. トピック設計（改訂）

### 現行（単一テナント）

```
carpal/{vehicle_id}/location
```

### 新設計（テナント階層を追加）

```
carpal/{tenant_id}/{device_id}/location
carpal/{tenant_id}/{device_id}/status
carpal/{tenant_id}/{device_id}/log          # Phase 2 (WARN以上)
carpal/{tenant_id}/{device_id}/cmd/ota      # Phase 3
```

- `tenant_id`: テナントの UUID（例 `a1b2c3d4` 短縮UUID）
- `device_id`: デバイス UUID。表示名 `car01` はメタデータとしてバックエンドが管理
- ワイルドカード購読は `carpal/{自テナント}/+/location` のみ許可 → **ACL だけでテナント分離が成立**

### 互換性対応

- 現行デバイス/シミュレータは `carpal/{vehicle_id}/...` で発信 → **移行期間中はブローカー側で旧形式を `default` テナントへ正規化**しない（複雑化するため）。代わりにデバイス・シミュレータ・フロントを同時に新形式へ更新する（現状コード量が少なく、移行コストは最小）

## 3. 認証・認可設計

### 3.1 認証の階層

| 主体 | 認証方法 | Phase |
|---|---|---|
| デバイス | MQTT ユーザー/パス（デバイスごとに発行、username = device_id） | 1〜 |
| ブラウザ | バックエンドでログイン → **JWT 発行** → mqtt.js は JWT を MQTT パスワードとして接続 | 2〜 |
| 管理 API | JWT (Authorization: Bearer) | 2〜 |

### 3.2 MQTT 動的認証（ポイント）

> **実装決定 (2026-11-06)**: go-auth プラグインの代わりに **Mosquitto 組込み Dynamic Security (dynsec)**
> を採用 (ビルド不要・2.1系で公式サポート)。API が `mosquitto_ctrl` (api イメージに内蔵) を実行し、
> device.create/rotate/suspend に連動して dynsec クライアント/role を動的管理する。
> 詳細実装: `backend/api/mqttProvision.mjs` + `docker/mosquitto/mosquitto.conf` (plugin 行)。



テナントが増えるたびに `passwd` を手配布はできないため:

- **Mosquitto の動的認証プラグイン**（例: `mosquitto-go-auth`）で、認証要求時にバックエンド/DBへ問い合わせる
  - デバイス: `device_id` + デバイスシークレット → DB照合
  - ブラウザ: JWT 検証 → テナント/ロール抽出
- **ACL も同プラグインで動的生成**:
  - デバイス: `carpal/{自テナント}/{自device_id}/#` の **publish のみ**
  - ブラウザ: `carpal/{自テナント}/+/status`,`/location` の **subscribe のみ**（管理者ロールは log/cmd も許可）

→ 「ブローカーにユーザーを足す運用」が消え、SaaS 化の最大の障害が解消される。

### 3.4 JWT ライフサイクル（監査 H-6 対応）

| 項目 | 仕様 |
|---|---|
| TTL | アクセストークン **15分**。フロントはリフレッシュトークン（7日・httpOnly cookie）で自動更新 |
| 失効 | go-auth は認証時に **users.disabled / tenants.status / memberships を DB 照合**するため、suspend・無効化・メンバー除去は即時（≈次回認証時）反映。失効リストは持たない（短TTLで代替） |
| トピック照合 | JWT の `tid` と要求トピックの tenant 階層が一致するかを ACL プラグインが検証 |
| デバイスシークレット | 2本持ち（active/previous）。ローテーション時は previous を重複期間（デフォルト24時間）だけ有効とし、devices.secret_rotated_at で管理 |
| 2FA | システム管理者は必須（admin-design §8）。TOTP |

### 3.3 ロールモデル（最小構成）

| ロール | 権限 |
|---|---|
| `owner` | テナント管理（メンバー招待・車両登録・削除） |
| `viewer` | 自テナント車両の閲覧のみ |
| `admin`（システム） | テナント作成・課金管理（SaaS運営者） |

個人プロジェクトの間は `default` テナントに owner=自分 のみ。

## 4. バックエンド設計（Phase 2 前倒し）

### 4.1 構成

```
Mosquitto ◀──購読── ingest (Node.js) ──書込──▶ PostgreSQL
                                                   ▲
ブラウザ ──HTTPS──▶ API (Node.js / Fastify) ───────┘
                      │ JWT発行 / テナント・車両・メンバー管理
                      └─認証時の照会──▶ Mosquitto-go-auth
```

- **API と ingest を同一プロセスにしない**（ingest は高頻度書込、API は低頻度。障害・スケールが異なる）
- 技術: Node.js + Fastify + Prisma（ファーム/フロントと言語を統一）

### 4.2 DB スキーマ（主要テーブル）

```sql
tenants        (id uuid PK, name, status text DEFAULT 'active',   -- active | suspended (admin-design A-1)
                plan text DEFAULT 'free', created_at)
users          (id uuid PK, email UNIQUE, password_hash, disabled bool DEFAULT false, created_at)
memberships    (user_id FK, tenant_id FK, role, PRIMARY KEY(user_id, tenant_id))
devices        (id uuid PK, tenant_id FK, display_name, secret_hash,
                secret_prev_hash,            -- ローテーション用 2本持ち (active/previous)
                secret_rotated_at timestamptz,
                disabled bool DEFAULT false, -- 無効化 (admin/owner 操作)
                last_seen_at timestamptz, firmware_version)
locations      (id bigserial, device_id FK, tenant_id FK,
                lat double, lng double, speed real, bearing real,
                recorded_at timestamptz,
                -- 月次パーティション、PK は (id, recorded_at)
                -- 冪等化: UNIQUE (device_id, recorded_at) ※ingest 再配信の重複排除 (H-4)
                PRIMARY KEY (id, recorded_at))
device_events  (id, device_id FK, level, event_id, message, jsonb, at)
```

- `locations` は月次パーティション＋`(tenant_id, device_id, recorded_at DESC)` インデックス
- `audit_logs`（admin-design §5）: `(tenant_id, at DESC)` インデックス。保持2年、月次パーティションを DROP して削除
- 削除ポリシー: 保持期間（例: 90日）超過パーティションを DROP（SaaS のコスト管理の要）

### 4.3 API エンドポイント（最小セット）

> ベースパス方針: テナント用 API は `/api/tenants/...`、システム管理用は `/api/admin/...`（admin-design.md §4.2）に統一する。

| Method | Path | 用途 |
|---|---|---|
| POST | `/api/auth/login` | ログイン → JWT |
| GET | `/api/tenants` / `POST /api/tenants` | テナント一覧/作成（**自己登録は Phase 3**。Phase 2 では admin が作成） |
| GET/POST | `/api/tenants/{tid}/devices` | 車両（デバイス）登録・一覧。作成時に secret 返却 |
| GET/POST | `/api/tenants/{tid}/members` | メンバー一覧・**招待**（招待は `POST .../members/invite`、admin-design §4.1 と同一形式） |
| GET | `/api/tenants/{tid}/locations?from=&to=&device=` | 履歴取得（再生用） |
| WS | `/ws/live` | （代替経路）ブラウザ向けライブ配信のバックエンド中継。現状は Mosquitto 直接のまま |

### 4.4 ライブ配信の経路判断

| 案 | 評価 |
|---|---|
| A. ブラウザ → Mosquitto 直接（JWT 認証） | ✅ 採用。低レイテンシ・実装済みの mqtt.js を流用 |
| B. ブラウザ → API(WebSocket) → ingest 中継 | Phase 3 候補。課金・監査・QoS 制御を API で一元化したい場合に切替 |

A のままでも JWT は API が発行するため、認可の一元性は保てる。

## 5. デプロイ構成の変化

### 現在（Phase 1）

```
Proxmox Docker: mosquitto ＋（ローカルで python http.server）
default テナントのみ。passwd 手動管理。
```

### Phase 2（マルチテナント基盤）

```
Proxmox Docker:
  caddy        ── TLS終端 (443) : / → web静的, /ws|/mqtt → mosquitto:9001, /api → api:3000
  mosquitto    ── go-auth プラグイン (DB/バックエンド照会)
  api          ── Fastify
  ingest       ── MQTT購読→DB
  postgres     ── データ本体 (volume)
  simulator    ── 開発用
```

- 全コンポーネントを docker-compose（Proxmox 1ホスト）で賄う。テナント数 ≲ 数十〜数百までは物理増設不要
- SaaS 本格化時は PostgreSQL → managed 化、Mosquitto → EMQX/HiveMQ 等の検討（**トピック契約が不変なので移行可能**）

## 6. UI への影響（ui-design 改訂点）

| 変更 | 内容 |
|---|---|
| ログイン画面 | 新規追加（メール/パスワード） |
| テナントスイッチャー | Header 左に現在テナント表示＋切替（所属テナントが複数の場合） |
| 車両ラベル | `car01`（display_name）を UI で使用。内部は UUID |
| サイドパネル | 車両一覧は API (`GET /tenants/{tid}/devices`) 由来に。オフライン車両は last_seen_at でも補正 |
| 車両カード | 履歴再生ボタン追加（Phase 2後半） |

## 7. ファームウェアへの影響

| 項目 | 変更 |
|---|---|
| 設定 | `TENANT_ID`, `DEVICE_ID`, `DEVICE_SECRET` を追加（起動時に Serial/Config で初期設定、NVS 保存） |
| トピック | `carpal/{tenant_id}/{device_id}/location` へ変更 |
| 認証 | MQTT username=device_id, password=device_secret |
| 時刻 | GNSS の UTC で `ts` を実時刻化（履歴保存と突合するため） |

## 8. 移行計画（現状 → マルチテナント対応）

| Step | 作業 | タイミング |
|---|---|---|
| 1 | トピックを `carpal/{tenant}/{device}/...` に変更（ファーム・シミュレータ・フロント） | **今すぐ**（デバイス未着なので再書込コストゼロ） |
| 2 | vehicle_id → UUID + display_name 化（シミュレータ・UI） | Step 1 と同時 |
| 3 | PostgreSQL + API + ingest 追加、Caddy で TLS/WSS | Phase 2-a |
| 4 | mosquitto-go-auth 導入、passwd 運用廃止 | Phase 2-b |
| 5 | ログイン・テナント管理 UI | Phase 2-b |
| 6 | 課金・テナント自己登録（SaaS 化） | Phase 3 |

**Step 1〜2 を今やっておくことが最重要**。デバイス到着後にトピックを変えると実機の再設定が全台必要になる。

## 9. SaaS 化チェックリスト（将来対応）

- [ ] マルチテナント分離テスト（他テナントの topic に publish/subscribe できないことを自動テスト化）
- [ ] レート制限（テナント毎の publish 頻度・location 保持容量）
- [ ] テナント削除時のデータ完全消去フロー（GDPR/個人情報配慮）
- [ ] 位置情報の取り扱い同意・利用規約（車両追跡は法的配慮必須）
- [ ] 監視（テナント別 KPI、ingest 遅延、Mosquitto 接続数）
- [ ] バックアップ/リストア手順（PostgreSQL 定期バックアップ）

## 10. 実装タスク（追加）

- [ ] トピック新形式への変更（simulator / mqtt.js / firmware / index.html）
- [ ] `default` テナントの定数化とドキュメント更新（design.md 改訂）
- [ ] Phase 2-a: docker-compose に postgres/api/ingest/caddy を追加
- [ ] DB スキーママイグレーション（Prisma migrate）初版作成
- [ ] テナント分離の結合テスト（ACL 検証スクリプト）
