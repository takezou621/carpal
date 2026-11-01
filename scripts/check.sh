#!/usr/bin/env bash
# scripts/check.sh — CI と同じ検査をローカルで実行する
# 使い方: ./scripts/check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== 1. Node.js 構文検査 =="
node --check web/simulator/simulator.js
node --check web/mqtt.js
echo "OK"

echo "== 2. docker compose 設定検証 =="
docker compose -f docker/mosquitto/docker-compose.yml config -q
echo "OK"

echo "== 3. Mosquitto conf 起動試験 =="
# passwd はリポジトリに含めないため、試験用にダミーを一時生成 (監査 N-3 対策)
TMPDIR_MQ=$(mktemp -d)
cp docker/mosquitto/mosquitto.conf "$TMPDIR_MQ/mosquitto.conf"
cp docker/mosquitto/acl_file "$TMPDIR_MQ/acl_file"
touch "$TMPDIR_MQ/passwd" && chmod 600 "$TMPDIR_MQ/passwd"
docker run --rm -v "$TMPDIR_MQ":/mosquitto/config:ro \
  eclipse-mosquitto:2 mosquitto -c /mosquitto/config/mosquitto.conf -v &
PID=$!
sleep 3
if kill -0 "$PID" 2>/dev/null; then
  kill "$PID" 2>/dev/null || true
  echo "OK (設定は正常にロードされました)"
else
  echo "NG: mosquitto が設定を読み込めませんでした" >&2
  rm -rf "$TMPDIR_MQ"
  exit 1
fi
rm -rf "$TMPDIR_MQ"

echo ""
echo "All checks passed."
