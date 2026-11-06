// I2C 識別診断 v3: 0x34 の正体 + 一般コールリセット
#include <Arduino.h>
#include <Wire.h>

uint8_t readReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  Wire.requestFrom(addr, (uint8_t)1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n[DIAG3] start");
  Wire.begin(15, 7);

  Serial.println("[DIAG3] 0x34 regs 0x00-0x06:");
  for (uint8_t r = 0; r <= 6; r++) {
    Serial.printf("  reg 0x%02X = 0x%02X\n", r, readReg(0x34, r));
  }
  Serial.println("[DIAG3] 0x5A reg 0x03 (chip id):");
  Serial.printf("  = 0x%02X\n", readReg(0x5A, 0x03));

  // 一般コール・リセット (0x00 addr, 2nd byte 0x06)
  Serial.println("[DIAG3] sending general call RESET (0x00/0x06)...");
  Wire.beginTransmission(0x00);
  Wire.write(0x06);
  Serial.printf("  gc result: %s\n", Wire.endTransmission() == 0 ? "sent" : "NACK (応答デバイスなし)");
  delay(100);
  Wire.beginTransmission(0x00);
  Wire.write(0x00);
  Serial.printf("  gc2 result: %s\n", Wire.endTransmission() == 0 ? "sent" : "NACK");
  delay(200);

  Serial.printf("[DIAG3] after: 0x5A reg03=0x%02X  0x34 reg03=0x%02X\n",
                readReg(0x5A, 0x03), readReg(0x34, 0x03));
  Wire.beginTransmission(0x5A);
  Serial.printf("[DIAG3] 0x5A probe: %s\n", Wire.endTransmission() == 0 ? "ACK (復帰!)" : "NACK");
}

void loop() {
  Wire.beginTransmission(0x5A);
  Serial.printf("[DIAG3] 0x5A: %s\n", Wire.endTransmission() == 0 ? "ACK" : "NACK");
  delay(4000);
}
