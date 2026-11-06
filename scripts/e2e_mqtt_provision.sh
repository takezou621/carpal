#!/usr/bin/env bash
# scripts/e2e_mqtt_provision.sh — dynsec プロビジョニング E2E
# API でデバイス登録 → 発行された secret で MQTT 接続可能/権限拒否/suspend を検証
# 前提: mosquitto (dynsec) + postgres + ingest + api が稼働、mosquitto_pub/sub が使えること
set -euo pipefail
# 認証情報は gitignore済み .env から取得 (ハードコード禁止)
source "$(dirname "$0")/../docker/backend/.env"
BASE=${BASE:-http://127.0.0.1:3001}
PASS=0; FAIL=0
check() { if [ "$2" = "$3" ]; then PASS=$((PASS+1)); echo "  OK  $1"; else FAIL=$((FAIL+1)); echo "  NG  $1 (expected=$2 actual=$3)"; fi; }
code() { curl -s -o /tmp/mqtt_e2e_body.json -w "%{http_code}" "$@"; }
jqget() { python3 -c "import json; d=json.load(open('/tmp/mqtt_e2e_body.json')); print(eval(\"d$1\"))"; }

echo "== 1) ログイン =="
check login 200 "$(code -X POST $BASE/api/auth/login -H 'Content-Type: application/json' -d '{"email":"admin@carpal.local","password":"$ADMIN_PASSWORD"}')"
TOKEN=$(jqget "['access_token']")

echo "== 2) APIでデバイス登録 =="
check device_create 201 "$(code -X POST $BASE/api/tenants/$(curl -s $BASE/api/admin/tenants -H "Authorization: Bearer $TOKEN" | python3 -c "import json,sys; print(json.load(sys.stdin)['tenants'][0]['id'])")/devices -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{"display_name":"mqtt-e2e","consent_confirmed":true}')"
DID=$(jqget "['device']['id']")
SECRET=$(jqget "['secret']")
TENANT=$(curl -s $BASE/api/admin/tenants -H "Authorization: Bearer $TOKEN" | python3 -c "import json,sys; print(json.load(sys.stdin)['tenants'][0]['id'])")

echo "== 3) 発行された secret で MQTT publish できる =="
MOSQ="docker run --rm --network host eclipse-mosquitto:2"
OUT=$($MOSQ mosquitto_pub -h localhost -u "$DID" -P "$SECRET" -t "carpal/$TENANT/$DID/location" -m '{"id":"'$DID'","lat":36.13,"lng":139.39}' -q 1 2>&1 || true)
if echo "$OUT" | grep -qiE "not autho"; then
  FAIL=$((FAIL+1)); echo "  NG  publish denied (provisioning missing?)"
else
  PASS=$((PASS+1)); echo "  OK  publish allowed"
fi

echo "== 4) 他デバイスのトピックへの publish は拒否 =="
OUT=$($MOSQ mosquitto_pub -h localhost -u "$DID" -P "$SECRET" -t "carpal/$TENANT/other-device/location" -m '{"bad":1}' -q 1 -V mqttv5 2>&1 || true)
echo "$OUT" > /tmp/cross_out.txt
if echo "$OUT" | grep -qiE "not autho"; then
  PASS=$((PASS+1)); echo "  OK  cross-topic publish denied"
else
  FAIL=$((FAIL+1)); echo "  NG  cross-topic publish allowed!"
fi

echo "== 5) subscribe は拒否 (デバイスは publish のみ) =="
OUT=$(timeout 8 $MOSQ mosquitto_sub -h localhost -u "$DID" -P "$SECRET" -t "carpal/$TENANT/$DID/location" -C 1 -W 6000 2>&1 || true)
if echo "$OUT" | grep -q "denied"; then
  PASS=$((PASS+1)); echo "  OK  subscribe denied"
else
  FAIL=$((FAIL+1)); echo "  NG  subscribe allowed!"
fi

echo "== 6) suspend で MQTT クライアント無効化 =="
check suspend 200 "$(code -X POST $BASE/api/admin/tenants/$TENANT/suspend -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{}')"
sleep 2
OUT=$($MOSQ mosquitto_pub -h localhost -u "$DID" -P "$SECRET" -t "carpal/$TENANT/$DID/location" -m '{"x":1}' -q 1 2>&1 || true)
if echo "$OUT" | grep -qiE "not autho"; then
  PASS=$((PASS+1)); echo "  OK  suspended publish denied"
else
  FAIL=$((FAIL+1)); echo "  NG  suspended device can publish"
fi

echo "== 7) resume で復帰 =="
check resume 200 "$(code -X POST $BASE/api/admin/tenants/$TENANT/suspend -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{"resume":true}')"
sleep 2
OUT=$($MOSQ mosquitto_pub -h localhost -u "$DID" -P "$SECRET" -t "carpal/$TENANT/$DID/location" -m '{"ok":1}' -q 1 2>&1 || true)
if echo "$OUT" | grep -qiE "not autho"; then
  FAIL=$((FAIL+1)); echo "  NG  publish still denied after resume"
else
  PASS=$((PASS+1)); echo "  OK  publish allowed after resume"
fi

echo ""
echo "RESULT: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && echo "ALL GREEN" || exit 1
