#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <esp_camera.h>
#include <esp_system.h>
#include <mbedtls/base64.h>

#define TINY_GSM_MODEM_SIM7080
#include <TinyGsmClient.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

constexpr int MODEM_RX = 4;
constexpr int MODEM_TX = 5;
constexpr int MODEM_PWR = 41;
constexpr int I2C_SDA = 15;
constexpr int I2C_SCL = 7;
constexpr size_t MAX_JPEG = 200000;

XPowersPMU pmu;
TinyGsm modem(Serial1);
TinyGsmClientSecure client(modem, 0);
Preferences preferences;
std::atomic<bool> parkingArmed{false};
bool cameraReady = false;
QueueHandle_t events;
uint32_t bootId;

struct ParkingEvent {
  uint32_t sequence;
  float acceleration;
  uint8_t *image;
  size_t imageSize;
};

void fatal(const char *message) {
  Serial.println(message);
  delay(5000);
  ESP.restart();
}

bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ACCEL_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readAcceleration(float *values) {
  Wire.beginTransmission(ACCEL_ADDRESS);
  Wire.write(0x32);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(ACCEL_ADDRESS, uint8_t(6)) != 6) return false;
  for (int axis = 0; axis < 3; ++axis) {
    const uint8_t low = Wire.read();
    const uint8_t high = Wire.read();
    values[axis] = static_cast<int16_t>((high << 8) | low) * 0.0039f;
  }
  return true;
}

bool initializeAccelerometer() {
  Wire.beginTransmission(ACCEL_ADDRESS);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(ACCEL_ADDRESS, uint8_t(1)) != 1 ||
      Wire.read() != 0xE5) return false;
  return writeRegister(0x31, 0x0B) &&  // Full resolution, +/-16 g.
         writeRegister(0x2C, 0x09) &&  // 50 Hz.
         writeRegister(0x2D, 0x08);    // Measurement mode.
}

bool initializeCamera() {
  if (!CAMERA_ENABLED) return false;
  pmu.setALDO1Voltage(1800);
  pmu.enableALDO1();
  pmu.setALDO2Voltage(2800);
  pmu.enableALDO2();
  pmu.setALDO4Voltage(3000);
  pmu.enableALDO4();
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 14;
  config.pin_d1 = 47;
  config.pin_d2 = 48;
  config.pin_d3 = 21;
  config.pin_d4 = 13;
  config.pin_d5 = 11;
  config.pin_d6 = 10;
  config.pin_d7 = 9;
  config.pin_xclk = 8;
  config.pin_pclk = 12;
  config.pin_vsync = 16;
  config.pin_href = 17;
  config.pin_sccb_sda = 2;
  config.pin_sccb_scl = 1;
  config.pin_reset = 18;
  config.pin_pwdn = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 15;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  return esp_camera_init(&config) == ESP_OK;
}

void monitorParking(void *) {
  float baseline[3] = {};
  bool calibrated = false;
  uint32_t sequence = 0;
  uint32_t lastEvent = 0;
  bool haveEvent = false;
  TickType_t next = xTaskGetTickCount();
  for (;;) {
    float sample[3];
    if (readAcceleration(sample)) {
      if (!calibrated) {
        memcpy(baseline, sample, sizeof(baseline));
        calibrated = true;
      }
      float squared = 0;
      for (int axis = 0; axis < 3; ++axis) {
        const float difference = sample[axis] - baseline[axis];
        squared += difference * difference;
        baseline[axis] += difference * 0.02f;
      }
      const float acceleration = sqrtf(squared);
      const uint32_t now = millis();
      if (parkingArmed.load() && acceleration >= SHOCK_THRESHOLD_G &&
          (!haveEvent || now - lastEvent >= EVENT_COOLDOWN_MS)) {
        ParkingEvent event = {++sequence, acceleration, nullptr, 0};
        if (cameraReady) {
          // Discard the buffered frame so the uploaded photo is taken after the shock.
          camera_fb_t *frame = esp_camera_fb_get();
          if (frame) esp_camera_fb_return(frame);
          frame = esp_camera_fb_get();
          if (frame) {
            if (frame->format == PIXFORMAT_JPEG && frame->len <= MAX_JPEG) {
              event.image = static_cast<uint8_t *>(ps_malloc(frame->len));
              if (event.image) {
                memcpy(event.image, frame->buf, frame->len);
                event.imageSize = frame->len;
              }
            }
            esp_camera_fb_return(frame);
          }
        }
        if (xQueueSend(events, &event, 0) != pdTRUE) {
          free(event.image);
          Serial.println("Parking queue full; event dropped");
        } else {
          Serial.println("Parking shock queued");
        }
        lastEvent = now;
        haveEvent = true;
      }
    } else {
      Serial.println("Accelerometer read failed");
    }
    vTaskDelayUntil(&next, pdMS_TO_TICKS(20));
  }
}

bool tlsSettings() {
  // Bind the CA to this TLS context and never ignore certificate dates.
  modem.sendAT("+CASSLCFG=0,\"crindex\",0");
  if (modem.waitResponse(5000L) != 1) return false;
  modem.sendAT("+CSSLCFG=\"ignorertctime\",0,0");
  if (modem.waitResponse(5000L) != 1) return false;
  return client.setCertificate(MODEM_CA_FILE);
}

bool writeAll(const String &payload) {
  size_t sent = 0;
  while (sent < payload.length()) {
    const size_t count = client.write(
        reinterpret_cast<const uint8_t *>(payload.c_str()) + sent,
        std::min(size_t(512), payload.length() - sent));
    if (!count) return false;
    sent += count;
  }
  return true;
}

bool post(const char *path, const JsonDocument &data, JsonDocument &response) {
  String body;
  serializeJson(data, body);
  if (!tlsSettings() || !client.connect(SERVER_HOST, SERVER_PORT, 30)) {
    Serial.println("Verified TLS connection failed; upload deferred");
    return false;
  }
  String headers = String("POST ") + path + " HTTP/1.1\r\nHost: " + SERVER_HOST +
                   "\r\nAuthorization: " + "Bearer" + " " + API_TOKEN +
                   "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                   body.length() + "\r\n\r\n";
  bool success = writeAll(headers) && writeAll(body);
  if (success) {
    client.setTimeout(15000);
    String status = client.readStringUntil('\n');
    int contentLength = -1;
    bool chunked = false;
    for (int count = 0; count < 32; ++count) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.isEmpty()) break;
      line.toLowerCase();
      if (line.startsWith("content-length:")) contentLength = line.substring(15).toInt();
      if (line.startsWith("transfer-encoding:")) chunked = true;
    }
    success = status.startsWith("HTTP/1.1 200 ") || status.startsWith("HTTP/1.0 200 ");
    if (success && !chunked && contentLength > 0 && contentLength <= 2048) {
      char buffer[2049];
      const size_t received = client.readBytes(buffer, contentLength);
      success = received == size_t(contentLength) &&
                !deserializeJson(response, buffer, received);
    } else {
      success = false;
    }
  }
  client.stop();
  return success;
}

void uploadEvent(const ParkingEvent &event, bool &success) {
  JsonDocument data;
  data["vehicle"] = VEHICLE_ID;
  char id[32];
  snprintf(id, sizeof(id), "%08lx-%lu",
           static_cast<unsigned long>(bootId), static_cast<unsigned long>(event.sequence));
  data["event_id"] = id;
  data["acceleration_g"] = event.acceleration;
  if (event.image) {
    const size_t capacity = 4 * ((event.imageSize + 2) / 3) + 1;
    auto encoded = static_cast<unsigned char *>(ps_malloc(capacity));
    if (!encoded) {
      success = false;
      return;
    }
    size_t length = 0;
    if (mbedtls_base64_encode(encoded, capacity, &length, event.image, event.imageSize) != 0) {
      free(encoded);
      success = false;
      return;
    }
    encoded[length] = 0;
    data["image_jpeg"] = reinterpret_cast<char *>(encoded);
    free(encoded);
  }
  JsonDocument response;
  success = !data.overflowed() && post("/api/events", data, response);
}

void setup() {
  Serial.begin(115200);
  if (strlen(API_TOKEN) < 32) fatal("Configure API_TOKEN in include/config.h");
  if (!psramFound()) fatal("PSRAM is required for parking photos");
  if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) fatal("PMU not found");
  pmu.disableTSPinMeasure();
  pmu.setBLDO1Voltage(3300);
  pmu.enableBLDO1();
  pmu.setDC3Voltage(3000);
  pmu.enableDC3();
  pmu.setBLDO2Voltage(3300);
  pmu.enableBLDO2();
  if (!initializeAccelerometer()) fatal("ADXL345 not found on SDA15/SCL7");
  cameraReady = initializeCamera();
  Serial.println(cameraReady ? "Camera ready" : "Camera unavailable; shocks will have no photo");
  preferences.begin("carpal", false);
  parkingArmed.store(preferences.getBool("armed", false));
  bootId = esp_random();
  events = xQueueCreate(3, sizeof(ParkingEvent));
  if (!events || xTaskCreate(monitorParking, "parking", 4096, nullptr, 1, nullptr) != pdPASS)
    fatal("Cannot start parking monitor");
  Serial1.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  pinMode(42, OUTPUT);
  digitalWrite(42, LOW);
  pinMode(MODEM_PWR, OUTPUT);
  digitalWrite(MODEM_PWR, LOW);
  if (!modem.testAT(1000)) {
    delay(100);
    digitalWrite(MODEM_PWR, HIGH);
    delay(1000);
    digitalWrite(MODEM_PWR, LOW);
    delay(3000);
  }
  if (!modem.init(SIM_PIN)) fatal("Modem initialization failed");
}

void loop() {
  const uint32_t start = millis();
  float latitude = 0, longitude = 0, speed = 0;
  bool fixed = false;
  uint32_t fixAt = 0;
  modem.gprsDisconnect();
  if (modem.enableGPS()) {
    const uint32_t gpsStart = millis();
    do {
      fixed = modem.getGPS(&latitude, &longitude, &speed);
      if (fixed) {
        fixAt = millis();
        break;
      }
      delay(1000);
    } while (millis() - gpsStart < GNSS_WINDOW_MS);
  }
  modem.disableGPS();
  if (modem.waitForNetwork(30000L) && modem.gprsConnect(APN, APN_USER, APN_PASSWORD)) {
    JsonDocument data;
    data["vehicle"] = VEHICLE_ID;
    if (fixed) {
      data["position"]["lat"] = latitude;
      data["position"]["lon"] = longitude;
      data["position"]["speed_kmh"] = speed;
      data["position"]["age_seconds"] = (millis() - fixAt) / 1000.0f;
    } else {
      data["position"] = nullptr;
    }
    JsonDocument response;
    if (post("/api/telemetry", data, response) && response["parking_armed"].is<bool>()) {
      const bool armed = response["parking_armed"];
      if (parkingArmed.exchange(armed) != armed) preferences.putBool("armed", armed);
      // Peek rather than remove: a failed upload is retried on the next cycle.
      ParkingEvent event;
      for (int count = 0; count < 3 && xQueuePeek(events, &event, 0) == pdTRUE; ++count) {
        bool success = false;
        uploadEvent(event, success);
        if (!success) break;
        xQueueReceive(events, &event, 0);
        free(event.image);
      }
    }
  } else {
    Serial.println("Cellular connection failed; queued events retained in RAM");
  }
  const uint32_t elapsed = millis() - start;
  if (elapsed < TELEMETRY_INTERVAL_MS) delay(TELEMETRY_INTERVAL_MS - elapsed);
}
