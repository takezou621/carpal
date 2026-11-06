#!/usr/bin/env node
/**
 * scripts/verify_marker.js — ブラウザ表示のE2E検証 (ヘッドレスChrome)
 *
 * 使い方:
 *   npm i puppeteer-core   (どこかのディレクトリで)
 *   node scripts/verify_marker.js
 *
 * 期待値 (expected) は mosquitto_sub の実測値に合わせて調整。
 * 前提: Mosquitto + 実機orシミュレータが稼働、web/ のデモページサーバ (8000) 起動、
 *       Chrome が /Applications にインストール済み。
 */

(async () => {
  const browser = await puppeteer.launch({
    executablePath: '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    headless: 'new',
    args: ['--no-sandbox'],
  });
  const page = await browser.newPage();
  const consoleLogs = [];
  page.on('console', m => consoleLogs.push(m.text()));

  console.log('loading page...');
  await page.goto('http://127.0.0.1:8000/', { waitUntil: 'networkidle2', timeout: 30000 });

  // マーカー出現を待つ (デバイスは30秒間隔で送信)
  await page.waitForFunction(
    'window.__carpalDebug && window.__carpalDebug.markers && window.__carpalDebug.markers.car01 && window.__carpalDebug.markers.car01.marker.getLatLng().lat !== 0',
    { timeout: 90000 }
  );
  console.log('marker appeared');

  const data = await page.evaluate(() => {
    const ms = window.__carpalDebug.markers;
    const out = {};
    for (const [id, m] of Object.entries(ms)) {
      const ll = m.marker.getLatLng();
      out[id] = {
        lat: ll.lat, lng: ll.lng,
        online: Date.now() - m.lastTs < 60000,
        trailPoints: m.trail.getLatLngs().length,
      };
    }
    return out;
  });

  // 熊谷市の実位置 (直近 mosquitto_sub で受信した値) と比較
  const expected = { lat: 36.133694, lng: 139.394043 };
  const car = data['car01'];
  const latOk = car && Math.abs(car.lat - expected.lat) < 0.05;   // ±5.5km
  const lngOk = car && Math.abs(car.lng - expected.lng) < 0.05;
  const onlineOk = !!(car && car.online);
  const ok = !!(latOk && lngOk && onlineOk);

  // マーカー位置にパンしてスクリーンショット
  if (car) {
    await page.evaluate((lat, lng) => {
      window.__carpalDebug.map.setView([lat, lng], 13);
    }, car.lat, car.lng);
    await new Promise(r => setTimeout(r, 3500));
  }
  await page.screenshot({ path: '/tmp/carpal-verify.png' });

  console.log(JSON.stringify({
    result: ok ? 'PASS' : 'FAIL',
    markers: data,
    checks: { latOk, lngOk, onlineOk },
    consoleLogs: consoleLogs.filter(l => l.includes('[carpal]')).slice(0, 6),
  }, null, 2));
  await browser.close();
})().catch(e => { console.error('VERIFY_FAIL:', e.message); process.exit(1); });
