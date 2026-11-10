// ltem_diag.ino — LTE 診断 v3: COPS=? の応答を連続読み取りで取りこぼさず取得する
#include <Arduino.h>
#define TINY_GSM_MODEM_SIM7080
#define TINY_GSM_RX_BUFFER 1024
#include <TinyGsmClient.h>
#define SerialAT Serial1

#define BOARD_MODEM_PWR_PIN   41
#define BOARD_MODEM_DTR_PIN   42
#define BOARD_MODEM_RXD_PIN   4
#define BOARD_MODEM_TXD_PIN   5
#define I2C_SDA               15
#define I2C_SCL               7

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
XPowersPMU PMU;
TinyGsm modem(SerialAT);

uint8_t probePmuAddress() {
  Wire.begin(I2C_SDA, I2C_SCL);
  const uint8_t candidates[] = {0x5A, 0x34};
  for (uint8_t addr : candidates) {
    Wire.beginTransmission(addr);
    Wire.write(0x03);
    if (Wire.endTransmission(false) == 0) {
      Wire.requestFrom(addr, (uint8_t)1);
      if (Wire.available() && Wire.read() == 0x4A) return addr;
    }
  }
  return 0;
}

bool pmuInit() {
  pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
  digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
  uint8_t addr = probePmuAddress();
  if (addr == 0) return false;
  if (!PMU.begin(Wire, addr, I2C_SDA, I2C_SCL)) return false;
  PMU.disableDC3();
  delay(1500);
  PMU.setDC3Voltage(3000);
  PMU.enableDC3();
  delay(500);
  PMU.setBLDO2Voltage(3300);
  PMU.enableBLDO2();
  PMU.disableTSPinMeasure();
  return true;
}

bool modemPowerOn() {
  Serial1.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);
  pinMode(BOARD_MODEM_DTR_PIN, OUTPUT);
  digitalWrite(BOARD_MODEM_DTR_PIN, LOW);
  int retry = 0;
  while (!modem.testAT(1000)) {
    Serial.print(".");
    if (retry++ > 8) {
      Serial.println("\n[DIAG] manual power key...");
      digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
      delay(2000);
      digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
      retry = 0;
    }
    delay(500);
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println("\n[DIAG v3] continuous-read COPS=? scan (max 300s)");
  if (!pmuInit()) {
    Serial.println("[PMU] init FAILED — halting");
    while (true) delay(10000);
  }
  Serial.println("[PMU] OK");
  modemPowerOn();
  Serial.print("[DIAG] CNMP=");  SerialAT.println("AT+CNMP?"); pumpUntil(2000, false);
  Serial.print("[DIAG] CMNB=");  SerialAT.println("AT+CMNB?"); pumpUntil(2000, false);
  Serial.println("[DIAG] issuing AT+COPS=? — reading continuously, do not disconnect");
  SerialAT.println("AT+COPS=?");
  pumpUntil(300000, true);       // 連続読み (フラッシュしない)
  Serial.println("\n[DIAG] scan window ended");
  Serial.println("[DIAG] polling CSQ/CEREG every 20s");
}

// SerialAT からの受信をすべて Serial へ流す。stopOnOk=true で最後の OK/ERROR を検出
static bool lastEnded = false;
void pumpUntil(uint32_t ms, bool stopOnOk) {
  uint32_t t0 = millis();
  lastEnded = false;
  while (millis() - t0 < ms) {
    while (SerialAT.available()) {
      char c = (char)SerialAT.read();
      if (c == '\r') continue;
      Serial.write(c);
      // 簡易終端検出: 行バッファで OK / ERROR を見る
      static String line = "";
      if (c == '\n') {
        line.trim();
        if (stopOnOk && (line == "OK" || line == "ERROR")) lastEnded = true;
        line = "";
      } else {
        line += c;
      }
    }
    if (stopOnOk && lastEnded) return;
    delay(5);
  }
}

void loop() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 20000) { delay(200); return; }
  lastPoll = millis();
  Serial.println("\n[DIAG] ===== status =====");
  SerialAT.println("AT+CSQ");   pumpUntil(1500, false);
  SerialAT.println("AT+CEREG?"); pumpUntil(1500, false);
  SerialAT.println("AT+CGATT?"); pumpUntil(1500, false);
}
