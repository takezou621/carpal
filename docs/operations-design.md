# carpal 運用設計書（障害時挙動・バックアップ・監視通知）

監査指摘 M-7 / M-8 に対応。CI/CD 設計書と合わせて運用面を扱う。

## 1. 障害シナリオ別の挙動設計

### 1.1 Mosquitto 再起動・クラッシュ

| 対象 | 挙動 | 設計上の対処 |
|---|---|---|
| デバイス | TCP切断 → 再接続 | **指数バックオフ必須**: 1s, 2s, 4s ... 最大60s（全台が同時に再接続するサンダリングハードを防ぐ。ファームに jitter 付き backoff を実装する） |
| ブラウザ | mqtt.js が3秒固定で再接続 | 接続数が少ないため許容。SaaS化時に backoff 化 |
| ingest | persistent session (QoS1) により再接続後、未取得メッセージを再配信 | 重複は `UNIQUE(device_id, recorded_at)` Upsert で冪等化（contract.md §4）。**CONNACK 拒否時（webuser 未プロビジョニングの起動直後など）は mqtt.js が自動再接続しないため、ingest 側で 5秒間隔の明示再接続を実装済み（`ingest.mjs`）** |
| データ | Retain（status）と persistent session は volume で永続化 | volume 消失時は「全車両一時オフライン表示」で自己修復（location 再受信で復帰） |

### 1.2 PostgreSQL 障害

| 状況 | ingest の挙動 |
|---|---|
| 短時間断（< 5分） | 書込をメモリキューにバッファ（上限1万件）し、復帰後フラッシュ |
| 長時間断 | バッファ満杯後は**location 受信を破棄し続ける**（WARN ログ `INGEST_BUFFER_FULL`）。ライブ表示は Mosquitto 直結のため影響なし＝履歴のみ欠損 |
| 再起動後 | キューを時刻順にフラッシュ（recorded_at で並べ替え済みのため順序逆転なし） |

### 1.3 API 障害

- ブラウザのライブ表示は Mosquitto 直接接続のため**影響を受けない**（A案経路の利点）
- 影響: ログイン不可・JWT 更新不可。既存 JWT は TTL まで有効 → 最大15分の猶予
- 対処: docker `restart: unless-stopped` + ヘルスチェックで自動復旧。復旧しない場合は監視アラート（§3）

### 1.4 自宅回線・電源断（単一ホスト障害）

- 現状: 全機能停止。デバイスは publish 失敗を繰り返す（QoS0 なので欠落、復帰後の location で自動回復）
- Phase 3（有償化）: 退避先として VPS 上にスタンバイ Mosquitto+Postgres を用意し、DDNS 切替（目標 RTO 1時間 / RPO 24時間）

### 1.5 目標復旧値（現状構成）

| 項目 | RTO | RPO |
|---|---|---|
| Mosquitto | 自動（数秒） | Retain/session は volume 永続 → 0 |
| PostgreSQL | 手動含め 1時間 | バックアップ次第（§2: 最大24時間） |
| Web 静的 | 再デプロイ 15分 | 0（リポジトリが正） |

## 2. バックアップ・リストア

### 2.1 対象と頻度

| 対象 | 方法 | 頻度 | 保持 |
|---|---|---|---|
| PostgreSQL | `pg_dump -Fc`（locations は月次パーティション単位の `pg_dump -t` でも可） | 毎日 03:00（cron） | 14日分 + 月次1本を3ヶ月 |
| Mosquitto volume | volume スナップショット | 週次 | 4世代 |
| リポジトリ / 設定 | GitHub（+リモートバックアップ） | push 毎 | 無期限 |
| デバイス設定（TENANT_ID 等） | NVS に保存。**クラウド側に平文保存しない**（シークレットはhashのみ） | — | — |

### 2.2 リストア手順（ドラフト）

```bash
# 1. postgres コンテナ停止
docker compose stop postgres
# 2. ボリューム再作成後、リストア
cat dump-YYYYMMDD.dump | docker compose exec -T postgres pg_restore -U carpal -d carpal --clean --if-exists
# 3. 起動 & 検証
docker compose up -d
curl -f http://localhost:3000/api/health   # API ヘルスチェック
mosquitto_sub -t 'carpal/#' -v -C 3        # メッセージ流通確認
```

- **リストア訓練を月1回実施**（バックアップは復旧できるまで意味がない）。Phase 2-a の完了条件に含める

## 3. 監視アラートと通知先

| 指標（admin-design §6 と同じ） | 閾値 | 通知先 |
|---|---|---|
| Mosquitto 接続数 | 上限の80% | ntfy.sh / LINE Notify（個人運用は即時性重視の push 通知） |
| ingest 遅延 | > 10秒 | 同上 |
| DB ディスク | > 80% | 同上＋メール |
| バックアップ失敗 | 1回でも失敗 | 同上（最優先。無通知=成功と見なさない。成功時も日次で「OK」を送るハートビート方式） |
| API p95 レイテンシ | > 1秒 | 日次サマリで可 |

- 通知チャネルの設計原則: **Critical は即時 push、Warning は日次サマリに集約**（アラート疲れの防止）
- Phase 3: Prometheus + Grafana + Alertmanager に移行し、テナント別ダッシュボードも提供

## 4. 可用性の段階設計

| 段階 | 構成 | 想定稼働率 |
|---|---|---|
| 現状 (Phase 1-2) | 自宅 Proxmox 1ホスト | ベストエフォート |
| Phase 3 初期 | Proxmox + VPS バックアップ受入れ（Mosquitto bridge / pg_replication の準備のみ） | 99%（目安） |
| SaaS 本格化 | VPS 2系統（active/standby）＋マネージドDB検討 | 99.5% |

## 5. メンテナンス運用

- 計画メンテナンス: テナント管理者へ事前通知（72時間前・基本深夜帯）
- Mosquitto のバージョンアップ: ステージング（同構成の別 compose project）で先に起動試験 → 本番
- ファーム更新: OTA は停車車両のみ段階配信（1台→24時間観察→残台数）。cicd-design.md §6 参照

### 5.1 MQTT TLS 公開 (mqtt.takezou.com:62913) — 2026-11-09 実装済み

- **DNS**: `mqtt.takezou.com` A レコード（DNS-only）= 自宅グローバル IPv4。PVE の
  `cloudflare-ddns.sh` (systemd timer `cloudflare-ddns.timer`) が `mail` と同一値を追跡
- **証明書**: Let's Encrypt DNS-01 (Cloudflare API トークン)。LXC carpal-backend の
  `carpal-cert-renew.timer` が日次で `certbot renew` を実行し、証明書が変わったときのみ
  mosquitto を再起動 (`/opt/carpal/certs/renew.sh`)
- **リスナー**: mosquitto `conf.d/tls-8883.conf` (compose override で注入、リポジトリ外)。
  ベンチは平文 1883 のみ (`docker/mosquitto/conf.d` は空)
- **ポート転送 (GL.iNet GL-BE9300 / OpenWrt)**: MAP-E 割当ポートは **5568-5583,
  9664-9679, ... 62912-62927** (16ポート × A=1..15 ブロック, PSID 92 / offset 4)。
  範囲内の **外部 62913 → 192.168.2.131:8883** を DNAT (`firewall.carpal-mqtt`)。
  8883 のままだと範囲外のため ISP の BR で破棄される (mail 25/465/587 が外部から
  不通な理由と同一)
- **MTU**: MAP-E 実効 MTU 1420 未満が必要なため、LXC 131 の eth0 / docker を
  **MTU 1400** に統一 (`/etc/network/interfaces`, `/etc/docker/daemon.json`)
- **ファーム**: WiFi + TLS は `WiFiClientSecure` + ISRG Root X1 検証
  (`isrgrootx1.h`)。LTE (TinyGSM) は TLS 未対応のため **Soracom Beam に TLS を肩代わり**
  (§5.2)。Beam 非依存の TLS はモデム内蔵 MQTT AT (AT+CMQTT*, SSL対応) への移行が
  Phase 2-e の残作業

### 5.2 SIM / SORACOM 運用 (plan-D + Beam) — 2026-11-10 整備

- **SIM**: Soracom plan-D (D-500MB / ドコモ網)。IMSI `44010…`、対応通信方式は
  **4G / LTE Cat.M1** (公式サブスクリプション一覧) → SIM7080G (Cat-M1 専用モジュール)
  で利用可能。NB-IoT は非対応 (§5.2 切り分け手順 3 参照)
- **APN**: SORACOM 公式設定 — APN `soracom.io` / ユーザー名 `sora` / パスワード `sora`
  (認証タイプ CHAP or PAP)
- **セッション**: plan-D の無通信タイマーは 1 時間。MQTT keepalive (PubSubClient 既定
  15秒) の traffic で常時維持される
- **Beam (TLS 肩代わり)**: SIM のグループに **MQTT エントリポイント**を追加する
  - エントリポイント URL: `beam.soracom.io:1883` (デバイスはここに平文接続)
  - 転送先: **Standard / MQTTS** / `mqtt.takezou.com` / `62913`
  - 認証情報: **パススルー ON** (デバイスの MQTT ユーザー car01 を dynsec へそのまま転送)
  - プラットフォームバージョン: **201912** (LWT 利用可。keepalive は 5〜1200 秒必須、
    0 または範囲外だと NOT_AUTHORIZED で拒否される)
  - QoS 1 (status) も 201912 では透過転送される
- **課金**: Beam MQTT は往復で **2 リクエスト/メッセージ**課金 (PINGREQ は対象外)。
  送信ポリシー (移動中 3 秒 / 停車 30 秒) がそのまま Beam リクエスト数の制御になる
- **切り分け手順** (LTE 接続不良時):
  1. Soracom コンソールで SIM ステータス確認 (`ready`=登録のみ / `active`=セッション確立)
  2. モデム AT: `AT+CGREG?` / `AT+CEREG?` (網登録)、`AT+CGATT?` (PS アタッチ)。
     `AT+CSQ` が `99,99` のまま、`AT+COPS=?` の全セルスキャンが数分後に ERROR で
     戻る場合は**セル未捕捉** — 設定ではなく LTE アンテナ (u.FL) の外れ・設置場所を疑う
     (2026-11-10 実機で確認)。なお `AT+CNMP` / `AT+CMNB` は NVRAM に persist するため、
     診断後は目的のモードへ明示設定して戻すこと
  3. Cat-M1 固定: `AT+CNMP=38` (LTE) + `AT+CMNB=1` (Cat-M1)。NB-IoT (`AT+CMNB=2`) は
     plan-D 非対応
  4. 到達確認: `ping pong.soracom.io` — 100.127.100.127 から応答があれば Soracom 網まで生存
  5. Beam 設定の転送先ホスト/ポート確認と、ブローカー側 8883 (Let's Encrypt) の証明書有効性確認

- **既知事象 (2026-11-10, T-SIM7080G / モデム FW R1951.07)**: GNSS を有効化
  (`AT+CGNSPWR=1`) すると、**GPS 捕捉中 (acquisition) に LTE の TCP が確立後
  約45〜47 秒で切断**される。GNSS ポーリング頻縞・MQTT 経路 (Beam) 非依存で、
  無通信でも発生 (アブレーション実験で切り分け済み)。屋外で GPS fix が確定すれば
  捕捉が終わって安定する見込み (実車確認待ち)。ファームは
  **PDP リフレッシュ → モデム電源リサイクルの 2 段階自己復帰**でこれを兜背する。
  恒久対策としてモデムファーム更新を検討 (§6)

## 6. 実装タスク

- [ ] ファーム: 再接続の指数バックオフ実装（jitter 付き）
- [ ] Phase 2-e: 実車フィールドテスト (LTE→Beam→Mosquitto→地図表示 E2E)
- [ ] Phase 2-e: モデム内蔵 MQTT AT (AT+CMQTT*, TLS) への移行で Beam 非依存化
- [ ] Phase 2-e: SIM7080G モデムファーム更新の検証 (GNSS+TCP 共存問題, R1951.07)
- [ ] Phase 2-a: postgres バックアップ cron コンテナ追加 + ハートビート通知
- [ ] Phase 2-a: `/api/health` エンドポイント
- [ ] Phase 2-b: ingest のメモリバッファ実装
- [ ] 月次: リストア訓練（手順書は本書 §2.2 を使用）
