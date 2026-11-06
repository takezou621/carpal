/**
 * api.js — 認証付き API フェッチヘルパー
 *
 * トークンは localStorage 管理 (LANベンチ用。本番はhttpOnly cookie + HTTPS に移行 —
 * docs/backend-design.md 冒頭の遂脱記載を参照)。
 */

const API_BASE = `http://${location.hostname}:3001`;

export const auth = {
  get access() { return localStorage.getItem('carpal.access_token'); },
  get refresh() { return localStorage.getItem('carpal.refresh_token'); },
  get tid() { return localStorage.getItem('carpal.tid'); },
  get role() { return localStorage.getItem('carpal.role'); },
  get sys() { return localStorage.getItem('carpal.sys') === 'true'; },
  save({ access_token, refresh_token, tenant_id, role, sys }) {
    localStorage.setItem('carpal.access_token', access_token);
    localStorage.setItem('carpal.refresh_token', refresh_token);
    if (tenant_id) localStorage.setItem('carpal.tid', tenant_id);
    if (role) localStorage.setItem('carpal.role', role);
    if (sys !== undefined) localStorage.setItem('carpal.sys', String(!!sys));
  },
  clear() {
    ['carpal.access_token', 'carpal.refresh_token', 'carpal.tid', 'carpal.role', 'carpal.sys']
      .forEach(k => localStorage.removeItem(k));
  },
  get loggedIn() { return !!this.access; },
};

/** 401時にリフレッシュ→再試行するフェッチ */
export async function apiFetch(path, options = {}) {
  const doFetch = () => fetch(`${API_BASE}${path}`, {
    ...options,
    headers: {
      'Content-Type': 'application/json',
      Authorization: `Bearer ${auth.access}`,
      ...(options.headers || {}),
    },
  });
  let res = await doFetch();
  if (res.status === 401 && auth.refresh) {
    // リフレッシュ試行 (1回のみ)
    const rr = await fetch(`${API_BASE}/api/auth/refresh`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ refresh_token: auth.refresh }),
    });
    if (rr.ok) {
      auth.save(await rr.json());
      res = await doFetch();
    } else {
      auth.clear();
      location.href = 'login.html';
      throw new Error('セッション期限切れ — ログインし直してください');
    }
  }
  const body = await res.json().catch(() => ({}));
  if (!res.ok) {
    const err = new Error(body?.error?.message || `API ${res.status}`);
    err.code = body?.error?.code || String(res.status);
    err.status = res.status;
    throw err;
  }
  return body;
}

/** ログイン実行 */
export async function login(email, password) {
  const res = await fetch(`${API_BASE}/api/auth/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ email, password }),
  });
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(body?.error?.message || 'ログインに失敗しました');
  auth.save(body);
  return body;
}

/** 未ログインならログインページへ */
export function requireAuth() {
  if (!auth.loggedIn) location.href = 'login.html';
  return auth.loggedIn;
}

export function logout() {
  auth.clear();
  location.href = 'login.html';
}
