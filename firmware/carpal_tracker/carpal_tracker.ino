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
 *   - CARPAL_USE_LTE 0 (既定): ESP32-S3 の WiFi で LAN の Mosquitto に接続 (ベンチ検証用)
 *   - CARPAL_USE_LTE 1: SIM7080G の LTE Cat-M 経由 (SIM到着後の実車検証用)
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
#else
#warning "secrets.h がありません。secrets.h.example をコピーして設定してください"
#define CARPAL_HAS_SECRETS 0
#define WIFI_SSID ""
#define WIFI_PASS ""
#define MQTT_HOST ""
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASS ""
#define CARPAL_TENANT_ID "default"
#define CARPAL_DEVICE_ID "car01"
#define CARPAL_DEVICE_SECRET "change-me"
#define CARPAL_USE_LTE 0
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

// GNSS fix 待ちの進捗を log トピックへ報告する間隔 (contract.md §3.1)
const uint32_t GNSS_LOG_INTERVAL_MS = 15000;
uint32_t gnssWaitStart = 0;

// ===== クライアント =====
#if CARPAL_USE_LTE
TinyGsm       modem(SerialAT);
TinyGsmClient netClient(modem);
#else
#include <WiFiClient.h>
WiFiClient    netClient;
TinyGsm       modem(Serial1);
#endif
PubSubClient  mqtt(netClient);

// ============================================================
// PMU 初期化 — モデム電源 (DC3) と GPSアンテナ電源 (BLDO2)
// 公式 MinimalModemGPSExample 準拠
// ============================================================
bool pmuInit() {
  if (!PMU.begin(Wire, 0x5A /*AXP2101_SLAVE_ADDRESS*/, I2C_SDA, I2C_SCL)) {
    return false;
  }
  // 電源サイクル時はモデム電源を一度OFFにしてクリーンに再起動
  PMU.disableDC3();
  delay(200);
  PMU.setDC3Voltage(3000);   // SIM7080G メイン電源 2700~3400mV
  PMU.enableDC3();

  PMU.setBLDO2Voltage(3300);
  PMU.enableBLDO2();         // GPSアンテナ電源 — GPSに必須

  PMU.disableTSPinMeasure(); // TSピン検出はOFFにしないと充電できない
  return true;
}

// ============================================================
// モデム起動 — PWRKEY パルス (公式のシーケンス準拠)
// ============================================================
bool modemPowerOn() {
  Serial1.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);

  pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
  digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
  delay(100);
  digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
  delay(1000);
  digitalWrite(BOARD_MODEM_PWR_PIN, LOW);

  // DTR=LOW でスリープ抑止
  pinMode(BOARD_MODEM_DTR_PIN, OUTPUT);
  digitalWrite(BOARD_MODEM_DTR_PIN, LOW);

  int retry = 0;
  while (!modem.testAT(1000)) {
    Serial.print(".");
    if (retry++ > 15) {
      digitalWrite(BOARD_MODEM_PWR_PIN, LOW);  delay(100);
      digitalWrite(BOARD_MODEM_PWR_PIN, HIGH); delay(1000);
      digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
      retry = 0;
      Serial.println("\nRetry start modem.");
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
    String online = String("{\"id\":\"") + CARPAL_DEVICE_ID + "\",\"online\":true}";
    mqtt.publish(TOPIC_STATUS.c_str(), online.c_str(), true);
  } else {
    Serial.printf("[MQTT] failed state=%d\n", mqtt.state());
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
  bool ok = mqtt.publish(TOPIC_LOC.c_str(), buf, n);
  Serial.printf("[PUB] %s %s\n", ok ? "OK" : "FAIL", buf);
}

// ============================================================
// setup / loop
// ============================================================
double lastLat = 0, lastLng = 0;
bool   lastFixValid = false;
uint32_t lastPublishMs = 0;

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
  mqtt.publish(TOPIC_LOG.c_str(), buf, n);
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
    Serial.println("[NET] reconnecting...");
    modem.gprsConnect(APN, "", "");
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
  float lat2 = 0, lon2 = 0, speed2 = 0, alt2 = 0, accuracy2 = 0;
  int vsat2 = 0, usat2 = 0, y2 = 0, mo2 = 0, d2 = 0, h2 = 0, mi2 = 0, s2 = 0;

  static bool modemReady = false;
  if (!modemReady) {
    Serial.println("[MODEM] power-on for GNSS...");
    modemPowerOn();
    Serial.print("[MODEM] ");
    Serial.println(modem.getModemInfo());
    if (!modem.enableGPS()) {
      Serial.println("[GNSS] enableGPS failed — retry in 30s");
      delay(30000);
      return;
    }
    // 測位高速化: GPS + BEIDOU 構成 (公式 MinimalModemGPSExample の ADVANCED_GPS_CONFIG 準拠)
    modem.sendAT("+CGNSMOD=1,0,1,0,0");
    modem.waitResponse();
    Serial.println("[GNSS] enabled (GPS+BEIDOU), waiting for fix...");
    modemReady = true;
  }

  bool have = modem.getGPS(&lat2, &lon2, &speed2, &alt2, &vsat2, &usat2,
                           &accuracy2, &y2, &mo2, &d2, &h2, &mi2, &s2);

  if (!have || (lat2 == 0.0 && lon2 == 0.0)) {
    static uint32_t lastWait = 0;
    if (gnssWaitStart == 0) gnssWaitStart = millis();
    // 進捗を log トピックへ報告 (USB を外して移動しても監視可能にする)
    if (millis() - lastWait > GNSS_LOG_INTERVAL_MS) {
      uint32_t elapsed = (millis() - gnssWaitStart) / 1000;
      char m[64];
      snprintf(m, sizeof(m), "elapsed=%lus rssi=%d", (unsigned long)elapsed,
               (int)WiFi.RSSI());
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

  mqtt.loop();
  delay(500);
}
