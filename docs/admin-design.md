# carpal Admin（管理機能）要件・仕様書

対応: `docs/multi-tenant-design.md` のロールモデルを詳細化する。

## 1. 用語の整理（重要）

「Admin」を混同しないこと。2種類の管理者が存在する。

| 種別 | 名称 | スコープ | 主な利用者 |
|---|---|---|---|
| **システム管理者** | `admin`（システム） | 全テナント・全データ | SaaS 運営者（現状: 開発者本人） |
| **テナント管理者** | `owner` | 自テナントのみ | 事業所の管理者（将来の顧客） |

本書は両方を扱う。**画面は `/admin`（システム管理コンソール）とテナント設定画面に分離**し、URL・API・JWTクレームで完全に区切る。

## 2. 要件

### 2.1 システム管理者（`admin`）の要件

| ID | 要件 | 優先度 |
|---|---|---|
| A-1 | 全テナントの一覧・検索・作成・停止（suspend）ができる | Must |
| A-2 | テナントをまたいだ車両（デバイス）一覧・状態確認ができる | Must |
| A-3 | 全ユーザーの一覧・照会・無効化ができる（パスワード変更は本人/ownerに委譲） | Must |
| A-4 | システム監視: Mosquitto 接続数、ingest 遅延、DB 容量、API レイテンシが見える | Must |
| A-5 | 監査ログ: テナント作成/停止、ユーザー無効化、デバイス削除などの管理操作を記録・照会できる | Must |
| A-6 | テナント別の利用量（デバイス数、location 保存件数/容量）を確認できる | Must（SaaS課金の基礎） |
| A-7 | テナントのプラン/上限（デバイス数・保持期間）を変更できる | Should |
| A-8 | 障害時の機能: 特定デバイスの強制切断（MQTT kick）、位置データの手動削除 | Should |
| A-9 | 課金（プラン変更・請求） | Won't（Phase 3、Stripe 検討） |

### 2.2 テナント管理者（`owner`）の要件

| ID | 要件 | 優先度 |
|---|---|---|
| T-1 | 自テナントの車両（デバイス）登録・表示名変更・削除。**登録時にデバイスシークレットを1回だけ発行** | Must |
| T-2 | メンバー（users）の招待・ロール付与（owner/viewer）・除去 | Must |
| T-3 | 自テナントの利用量確認（自テナントのみ） | Should |
| T-4 | 自テナントの監査ログ照会（自テナントの操作のみ） | Should |

## 3. 権限マトリクス

| 操作 | viewer | owner | admin(システム) |
|---|---|---|---|
| 車両位置の閲覧（自テナント） | ✅ | ✅ | ✅ |
| 履歴の閲覧（自テナント） | ✅ | ✅ | ✅ |
| デバイス登録/削除 | ❌ | ✅ | ✅ |
| メンバー管理 | ❌ | ✅ | ✅ |
| 自テナント利用量/監査ログ | ❌ | ✅ | ✅ |
| 全テナント一覧・suspend | ❌ | ❌ | ✅ |
| 全ユーザー照会・無効化 | ❌ | ❌ | ✅ |
| 利用量（全テナント）・課金 | ❌ | ❌ | ✅ |
| システム監視メトリクス | ❌ | ❌ | ✅ |

判定は**すべてサーバ側（API + MQTT ACL）で行う**。UI の表示制御は UX 上のものに過ぎない。

## 4. API 仕様

認証: すべて `Authorization: Bearer <JWT>`。JWT には `tid`(tenant), `role`, `sys`(システム管理者フラグ) を含む。

### 4.1 テナント管理者向け（owner・`/api` 配下）

```
POST   /api/tenants/{tid}/devices              デバイス登録 → { id, secret } (secret はこの1回のみ返す)
PATCH  /api/tenants/{tid}/devices/{id}         表示名変更・停止
DELETE /api/tenants/{tid}/devices/{id}         削除（location も論理削除）
GET    /api/tenants/{tid}/members
POST   /api/tenants/{tid}/members/invite       招待（メール＋ロール）
DELETE /api/tenants/{tid}/members/{user_id}
GET    /api/tenants/{tid}/usage                { devices: n, locations: n, storage_mb: x }
GET    /api/tenants/{tid}/audit_logs?from=&to=
```

### 4.2 システム管理者向け（admin・`/api/admin` 配下）

```
GET    /api/admin/tenants?q=&status=       全テナント一覧・検索
POST   /api/admin/tenants                  テナント作成
POST   /api/admin/tenants/{tid}/suspend    停止/再開（MQTT ACL と API 両方で拒否される）
GET    /api/admin/users?q=                 全ユーザー照会
POST   /api/admin/users/{uid}/disable      無効化（全テナントでログイン不可）
GET    /api/admin/devices?tid=&online=     全デバイス一覧（最終接続時刻付き）
GET    /api/admin/usage                    テナント別利用量一覧
GET    /api/admin/audit_logs               全テナント監査ログ照会
GET    /api/admin/metrics                  システムメトリクス（Prometheus形式も出力）
POST   /api/admin/devices/{id}/kick        MQTT 強制切断（$SYS 制御 or broker API）
```

## 5. 監査ログ仕様

- 保存先: PostgreSQL `audit_logs` テーブル（`logging-design.md` の `device_events` とは別管理）
- 変更不可: UPDATE/DELETE を行わない（論理的イミュータブル。アプリ側で禁止）

```sql
audit_logs (
  id          bigserial PK,
  actor_id    uuid,          -- 実行者 (ユーザー or 'system')
  actor_role  text,          -- owner / admin
  tenant_id   uuid,          -- 対象テナント
  action      text,          -- 'device.create', 'member.invite', 'tenant.suspend', ...
  target_type text,          -- 'device' / 'user' / 'tenant'
  target_id   text,
  detail      jsonb,         -- 変更前後値など
  ip          inet,
  at          timestamptz
)
```

- 記録対象: 権限マトリクスの「閲覧以外」の操作すべて＋ログイン成功/失敗
- 保持: 2年（法令上の理由で調整）

## 6. システム監視（A-4）の設計

| 指標 | 取得元 | 閾値アラート例 |
|---|---|---|
| MQTT 接続数 | Mosquitto `$SYS/broker/clients/connected` | > 設定上限の80% |
| ingest 遅延 | メッセージ `ts` と処理時刻の差の平均 | > 10秒 |
| location 書込レート | ingest 内部カウンタ | 急増（異常検知） |
| DB 容量/テーブルサイズ | `pg_total_relation_size` | ディスク80% |
| API レイテンシ | Fastify フック | p95 > 1秒 |
| デバイス在線率 | devices.last_seen_at から集計 | テナント毎の在線率 < 80% |

- 現状: `/api/admin/metrics` の JSON で最小限提供
- 将来: Prometheus + Grafana（Proxmox の別コンテナ）へエクスポート

## 7. UI 設計（Admin コンソール `/admin`）

### 画面一覧

```
/admin                    ダッシュボード (テナント数・デバイス数・在線率・ingest遅延)
/admin/tenants            テナント一覧 (検索・status絞込)
/admin/tenants/:id        テナント詳細 (メンバー・デバイス・利用量・suspend操作)
/admin/users              ユーザー一覧 (無効化)
/admin/devices            全デバイス一覧 (最終接続・オンライン状態)
/admin/audit-logs         監査ログ (検索・CSV DL)
/admin/metrics            システム監視
```

### テナント管理者向け（既存アプリ内）

```
/settings                 テナント設定 (表示名)
/settings/devices         車両管理 (登録時にシークレット表示モーダル)
/settings/members         メンバー管理
/settings/usage           利用状況
```

### デザイン方針

- 車両マップUI とは**完全に別画面**（影響を分離・見た目は機能重視のシンプルなテーブル中心）
- シークレットは作成時1回のみ表示（再表示不可。紛失時は再発行）—— 仕様として明文化
- 破壊的操作（suspend・削除・無効化）は確認ダイアログ＋監査ログ記録

## 8. セキュリティ要件

1. **`/admin` は JWT の `sys: true` クレームが必須**。owner が URL 直叩きしても API が 403 を返す
2. システム管理者アカウントは通常の招待フローでは作れない（DB 初期化時に seed 作成）
3. 2FA: システム管理者には必須化（Phase 2-b）、テナントownerには任意
4. 監査ログへの書込は API プロセスのみ（DB ユーザーの権限で UPDATE/DELETE を禁止）
5. admin API は全リクエストを監査ログに記録

## 9. 実装フェーズ

| Phase | 内容 | 要件ID |
|---|---|---|
| 2-a | devices API（シークレット発行）＋ テナント分離テスト | T-1 |
| 2-b | members/invite、`/settings` 画面群 | T-1, T-2 |
| 2-c | audit_logs、usage 照会 | T-3, T-4, A-5, A-6 |
| 2-d | `/admin` コンソール（tenants/users/devices/audit） | A-1〜A-3, A-6 |
| 2-e | metrics・MQTT kick | A-4, A-8 |
| 3 | プラン/上限・課金 | A-7, A-9 |

個人プロジェクトの間は「admin=自分」であるため、**2-a（デバイス登録API）が実質的に必須の最初の管理機能**。それ以前は DB 直接操作で代替する。
