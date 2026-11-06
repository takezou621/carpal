// bench_scan.ino — 近隣の 2.4GHz WiFi をスキャンして表示する診断用スケッチ
#include <Arduino.h>
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  setenv("TZ", "UTC", 1);
  delay(2000);
  Serial.println("\n[SCAN] WiFi scan start...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  int n = WiFi.scanNetworks();
  Serial.printf("[SCAN] %d networks found:\n", n);
  for (int i = 0; i < n; i++) {
    Serial.printf("  SSID=\"%s\"  RSSI=%d dBm  ch=%d %s\n",
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "(open)" : "");
  }
  Serial.println("[SCAN] done. Reset to scan again.");
}

void loop() { delay(1000); }
