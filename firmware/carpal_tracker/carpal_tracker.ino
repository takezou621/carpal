/**
 * carpal_tracker.ino — LILYGO T-SIM7080G-S3 用 車両追跡ファームウェア
 *
 * 公式リポジトリ (github.com/Xinyuan-LilyGO/LilyGo-T-SIM7080G) の
 * utilities.h / MinimalModemGPSExample に準拠した実装。
 *
 * ハードウェア構成 (T-SIM7080G-S3):
 *   - ESP32-S3 (ネイティブUSB)
 *   - SIM7080G モデム: 電源は AXP2101 PMU の DC3 (3000mV)
 *   - GPSアンテナ電源: PMU BLDO2 (3300mV) — GPS使用前に必ずON
 *   - ピン: UART RX=4 TX=5 / PWRKEY=41 / DTR=42 / RI=3 / I2C SDA=15 SCL=7
 *
 * 接続経路:
 *   - CARPAL_USE_LTE 0: ESP32-S3 の WiFi で Mosquitto に接続 (ベンチ検証用)
 *   - CARPAL_USE_LTE 1: SIM7080G の LTE Cat-M1 (SORACOM plan-D / ドコモ網) 経由。
 *     MQTT は SORACOM Beam MQTT エントリポイント (beam.soracom.io:1883, 平文) を
 *     経由し、Beam が MQTTS で公開ブローカーへ転送する (operations-design.md §5.2)
 *
 * 必要ライブラリ:
 *   - TinyGSM 0.12.0 (LilyGO fork — リポジトリ lib/TinyGSM を推奨)
 *   - XPowersLib     (リポジトリ lib/XPowersLib)
 *   - PubSubClient   (リポジトリ lib/pubsubclient)
 *   - ArduinoJson v7 (Library Manager)
 *
 * 認証情報は secrets.h に分離 (secrets.h.example をコピー。コミット禁止)。
 */

#include <Arduino.h>

// ===== 接続経路の選択 (secrets.h で上書き可) =====
#if __has_include("secrets.h")
#include "secrets.h"
#define CARPAL_HAS_SECRETS 1
// APN 認証 — SORACOM 公式 APN 設定 (APN soracom.io / sora / sora、CHAP or PAP)。
// secrets.h に未定義のときの既定値 (旧 secrets.h でも LTE モードがビルド可能になる)
#ifndef APN
#define APN "soracom.io"
#endif
#ifndef APN_USER
#define APN_USER "sora"
#endif
#ifndef APN_PASS
#define APN_PASS "sora"
#endif
#else
#warning "secrets.h がありません。secrets.h.example をコピーして設定してください"
#define CARPAL_HAS_SECRETS 0
#define WIFI_SSID ""
#define WIFI_PASS ""
#define MQTT_HOST ""
#define MQTT_PORT 1883
#define MQTT_USE_TLS 0
#define MQTT_USER ""
#define MQTT_PASS ""
#define CARPAL_TENANT_ID "default"
#define CARPAL_DEVICE_ID "car01"
#define CARPAL_DEVICE_SECRET "change-me"
#define CARPAL_USE_LTE 0
#define APN "soracom.io"
#define APN_USER "sora"
#define APN_PASS "sora"
#endif

#if CARPAL_USE_LTE
#define TINY_GSM_MODEM_SIM7080
#define TINY_GSM_RX_BUFFER 1024
#include <TinyGsmClient.h>
#else
#include <WiFi.h>
// WiFiベンチモードでも GNSS は SIM7080G の AT 経由で使うため TinyGSM を読む
#define TINY_GSM_MODEM_SIM7080
#define TINY_GSM_RX_BUFFER 1024
#include <TinyGsmClient.h>
#endif

#include <PubSubClient.h>
#include <ArduinoJson.h>

// ===== ボードピン定義 (公式 utilities.h 準拠) =====
#define BOARD_MODEM_PWR_PIN   41   // SIM7080G PWRKEY
#define BOARD_MODEM_DTR_PIN   42
#define BOARD_MODEM_RI_PIN    3
#define BOARD_MODEM_RXD_PIN   4    // ESP32-S3 側 RX (モデム TX)
#define BOARD_MODEM_TXD_PIN   5    // ESP32-S3 側 TX (モデム RX)
#define I2C_SDA               15
#define I2C_SCL               7

// ===== PMU (AXP2101) =====
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
XPowersPMU PMU;

// ===== MQTT トピック (docs/contract.md 準拠) =====
String TOPIC_LOC, TOPIC_STATUS, TOPIC_LOG;

// ===== 送信間隔 (適応制御) =====
const uint32_t INTERVAL_MOVING_MS = 3000;
const uint32_t INTERVAL_IDLE_MS   = 30000;
const float    MOVING_SPEED_KMH   = 5.0;

// ===== 再接続バックオフ (operations-design §1.1) =====
uint32_t backoffMs = 1000;
uint32_t lastAttemptMs = 0;
uint32_t mqttFailingSince = 0;   // MQTT が連続失敗し始めた時刻 (0 = 失敗なし)

// GNSS fix 待ちの進捗を log トピックへ報告する間隔 (contract.md §3.1)
const uint32_t GNSS_LOG_INTERVAL_MS = 15000;
uint32_t gnssWaitStart = 0;

// GNSS ポーリング間隔。1 秒ごとの getGPS (AT+CGNSINF) が LTE TCP と干渉して
// 接続が ~47 秒で切れる実機事象があり、5 秒に緩めて回避 (2026-11-10)
#ifndef GNSS_POLL_INTERVAL_MS
#define GNSS_POLL_INTERVAL_MS 5000
#endif

// ===== クライアント =====
#if CARPAL_USE_LTE
// NOTE: TinyGSM の SIM7080 ドライバは TLS ソケット未対応。
// LTE では SORACOM Beam MQTT エントリポイント (beam.soracom.io:1883) に平文で
// 接続し、Beam が転送先ブローカー (MQTTS) との TLS を肩代わりする。
// 平文区間は SORACOM 網内に閉じる (docs/operations-design.md §5.2)。
// Beam 非依存の TLS はモデム内蔵 MQTT AT (AT+CMQTT*) への移行が別途必要 (Phase 2-e)。
#if MQTT_USE_TLS
#warning "LTE + MQTT_USE_TLS は未対応です (Beam MQTT エントリポイントは平文 1883 で接続します)"
#endif
#define SerialAT Serial1   // LilyGO 公式 example 準拠のモデム UART 別名
TinyGsm       modem(SerialAT);
TinyGsmClient netClient(modem);
#else
#include <WiFiClient.h>
#if MQTT_USE_TLS
#include <WiFiClientSecure.h>
#include "isrgrootx1.h"
// TLS 8883 (mqtt.takezou.com)。Let's Encrypt ルート CA (ISRG Root X1) で検証する
WiFiClientSecure netClient;
#else
WiFiClient    netClient;
#endif
TinyGsm       modem(Serial1);
#endif
PubSubClient  mqtt(netClient);

// ============================================================
// PMU 初期化 — モデム電源 (DC3) と GPSアンテナ電源 (BLDO2)
// 重要: DC3 を投入する **前に** PWRKEY を LOW に固定しておくこと。
// SIM7080G の autostart は「電源投入時に PWRKEY が LOW」の場合のみ
// 働くため、順序を間違えるとモデムが起動しない (実機で確認)。
// ============================================================
// AXP2101 の実アドレスをプローブする (0x5A 通常 / 0x34 fault移行時)
// NOTE: XPowersLib begin() は2回目呼び出しで __addr を更新しないため、
//       begin を呼ぶ前に必ず1回だけ正しいアドレスを確定させること。
uint8_t probePmuAddress() {
  Wire.begin(I2C_SDA, I2C_SCL);
  const uint8_t candidates[] = {0x5A, 0x34};
  for (uint8_t addr : candidates) {
    Wire.beginTransmission(addr);
    Wire.write(0x03);                       // IC_TYPE register
    if (Wire.endTransmission(false) == 0) {
      Wire.requestFrom(addr, (uint8_t)1);
      if (Wire.available() && Wire.read() == 0x4A) {   // AXP2101 chip id
        Serial.printf("[PMU] AXP2101 found at 0x%02X\n", addr);
        return addr;
      }
    }
  }
  return 0;
}

bool pmuInit() {
  // PWRKEY を LOW に固定 (autostart 有効化)
  pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
  digitalWrite(BOARD_MODEM_PWR_PIN, LOW);

  uint8_t addr = probePmuAddress();
  if (addr == 0) {
    Serial.println("[PMU] AXP2101 not found on I2C (0x5A/0x34)");
    return false;
  }
  if (!PMU.begin(Wire, addr, I2C_SDA, I2C_SCL)) {
    return false;
  }
  // 電源サイクル: モデム電源を一度OFFにしてクリーンに再起動
  PMU.disableDC3();
  delay(1500);               // 残電荷による不完全リセットを避けるため1.5秒
  PMU.setDC3Voltage(3000);   // SIM7080G メイン電源 2700~3400mV
  PMU.enableDC3();
  delay(500);                // 電源安定待ち (autostart がここで発火する)

  PMU.setBLDO2Voltage(3300);
  PMU.enableBLDO2();         // GPSアンテナ電源 — GPSに必須

  PMU.disableTSPinMeasure(); // TSピン検出はOFFにしないと充電できない
  return true;
}

// ============================================================
// モデム起動 — autostart を待ち、必要なら手動 PWRKEY (LOW 1.2秒)
// ============================================================
bool modemPowerOn() {
  Serial1.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);

  // DTR=LOW でスリープ抑止
  pinMode(BOARD_MODEM_DTR_PIN, OUTPUT);
  digitalWrite(BOARD_MODEM_DTR_PIN, LOW);

  // autostart による起動を待つ (PMU が DC3 を投入済み)
  int retry = 0;
  while (!modem.testAT(1000)) {
    Serial.print(".");
    mqtt.loop();                       // 起動中もMQTT keepaliveを維持
    if (retry++ > 8) {
      // autostart しなかった場合: 手動起動。
      // ボードの PWRKEY 回路はトランジスタ反転型のため、ESPピン HIGH ≥1.5秒
      // が PWRKEY アサート (実機での成否パターンと整合: HIGH 1000ms は境界値)
      Serial.println("\n[MODEM] manual power key (pin HIGH 2s)...");
      digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
      delay(2000);
      digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
      retry = 0;
    }
    delay(500);
  }
  return true;
}

// ============================================================
// GPS → epoch ms (docs/contract.md §2: ts は GNSS UTC から生成)
// ============================================================
long long gpsEpochMs(int y, int mo, int d, int h, int mi, int s) {
  if (y < 100) y += 2000;               // 2桁年対策
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
  time_t epoch = mktime(&t);            // setup で TZ=UTC 済
  return (epoch > 1577836800L) ? (long long)epoch * 1000LL : 0; // 2020年以降のみ有効
}

// ============================================================
// MQTT
// ============================================================
bool mqttEnsureConnected() {
  if (mqtt.connected()) return true;
  if (millis() - lastAttemptMs < backoffMs) return false;   // 指数バックオフ
  lastAttemptMs = millis();

  Serial.printf("[MQTT] connecting to %s:%u (backoff %ums)...\n",
                MQTT_HOST, MQTT_PORT, backoffMs);
  String will = String("{\"id\":\"") + CARPAL_DEVICE_ID + "\",\"online\":false}";
  bool ok = mqtt.connect(CARPAL_DEVICE_ID, MQTT_USER, MQTT_PASS,
                         TOPIC_STATUS.c_str(), 1, true, will.c_str());
  if (ok) {
    Serial.println("[MQTT] connected");
    backoffMs = 1000;   // 成功でリセット
    mqttFailingSince = 0;
    String online = String("{\"id\":\"") + CARPAL_DEVICE_ID + "\",\"online\":true}";
    mqtt.publish(TOPIC_STATUS.c_str(), online.c_str(), true);
  } else {
    Serial.printf("[MQTT] failed state=%d\n", mqtt.state());
    netClient.stop();   // 切断後の stale ソケットを閉じないと次の TCP 接続が連続失敗する
    if (mqttFailingSince == 0) mqttFailingSince = millis();
    backoffMs = (backoffMs < 60000) ? backoffMs * 2 : 60000;   // 1s→60s
  }
  return ok;
}

float bearingDeg(double lat1, double lng1, double lat2, double lng2) {
  double rad = PI / 180.0;
  double y = sin((lng2 - lng1) * rad) * cos(lat2 * rad);
  double x = cos(lat1 * rad) * sin(lat2 * rad)
           - sin(lat1 * rad) * cos(lat2 * rad) * cos((lng2 - lng1) * rad);
  return (float)fmod(atan2(y, x) / rad + 360.0, 360.0);
}

// ============================================================
// 位置パブリッシュ (docs/contract.md §2 スキーマ)
// ============================================================
void publishLocation(double lat, double lng, float kmh, float brg, int usat,
                     long long tsMs) {
  JsonDocument doc;
  doc["id"]  = CARPAL_DEVICE_ID;
  doc["lat"] = serialized(String(lat, 6));
  doc["lng"] = serialized(String(lng, 6));
  doc["spd"] = kmh;
  if (brg >= 0) doc["brg"] = brg;
  if (usat > 0) doc["sats"] = usat;
  if (tsMs > 0) doc["ts"]   = tsMs;

  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  // NOTE: PubSubClient 2.7 では publish(topic, char*, size_t) が
  //       (topic, payload, retain=true) の3引数オーバーロードと解釈され
  //       retain が立つため、4引数で retain=false を明示する
  bool ok = mqtt.publish(TOPIC_LOC.c_str(), (const uint8_t*)buf, (unsigned int)n, false);
  Serial.printf("[PUB] %s %s\n", ok ? "OK" : "FAIL", buf);
}

// ============================================================
// setup / loop
// ============================================================
double lastLat = 0, lastLng = 0;
bool   lastFixValid = false;
uint32_t lastPublishMs = 0;
bool   modemReady = false;   // GNSS 有効化済み (モデム電源リサイクル時に false へ戻す)

// ============================================================
// 診断ログ発報 (contract.md §3.1: carpal/{tenant}/{device}/log)
// ============================================================
void publishLog(const char* level, const char* event, const String &msg) {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["id"]    = CARPAL_DEVICE_ID;
  doc["level"] = level;
  doc["event"] = event;
  doc["msg"]   = msg;
  char buf[192];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  // retain=false を明示 (3引数オーバーロードの罠 — publishLocation 側のNOTE参照)
  mqtt.publish(TOPIC_LOG.c_str(), (const uint8_t*)buf, (unsigned int)n, false);
}

// ============================================================
// 電波強度 (診断ログ用)。LTE: TinyGSM 0-31 (99=不明) / WiFi: dBm
// ============================================================
static inline int signalLevel() {
#if CARPAL_USE_LTE
  return modem.getSignalQuality();
#else
  return WiFi.RSSI();
#endif
}

void setup() {
  Serial.begin(115200);
  // GNSS は UTC を返すため mktime の解釈を UTC 固定に
  setenv("TZ", "UTC", 1);
  tzset();
  delay(2500);

  TOPIC_LOC    = String("carpal/") + CARPAL_TENANT_ID + "/" + CARPAL_DEVICE_ID + "/location";
  TOPIC_STATUS = String("carpal/") + CARPAL_TENANT_ID + "/" + CARPAL_DEVICE_ID + "/status";
  TOPIC_LOG    = String("carpal/") + CARPAL_TENANT_ID + "/" + CARPAL_DEVICE_ID + "/log";
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
#if !CARPAL_USE_LTE && MQTT_USE_TLS
  // ルート CA を設定 (WiFi + TLS 8883 のみ。docs/backend-design.md §2.2)
  netClient.setCACert(ISRG_ROOT_X1_PEM);
#endif
  // NOTE: LilyGO lib の PubSubClient 2.7 には setKeepAlive/setSocketTimeout が無い
  // (デフォルト KeepAlive 15秒・タイムアウト 15秒を使用。LWT検出は15秒後)

#if CARPAL_USE_LTE
  // ---- LTE モード: PMUでモデム/GPS電源を確保してからモデム起動 ----
  if (!pmuInit()) {
    Serial.println("[PMU] init FAILED — halting (check wiring/board model)");
    while (true) delay(10000);
  }
  Serial.println("[PMU] AXP2101 OK (modem DC3=3.0V, GPS BLDO2=3.3V)");
  modemPowerOn();
  Serial.print("[MODEM] ");
  Serial.println(modem.getModemInfo());
  // ドコモ網は GSM / NB-IoT を提供していないため、LTE Cat-M1 に固定する。
  // 放置すると GSM/NB を含む全方式スキャンで登録が遅くなる (operations-design.md §5.2)
  modem.sendAT("+CNMP=38");   // 38 = LTE のみ
  modem.waitResponse();
  modem.sendAT("+CMNB=1");    // 1 = Cat-M1 優先 (NB-IoT は plan-D 非対応)
  modem.waitResponse();
#else
  // ---- ベンチ: WiFi で LAN の Mosquitto へ。GPS用に PMU の BLDO2 も有効化 ----
  if (pmuInit()) {
    Serial.println("[PMU] AXP2101 OK (GPS antenna power BLDO2=3.3V)");
  } else {
    Serial.println("[PMU] init FAILED — GPS antenna power unavailable");
  }
  Serial.printf("[WiFi] connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(400);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] connected, IP=%s  RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("\n[WiFi] FAILED status=%d (1=SSID未検出/6=切断: パス相違・5GHz専用SSIDに注意)\n",
                  WiFi.status());
  }
#endif
}

void loop() {
#if CARPAL_USE_LTE
  if (!modem.isNetworkConnected()) {
    // 登録完了を待ってからデータ接続する (連打すると登録が進まない場合がある)
    Serial.println("[NET] waiting for LTE registration (Cat-M1)...");
    if (modem.waitForNetwork(60000L) && modem.isNetworkConnected()) {
      Serial.printf("[NET] registered, connecting data (APN=%s)...\n", APN);
      if (!modem.gprsConnect(APN, APN_USER, APN_PASS)) {
        Serial.println("[NET] gprsConnect FAILED");
      }
    } else {
      Serial.printf("[NET] registration timeout (signal=%d)\n", modem.getSignalQuality());
    }
  } else if (mqttFailingSince != 0 && millis() - mqttFailingSince > 120000UL) {
    // 登録は生きているのに TCP が通らない → PDP (データ経路) だけ死んでいる。
    // gprsConnect をやり直して経路を作り直す (2026-11-10 実機で発生したパターン)
    Serial.println("[NET] registered but TCP unreachable — PDP refresh");
    modem.gprsDisconnect();
    bool ok = modem.gprsConnect(APN, APN_USER, APN_PASS);
    if (ok) {
      Serial.println("[NET] PDP re-established");
    } else {
      // 段階2: モデム電源をリサイクルしてから再登録・再接続
      Serial.println("[NET] PDP re-establish FAILED — modem power cycle");
      modem.gprsDisconnect();
      PMU.disableDC3();
      delay(1500);
      PMU.enableDC3();
      delay(500);
      modemPowerOn();
      Serial.print("[MODEM] ");
      Serial.println(modem.getModemInfo());
      modem.sendAT("+CNMP=38");
      modem.waitResponse();
      modem.sendAT("+CMNB=1");
      modem.waitResponse();
      modemReady = false;   // モデム再起動後に GNSS を有効化し直す
      Serial.println("[NET] re-registering after power cycle...");
      if (modem.waitForNetwork(60000L) && modem.isNetworkConnected()) {
        modem.gprsConnect(APN, APN_USER, APN_PASS);
      }
    }
    mqttFailingSince = millis();   // 次の判定まで再び待つ
    backoffMs = 1000;              // 経路更新後はすぐ試す
    lastAttemptMs = 0;
  }
#else
  // WiFi 再接続: 接続試行中に reconnect() を連打すると失敗するため10秒クールダウン
  static uint32_t lastWifiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry > 10000) {
    lastWifiRetry = millis();
    Serial.printf("[WiFi] retry (status=%d)\n", WiFi.status());
    WiFi.reconnect();
  }
#endif

  if (!mqttEnsureConnected()) {
    mqtt.loop();
    delay(1000);
    return;
  }

  // ---- GNSS 取得 (TinyGSM getGPS: 13引数・速度はノット) ----
  // NOTE: CARPAL_SKIP_GNSS=1 でビルドすると GNSS を止めて MQTT 接続安定性を
  //       単独検証できる (2026-11-10: GNSS 待ち開始 ~47s で切断が再現した切り分け用)
#ifndef CARPAL_SKIP_GNSS
  float lat2 = 0, lon2 = 0, speed2 = 0, alt2 = 0, accuracy2 = 0;
  int vsat2 = 0, usat2 = 0, y2 = 0, mo2 = 0, d2 = 0, h2 = 0, mi2 = 0, s2 = 0;

  if (!modemReady) {
    Serial.println("[MODEM] power-on for GNSS...");
    modemPowerOn();
    Serial.print("[MODEM] ");
    Serial.println(modem.getModemInfo());
    if (!modem.enableGPS()) {
      Serial.println("[GNSS] enableGPS failed — retry in 30s");
      delay(30000);
      return;
    }    // 測位高速化: GPS + BEIDOU 構成 (公式 MinimalModemGPSExample の ADVANCED_GPS_CONFIG 準拠)
    modem.sendAT("+CGNSMOD=1,0,1,0,0");
    modem.waitResponse();
    Serial.println("[GNSS] enabled (GPS+BEIDOU), waiting for fix...");
    modemReady = true;
  }

  // getGPS の発熱的な AT 連打を抑える: GNSS_POLL_INTERVAL_MS ごとにのみ測位を読む
  // (間で mqtt.loop() を回して keepalive を維持)
  static uint32_t lastGpsPoll = 0;
  if (millis() - lastGpsPoll < GNSS_POLL_INTERVAL_MS) {
    mqtt.loop();
    delay(200);
    return;
  }
  lastGpsPoll = millis();

  bool have = modem.getGPS(&lat2, &lon2, &speed2, &alt2, &vsat2, &usat2,
                           &accuracy2, &y2, &mo2, &d2, &h2, &mi2, &s2);

  if (!have || (lat2 == 0.0 && lon2 == 0.0)) {
    static uint32_t lastWait = 0;
    if (gnssWaitStart == 0) gnssWaitStart = millis();
    // 進捗を log トピックへ報告 (USB を外して移動しても監視可能にする)
    if (millis() - lastWait > GNSS_LOG_INTERVAL_MS) {
      uint32_t elapsed = (millis() - gnssWaitStart) / 1000;
      char m[112];
      snprintf(m, sizeof(m),
               "elapsed=%lus sats=%d/%d rssi=%d dc3=%u/%umV bldo2=%u",
               (unsigned long)elapsed, usat2, vsat2, signalLevel(),
               (unsigned)PMU.isEnableDC3(), (unsigned)PMU.getDC3Voltage(),
               (unsigned)PMU.isEnableBLDO2());
      publishLog("INFO", "GNSS_WAITING", m);
      Serial.printf("[GNSS] waiting for fix... %s\n", m);
      lastWait = millis();
    }
    mqtt.loop();
    delay(1000);
    return;
  }

  // 初fixを log へ報告
  if (gnssWaitStart > 0) {
    char m[96];
    snprintf(m, sizeof(m), "fix in %lus sats=%d lat=%.6f lng=%.6f",
             (unsigned long)((millis() - gnssWaitStart) / 1000), usat2, lat2, lon2);
    publishLog("INFO", "GNSS_FIX", m);
    Serial.printf("[GNSS] FIX! %s\n", m);
    gnssWaitStart = 0;
  }

  float kmh = speed2 * 1.852;   // ノット → km/h
  float brg = lastFixValid ? bearingDeg(lastLat, lastLng, lat2, lon2) : -1.0f;

  bool moving = kmh >= MOVING_SPEED_KMH;
  uint32_t interval = moving ? INTERVAL_MOVING_MS : INTERVAL_IDLE_MS;
  if (millis() - lastPublishMs >= interval) {
    long long tsMs = gpsEpochMs(y2, mo2, d2, h2, mi2, s2);
    publishLocation(lat2, lon2, kmh, brg, usat2, tsMs);
    lastPublishMs = millis();
    lastLat = lat2; lastLng = lon2; lastFixValid = true;
  }

#endif   // CARPAL_SKIP_GNSS

  mqtt.loop();
  delay(500);
}
