# carpal バックエンド詳細設計書（Phase 2 実装仕様）

対応: `docs/multi-tenant-design.md` §4（構成・DB）, `docs/admin-design.md` §4（API）, `docs/contract.md`（契約）
本書は Phase 2 の実装に直接使えるレベルの詳細を定義する。

> **実装状況 (2026-11-06)**: §2 (ingest) と履歴APIの最小版を実装済み
> (`backend/ingest/ingest.mjs` + `docker/backend/docker-compose.yml`)。
> locations (位置履歴) と device_events (ログ・実装済み: logトピック購読→保存、`GET /api/logs` で照会可)。
> **意図的なMVP遂脱**: 月次パーティション → 単一テーブル+インデックス (データ増大時に移行)、
> JWT は **RS256 実装済み** (鍵ペアはDB永続化)、リフレッシュトークンはJSON応答 (本番はhttpOnly cookie+HTTPS)、
> メンバーAPI実装済み (inviteメール送信のみ未実装 — 初期パスワード1回表示方式)、**MQTT動的認証は go-auth ではなく dynsec+mosquitto_ctrl 方式で実装済み**、履歴APIは CORS wildcard (LANベンチ限定)。
> ログインレート制限はベンチ用に30/min (本番は5/minへ締める)。
> ingest の DB障害時メモリバッファ (§1.2) も未実装 (書込失敗時はログ+欠落)。

## 1. プロセス構成

```
docker-compose (Proxmox 1ホスト)
├── caddy      :443  TLS終端。/ → web静的, /api → api:3000, /mqtt → mosquitto:9001 (ws)
├── mosquitto  :1883 / :9001 (acl_file)
├── api        Fastify (Node 22, ESM)。テナント管理・JWT発行・履歴照会
├── ingest     mqtt クライアント。carpal/+/+/location を購読 → PostgreSQL
├── postgres   16 (volume, 月次パーティション)
├── web        静的ファイル (caddy から配信)
└── simulator  開発用
```

- **api と ingest は別プロセス**（障害・スケール特性が異なるため。multi-tenant §4.1）
- 共通コード（契約バリデーション・DB接続）は `packages/shared/` に置く（将来的に pnpm workspace 化）

## 2. ingest 設計

### 2.1 購読と処理フロー

```
MQTT (QoS1, cleanSession=false)
  → JSON パース
  → 契約バリデーション (contract.md §2: id≡topic, 範囲, ts)
  → recorded_at 決定 (ts があれば it, 無ければ受信時刻)
  → メモリキュー (上限1万件 / operations-design §1.2)
  → バッチ書込 (100件 or 500ms ごと, UPSERT)
```

### 2.2 Upsert（冪等化）

```sql
INSERT INTO locations (device_id, tenant_id, lat, lng, speed, bearing, recorded_at)
VALUES ($1,$2,$3,$4,$5,$6,$7)
ON CONFLICT (device_id, recorded_at) DO UPDATE
  SET lat=EXCLUDED.lat, lng=EXCLUDED.lng, speed=EXCLUDED.speed, bearing=EXCLUDED.bearing;
```

- `ts` 無しで受信時刻を使うメッセージは重複し得るが、UPSERT で上書きされるため実害なし
- バリデーション失敗: `device_events` に `level=WARN, event_id=MSG_INVALID` を記録し破棄

### 2.3 デバイス在線の記録

- status 購読も行い、`devices.last_seen_at` と location 受信のたびに更新（60秒以上経過時のみ UPDATE で書込削減）

## 3. API 設計（Fastify）

### 3.1 共通仕様

| 項目 | 仕様 |
|---|---|
| 認証 | JWT (RS256)。`Authorization: Bearer`。クレーム: `sub`(user_id), `tid`, `role`, `sys` |
| エラー形式 | `{ "error": { "code": "TENANT_NOT_FOUND", "message": "..." } }` + 適切な HTTP status |
| バリデーション | Fastify JSON Schema（リクエスト定義をコード＝仕様として使う） |
| レート制限 | `@fastify/rate-limit`: ログイン 5回/分/IP、API 100回/分/user |
| 監査 | admin-design §5 の `audit_logs` に書込（read 以外の全操作） |

### 3.2 主要エンドポイント実装メモ

| Endpoint | 動作メモ |
|---|---|
| `POST /api/auth/login` | email+password → アクセストークン(15分) + リフレッシュ(httpOnly cookie, 7日)。失敗も監査記録 |
| `POST /api/auth/refresh` | リフレッシュトークン回転（使用済み検知で全無効化） |
| `POST /api/tenants/{tid}/devices` | owner権限必須。`consent_confirmed=true` 必須（legal-compliance §2.2）。secret は32バイトランダム→**平文はこの応答1回のみ**、DB は argon2id hash |
| `POST /api/tenants/{tid}/devices/{id}/rotate` | secret 再発行（previous を2本持ち期間24h）。監査記録 |
| `GET /api/tenants/{tid}/locations` | `from/to` 必須（最大7日）。`LIMIT 10000` 上限。CSV/JSON 切替 |
| `GET /api/tenants/{tid}/usage` | devices COUNT, locations COUNT(当月パーティション), pg_total_relation_size |

### 3.3 go-auth 連携（Phase 2-c）

- `mosquitto-go-auth` の HTTP モードで API 側に内部エンドポイントを追加:
  - `POST /internal/mqtt/auth`: username=device_id → devices 照合（active/previous 両ハッシュ試行）
  - `POST /internal/mqtt/acl`: JWT 解析 → tid/role から許可トピック判定（multi-tenant §3.2 の ACL ルール）
- 内部エンドポイントは Docker ネットワーク内のみリッスン

## 4. JWT 詳細（multi-tenant §3.4 の実装仕様）

```json
{
  "sub": "uuid-user", "tid": "uuid-tenant", "role": "owner",
  "sys": false,                       // システム管理者フラグ
  "ver": 3,                           // トークンバージョン (パスワード変更時に users.token_ver++ → 旧JWT失効)
  "iat": 1730460000, "exp": 1730460900
}
```

- 鍵: RS256、秘密鍵は Secrets 管理、公開鍵は go-auth に配布
- 失効は DB 照合（users.disabled / tenants.status / token_ver）で即時反映

## 5. テスト設計

| テスト | 内容 | ツール |
|---|---|---|
| 契約テスト | contract.md のスキーマに対する fixture 送受信（正常/異常系） | Vitest |
| テナント分離 | テナントAのJWTでテナントBの topic/API にアクセス→必ず 403/deny | Vitest + 実Mosquitto (testcontainers) |
| ingest 冪等性 | 同一メッセージ2回送信 → locations 1件 | Vitest + 実Postgres |
| E2E | simulator 起動 → API で履歴取得まで | Playwright/API テスト |

**テナント分離テスト（multi-tenant §9 の最重要項目）は CI で必ず実行**（cicd-design.md の test ジョブに組込）。

## 6. 実装タスク（Phase 2-a〜2-c の順）

- [ ] `packages/shared/`: 契約バリデータ（JSON Schema 生成）
- [ ] postgres 初期スキーマ（Prisma migrate または raw SQL マイグレーション）
- [ ] ingest 実装（購読・バリデーション・バッチUpsert・メモリキュー）
- [ ] api 実装（auth → devices → locations → usage の順）
- [ ] caddy 追加（TLS 終端・ルーティング）
- [ ] go-auth 導入（Phase 2-c）＋テナント分離テストのCI組込
