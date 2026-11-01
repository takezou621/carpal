/**
 * carpal_tracker.ino
 * LILYGO T-SIM7080-S3 用 車両追跡ファームウェア
 *
 * - SIM7080G の GNSS で緯度経度・速度・方位を取得
 * - LTE (Cat-M) 接続後、MQTT で JSON をパブリッシュ
 * - 停車時は送信間隔を伸ばす省電力制御つき
 *
 * 必要ライブラリ (Arduino Library Manager):
 *   - TinyGSM          (>= 0.11.7)  : モデム制御 (MQTTも内蔵のTinyGSMClient MQTTが便利)
 *   - PubSubClient     (>= 2.3)     : MQTT クライアント
 *   - ArduinoJson      (>= 7.x)     : JSON 生成
 *
 * ボード設定:
 *   Board: ESP32S3 Dev Module
 *   USB CDC On Boot: Enabled
 *   PS: T-SIM7080-S3 は PSRAM搭載 → OPI PSRAM 有効推奨
 */

#include <Arduino.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ============================================================
// ハードウェア定義 (T-SIM7080-S3)
// ============================================================
#define UART_BAUD           115200
#define PIN_DTR             35
#define PIN_TX              26
#define PIN_RX              27
#define PWR_PIN             4
#define SD_DET              33
#define MODEM_BAUD          115200

// ============================================================
// ユーザ設定 (環境に合わせて変更)
// ============================================================
const char*  APN          = "your-apn";            // SIM の APN
const char*  MQTT_HOST    = "192.168.1.100";       // Mosquitto (自宅 Proxmox のアドレス)
const uint16_t MQTT_PORT  = 1883;
const char*  MQTT_USER    = "";                    // 認証なしの場合は空
const char*  MQTT_PASS    = "";
const char*  TENANT_ID    = "default";               // テナントID (将来SaaS化時にUUID化)
const char*  DEVICE_ID    = "car01";                  // デバイスID (車両ごとに変更)
const char*  DEVICE_SECRET= "change-me";              // デバイス認証シークレット (MQTTパスワード)

// トピック設計 (マルチテナント対応):
//   carpal/{tenant_id}/{device_id}/location
//   carpal/{tenant_id}/{device_id}/status  … LWTでオフライン通知
String TOPIC_LOC, TOPIC_STATUS;

// ============================================================
// 送信間隔 (適応制御)
// ============================================================
const uint32_t INTERVAL_MOVING_MS = 3000;    // 移動中: 3秒間隔
const uint32_t INTERVAL_IDLE_MS   = 30000;   // 停車中: 30秒間隔
const float    MOVING_SPEED_KMH   = 5.0;     // 5km/h 以上で「移動中」とみなす

// ============================================================
// グローバル
// ============================================================
TinyGsm        modem(Serial1);
TinyGsmClient  gsmClient(modem);
PubSubClient   mqtt(gsmClient);

struct GpsData {
  double lat = 0.0;
  double lng = 0.0;
  float  speedKmh = 0.0;
  float  hdop = 99.9;
  int    satellites = 0;
  bool   valid = false;
};

GpsData lastFix;                 // 最後に有効だった測位 (ベアリング計算用)
uint32_t lastPublishMs = 0;

// ============================================================
// モデム起動
// ============================================================
bool modemPowerOn() {
  // T-SIM7080-S3 は PWR_PIN の High→Low パルスで電源ON
  pinMode(PWR_PIN, OUTPUT);
  digitalWrite(PWR_PIN, HIGH);
  delay(300);
  digitalWrite(PWR_PIN, LOW);
  delay(1000);

  Serial.println("Waiting for modem...");
  for (uint32_t start = millis(); millis() - start < 15000; ) {
    if (modem.testAT()) return true;
    delay(200);
  }
  // 応答しない場合は再度電源パルス
  digitalWrite(PWR_PIN, HIGH); delay(1000);
  digitalWrite(PWR_PIN, LOW);
  for (uint32_t start = millis(); millis() - start < 15000; ) {
    if (modem.testAT()) return true;
    delay(200);
  }
  return false;
}

// ============================================================
// GNSS (SIM7080G 内蔵) 取得
// ============================================================
bool readGnss(GpsData &out) {
  if (!modem.enableGNSS(1, 24)) {   // enable=1, GNSS system: GPS+GLONASS+BEIDOU (bitmask 24 → 必要に応じ調整)
    // すでに有効な場合は失敗することがあるため無視して継続
  }
  float lat = 0, lng = 0, speed = 0, hdop = 99;
  int    sats = 0;
  bool   ok = modem.getGNSS(&lat, &lng, &speed, nullptr, nullptr, nullptr, &sats);
  // TinyGSM の実装により引数が異なる場合は getGNSS のシグネチャに合わせて調整してください。
  if (!ok || (lat == 0.0 && lng == 0.0)) return false;

  out.lat       = lat;
  out.lng       = lng;
  out.speedKmh  = speed;
  out.satellites = sats;
  out.valid     = true;
  return true;
}

// ============================================================
// MQTT
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  // 本ファームは Subscribe を使わないが、コマンド受信時はここで処理
}

// ============================================================
// 時刻 (epoch ms) — 監査 H-3 対応
// NITZ (モバイル網の時刻) から取得。取得できない場合は ts を省略し、
// サーバ (ingest) が受信時刻で代用する。millis() は使わない (49日周回問題)。
// ============================================================
long epochSeconds() {
  // TinyGSM: "YY/MM/DD,HH:MM:SS+zz" 形式
  String dt = modem.getGSMDateTime(DATETIME_FULL);
  if (dt.length() < 20) return 0;
  int y  = dt.substring(0, 2).toInt() + 2000;
  int mo = dt.substring(3, 5).toInt();
  int d  = dt.substring(6, 8).toInt();
  int h  = dt.substring(9, 11).toInt();
  int mi = dt.substring(12, 14).toInt();
  int se = dt.substring(15, 17).toInt();
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = mi; t.tm_sec = se;
  time_t epoch = mktime(&t);
  return (epoch > 1000000000L) ? (long)epoch : 0;   // 妥当性チェック
}

void publishLocation(const GpsData &fix) {
  JsonDocument doc;
  doc["id"]     = DEVICE_ID;
  doc["lat"]    = serialized(String(fix.lat, 6));
  doc["lng"]    = serialized(String(fix.lng, 6));
  doc["spd"]    = fix.speedKmh;
  long es = epochSeconds();
  if (es > 0) {
    doc["ts"] = (long long)es * 1000LL;    // epoch ms
  }                                        // 取得できない場合は省略 (サーバ側で受信時刻使用)
  if (lastFix.valid) {
    doc["brg"]   = bearingDeg(lastFix.lat, lastFix.lng, fix.lat, fix.lng); // 進行方向
  }
  doc["sats"]   = fix.satellites;

  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  bool ok = mqtt.publish(TOPIC_LOC.c_str(), buf, n);
  Serial.printf("[MQTT] publish(%s): %s  %s\n", TOPIC_LOC.c_str(), ok ? "OK" : "FAIL", buf);
}

bool mqttEnsureConnected() {
  if (mqtt.connected()) return true;
  Serial.print("[MQTT] connecting... ");
  String willMsg = String("{\"id\":\"") + DEVICE_ID + "\",\"online\":false}";
  bool ok = mqtt.connect(DEVICE_ID, DEVICE_ID, DEVICE_SECRET,
                         TOPIC_STATUS.c_str(), 1, true, willMsg.c_str());
  if (ok) {
    Serial.println("OK");
    // LWT の逆 = オンライン通知
    String online = String("{\"id\":\"") + DEVICE_ID + "\",\"online\":true}";
    mqtt.publish(TOPIC_STATUS.c_str(), online.c_str(), true);
  } else {
    Serial.printf("FAIL state=%d\n", mqtt.state());
  }
  return ok;
}

// ============================================================
// ベアリング計算 (2点間の進行方向)
// ============================================================
float bearingDeg(double lat1, double lng1, double lat2, double lng2) {
  double rad = PI / 180.0;
  double y = sin((lng2 - lng1) * rad) * cos(lat2 * rad);
  double x = cos(lat1 * rad) * sin(lat2 * rad)
           - sin(lat1 * rad) * cos(lat2 * rad) * cos((lng2 - lng1) * rad);
  double deg = atan2(y, x) / rad;
  return (float)fmod(deg + 360.0, 360.0);
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(UART_BAUD);
  delay(2000);

  // 時刻解釈を UTC 固定にする (監査 N-2 対策: NITZ は UTC を返すため
  // ローカルTZの mktime で解釈すると JST 環境で +9h ずれる)
  setenv("TZ", "UTC", 1);
  tzset();

  Serial1.begin(MODEM_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  pinMode(PIN_DTR, OUTPUT);
  digitalWrite(PIN_DTR, LOW);   // DTR=LOW でスリープ抑止
  delay(100);

  TOPIC_LOC    = String("carpal/") + TENANT_ID + "/" + DEVICE_ID + "/location";
  TOPIC_STATUS = String("carpal/") + TENANT_ID + "/" + DEVICE_ID + "/status";
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);

  if (!modemPowerOn()) {
    Serial.println("Modem init failed. Halt.");
    while (true) delay(10000);
  }

  Serial.print("Modem: ");
  Serial.println(modem.getModemInfo());

  // ネットワーク登録
  if (!modem.gprsConnect(APN, "", "")) {
    Serial.println("LTE connect failed. Retry in loop.");
  } else {
    Serial.print("LTE OK, signal: ");
    Serial.println(modem.getSignalQuality());
  }

  modem.enableGNSS(1, 24);   // GNSS 有効化
}

void loop() {
  // ネットワーク断対応
  if (!modem.isNetworkConnected()) {
    Serial.println("[NET] reconnecting...");
    modem.gprsConnect(APN, "", "");
  }

  if (!mqttEnsureConnected()) {
    mqtt.loop();
    delay(2000);
    return;
  }

  GpsData fix;
  bool have = readGnss(fix);

  if (have) {
    bool moving = fix.speedKmh >= MOVING_SPEED_KMH;
    uint32_t interval = moving ? INTERVAL_MOVING_MS : INTERVAL_IDLE_MS;

    if (millis() - lastPublishMs >= interval) {
      publishLocation(fix);
      lastPublishMs = millis();
      lastFix = fix;
    }
  } else {
    Serial.println("[GNSS] no fix yet...");
    delay(1000);
  }

  mqtt.loop();
}
