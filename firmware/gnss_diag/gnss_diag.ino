// gnss_diag.ino — GNSS の衛星捕捉状況を AT+SGNSSSTATE で確認する診断用スケッチ
// StreamDebugger で全AT通信をシリアルにミラーする
#include <Arduino.h>
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
#define DUMP_AT_COMMANDS
#define TINY_GSM_MODEM_SIM7080
#define TINY_GSM_RX_BUFFER 1024
#include <TinyGsmClient.h>
#include <StreamDebugger.h>

XPowersPMU PMU;
StreamDebugger debugger(Serial1, Serial);
TinyGsm modem(debugger);

void setup() {
  Serial.begin(115200);
  setenv("TZ", "UTC", 1);
  delay(2500);

  // PMU 初期化 (モデム DC3 + GPSアンテナ BLDO2)
  if (!PMU.begin(Wire, 0x5A, 15, 7)) {
    Serial.println("[PMU] init FAILED");
    while (true) delay(5000);
  }
  PMU.disableDC3(); delay(200);
  PMU.setDC3Voltage(3000); PMU.enableDC3();
  PMU.setBLDO2Voltage(3300); PMU.enableBLDO2();
  PMU.disableTSPinMeasure();
  Serial.println("[PMU] OK");

  // モデム起動 (PWRKEY 41)
  Serial1.begin(115200, SERIAL_8N1, 4, 5);
  pinMode(41, OUTPUT);
  digitalWrite(41, LOW); delay(100);
  digitalWrite(41, HIGH); delay(1000);
  digitalWrite(41, LOW);
  while (!modem.testAT(1000)) { Serial.print("."); delay(500); }
  Serial.println("\n[MODEM] OK");

  if (!modem.enableGPS()) {
    Serial.println("[GNSS] enable failed");
  }
  // GPS+BEIDOU
  modem.sendAT("+CGNSMOD=1,0,1,0,0");
  modem.waitResponse();
  Serial.println("[DIAG] GNSS enabled. Polling SGNSSSTATE every 15s...");
}

void loop() {
  Serial.println("\n[DIAG] --- AT+SGNSSSTATE? ---");
  modem.sendAT("+SGNSSSTATE?");
  // レスポンスを最大8秒待って全行吐き出す (StreamDebuggerがミラー済みだが明示も行う)
  String r = modem.stream.readStringUntil('\n');
  modem.waitResponse(8000);
  Serial.println("[DIAG] ----------------------");
  delay(15000);
}
