#pragma once

// Copy to config.h; that file is ignored by git.
constexpr char VEHICLE_ID[] = "car-1";
constexpr char APN[] = "your-carrier-apn";
constexpr char APN_USER[] = "";
constexpr char APN_PASSWORD[] = "";
constexpr char SIM_PIN[] = "";
constexpr char SERVER_HOST[] = "carpal.example.com";
constexpr uint16_t SERVER_PORT = 443;
constexpr char API_TOKEN[] = "";

// Filename of the CA certificate provisioned into the SIM7080 filesystem.
constexpr char MODEM_CA_FILE[] = "carpal-ca.pem";
constexpr uint8_t ACCEL_ADDRESS = 0x53;
constexpr float SHOCK_THRESHOLD_G = 0.35f;
constexpr uint32_t EVENT_COOLDOWN_MS = 30000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 10000;
constexpr uint32_t GNSS_WINDOW_MS = 15000;

// Official LILYGO camera connector only; verify your board revision and camera.
constexpr bool CAMERA_ENABLED = true;
