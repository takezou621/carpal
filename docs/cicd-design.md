# carpal CI/CD 仕様書

対応: `docs/design.md` / `docs/logging-design.md`

## 1. 方針

- **現状はローカル Git リポジトリ**のため、まずは「ローカルで回す検証スクリプト」を整備し、GitHub にホストした時点で GitHub Actions に移行する二段構え
- **自動デプロイの対象は限定**: Mosquitto 設定（Proxmox）と Web 静的ファイル。ファームの書き込みは物理接続が要るため自動化しない（OTA は Phase 3）
- すべての PR / push で「壊れていないこと」を機械的に保証する

## 2. パイプライン全体像

```
push / PR
   │
   ├─ [lint]        構文・フォーマット検査（高速・毎回）
   ├─ [test]        シミュレータ・フロントのユニットテスト
   ├─ [fw-build]    ファームウェアのコンパイル検証 (arduino-cli)
   └─ [deploy]      main マージ時のみ。Proxmox へ設定反映・Web 公開
```

## 3. CI ジョブ仕様

### 3.1 lint（毎回・最速）

| 対象 | ツール | 内容 |
|---|---|---|
| Node.js (simulator) | `node --check` + ESLint | 構文検査 |
| Web (js/html) | ESLint + tidy(任意) | 構文検査 |
| YAML/設定 | `docker compose config` | docker-compose.yml の検証 |
| Mosquitto conf | コンテナで `mosquitto -c ... -v` 起動試験 | 設定ミスの早期発見 |

### 3.2 test（Node.js）

- 対象: `web/simulator/` の移動ロジック（ベアリング・速度計算）→ Vitest
- `web/mqtt.js` は mqtt.js ライブラリをモックしてメッセージハンドリングをテスト
- カバレッジ閾値は設けない（実用コード中心・過剰なカバレッジ競争を避ける）

### 3.3 fw-build（ファームコンパイル検証）

デバイスが手元になくても**コンパイルが通ること**を毎回保証する（デバイス到着後も継続）。

```yaml
# .github/workflows/ci.yml 抜粋（イメージ）
- uses: usimd/arduino-compile-sketch@v1  # または arduino-cli 直使用
  with:
    fqbn: esp32:esp32:esp32s3
    sketch: firmware/carpal_tracker
    libraries: |
      - name: TinyGSM
      - name: PubSubClient
      - name: ArduinoJson
```

- ボード定義は `esp32:esp32` を CLI で追加（`--additional-urls` で Espressif ボードマネージャ指定）
- コンパイルは数分かかるため、`firmware/` と `docs/` 配下のみの変更ではスキップ（paths フィルタ）

### 3.4 ジョブ実行条件

| ジョブ | 実行条件 |
|---|---|
| lint | 全 push/PR |
| test | `web/**` 変更時 |
| fw-build | `firmware/**` 変更時 |
| deploy | main へのマージ時のみ |

## 4. CD 仕様（main マージ時）

### 4.1 Mosquitto 設定のデプロイ

ターゲット: 自宅 Proxmox 上の Docker ホスト。

```
GitHub Actions ──SSH(鍵認証)──▶ Proxmox ホスト
                                └─ docker compose pull/up -d (docker/mosquitto/)
```

- 手段: `appleboy/ssh-action` 等で SSH 実行。秘密鍵は GitHub Secrets に登録
- **設定変更は版管理が正**: `docker/mosquitto/mosquitto.conf` をリポジトリで管理し、デプロイで上書き同期
- `passwd` ファイルは**リポジトリに含めない**（Secrets 化困難なため）。Proxmox 側で手動管理、CI は touch しない

### 4.2 Web 静的ファイルのデプロイ

| 段階 | 手段 |
|---|---|
| 現状 | なし（ローカル `python3 -m http.server` で手動確認） |
| Phase 2-a | Proxmox 上の Caddy/nginx コンテナに `web/` をマウントし、SSH で `rsync` 同期 |
| Phase 2-b | GitHub Pages（静的のみ。この場合 MQTT は WSS 公開 URL 必須・混在コンテンツ注意） |

推奨は **Caddy + rsync**: 自前 TLS（内蔵 ACME）で WSS も同時に解決できるため。

### 4.3 Secrets 一覧（GitHub 側）

| Secret | 用途 |
|---|---|
| `PROXMOX_SSH_KEY` | デプロイ用 SSH 秘密鍵（デプロイ専用ユーザー・権限最小化） |
| `PROXMOX_HOST` / `PROXMOX_USER` | 接続先 |

MQTT パスワード等のアプリ秘密情報はリポジトリ・CI に置かない（Proxmox 側で管理）。

## 5. ローカル検証スクリプト（CI 導入前の現状用）

`scripts/check.sh` — CI と同じ検査をローカルで一回で実行:

```bash
#!/usr/bin/env bash
set -euo pipefail
# 1. Node 構文検査
node --check web/simulator/simulator.js
node --check web/mqtt.js
# 2. compose 設定検証
docker compose -f docker/mosquitto/docker-compose.yml config -q
# 3. Mosquitto conf 起動試験
docker run --rm -v "$PWD/docker/mosquitto":/mosquitto/config:ro \
  eclipse-mosquitto:2 mosquitto -c /mosquitto/config/mosquitto.conf -v &
sleep 2 && kill %1
echo "All checks passed."
```

- コミット前・プッシュ前に実行する運用ルールとする
- （将来）`lefthook` / `husky` の pre-push フックから自動実行

## 6. ファームウェアのリリース方針

| 段階 | 方式 |
|---|---|
| 現在 | Arduino IDE / arduino-cli で USB 書き込み（手動） |
| Phase 3 | **MQTT OTA**: `carpal/{tenant}/{device}/cmd/ota` トピックにファーム URL を配信し、デバイスが HTTP 取得→適用。リリースタグ `fw/vX.Y.Z` をトリガーにビルド成果物（.bin）を GitHub Releases へ添付。**署名検証（ed25519 等）とロールバック（A/B パーティション or 起動回数カウントでの自動ロールバック）を必須要件とする** |

メジャーバージョン互換性のルール: OTA 配信する .bin は `fw/v*` タグからのみ生成し、topic スキーマ変更を含む版は車両全台停車中に適用する運用とする。

## 7. 実装タスク

- [ ] `scripts/check.sh` 作成（CI 導入前のローカル検証）
- [ ] Mosquitto conf に `log_type` 追記（ログ仕様と合わせ）
- [ ] `.github/workflows/ci.yml` 作成（リポジトリを GitHub へ push 後）
- [ ] `.github/workflows/deploy.yml` 作成（Proxmox 側の SSH ユーザー整備後）
- [ ] （Phase 3）OTA ワークフロー＋ファーム側 OTA 実装
