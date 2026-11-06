#!/usr/bin/env bash
# scripts/api_e2e.sh — API の E2E 検証 (正常系・権限拒否・suspend)
# 使い方: ./scripts/api_e2e.sh  (API が http://127.0.0.1:3001 で起動していること)
set -euo pipefail
BASE=${BASE:-http://127.0.0.1:3001}
PASS=0; FAIL=0
check() { # name expected actual
  if [ "$2" = "$3" ]; then PASS=$((PASS+1)); echo "  OK  $1";
  else FAIL=$((FAIL+1)); echo "  NG  $1 (expected=$2 actual=$3)"; fi
}
code() { curl -s -o /tmp/api_e2e_body.json -w "%{http_code}" "$@"; }
jqget() { python3 -c "import json,sys; d=json.load(open('/tmp/api_e2e_body.json')); print(eval(\"d$1\"))"; }

echo "== 1) ヘルスチェック =="
check health 200 "$(code $BASE/api/health)"

echo "== 2) 未認証アクセスは 401 =="
check no_token 401 "$(code $BASE/api/admin/tenants)"

echo "== 3) ログイン (管理者シード) =="
check login 200 "$(code -X POST $BASE/api/auth/login -H 'Content-Type: application/json' \
  -d '{"email":"admin@carpal.local","password":"change-me-now"}')"
TOKEN=$(jqget "['access_token']")
REFRESH=$(jqget "['refresh_token']")
[ -n "$TOKEN" ] && { PASS=$((PASS+1)); echo "  OK  token issued"; } || { FAIL=$((FAIL+1)); echo "  NG  no token"; }

echo "== 4) 誤認証情報は 401 =="
check bad_login 401 "$(code -X POST $BASE/api/auth/login -H 'Content-Type: application/json' \
  -d '{"email":"admin@carpal.local","password":"wrong"}')"

echo "== 5) テナント一覧 (sys) =="
check list_tenants 200 "$(code $BASE/api/admin/tenants -H "Authorization: Bearer $TOKEN")"

echo "== 6) テナント作成 (sys) =="
check create_tenant 201 "$(code -X POST $BASE/api/admin/tenants -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"name":"e2e-test-tenant"}')"
TID=$(jqget "['tenant']['id']")

echo "== 7) デバイス登録: 同意なしは 400 CONSENT_REQUIRED =="
check consent_required 400 "$(code -X POST $BASE/api/tenants/$TID/devices -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"display_name":"e2e-car"}')"

echo "== 8) デバイス登録: 同意あり → secret 1回発行 =="
check device_create 201 "$(code -X POST $BASE/api/tenants/$TID/devices -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"display_name":"e2e-car","consent_confirmed":true}')"
SECRET=$(jqget "['secret']")
DID=$(jqget "['device']['id']")
[ -n "$SECRET" ] && { PASS=$((PASS+1)); echo "  OK  secret issued (${#SECRET} chars)"; } || { FAIL=$((FAIL+1)); echo "  NG  no secret"; }

echo "== 9) シークレットローテーション =="
check rotate 200 "$(code -X POST $BASE/api/tenants/$TID/devices/$DID/rotate -H "Authorization: Bearer $TOKEN")"
NEW_SECRET=$(jqget "['secret']")
[ "$SECRET" != "$NEW_SECRET" ] && { PASS=$((PASS+1)); echo "  OK  secret rotated"; } || { FAIL=$((FAIL+1)); echo "  NG  secret unchanged"; }

echo "== 10) suspend 中のデバイス登録は 409 =="
check suspend 200 "$(code -X POST $BASE/api/admin/tenants/$TID/suspend -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{}')"
check create_while_suspended 409 "$(code -X POST $BASE/api/tenants/$TID/devices -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"display_name":"x","consent_confirmed":true}')"

echo "== 11) resume =="
check resume 200 "$(code -X POST $BASE/api/admin/tenants/$TID/suspend -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{"resume":true}')"

echo "== 12) リフレッシュ回転 (旧refreshは再利用不可) =="
check refresh 200 "$(code -X POST $BASE/api/auth/refresh -H 'Content-Type: application/json' -d "{\"refresh_token\":\"$REFRESH\"}")"
check refresh_reuse_revoked 401 "$(code -X POST $BASE/api/auth/refresh -H 'Content-Type: application/json' -d "{\"refresh_token\":\"$REFRESH\"}")"

echo "== 13) 監査ログ記録 =="
AUDIT_COUNT=$(code $BASE/api/tenants/$TID/audit_logs -H "Authorization: Bearer $TOKEN" >/dev/null; python3 -c "import json; print(len(json.load(open('/tmp/api_e2e_body.json'))['logs']))" 2>/dev/null || echo 0)
[ "${AUDIT_COUNT:-0}" -ge 3 ] && { PASS=$((PASS+1)); echo "  OK  audit logs recorded ($AUDIT_COUNT)"; } || { FAIL=$((FAIL+1)); echo "  NG  audit logs missing ($AUDIT_COUNT)"; }

echo ""
echo "RESULT: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && echo "ALL GREEN" || exit 1
