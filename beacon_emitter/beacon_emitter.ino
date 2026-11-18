#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <esp32-hal-rgb-led.h>
#include <Preferences.h>

// Devboard pinout (matches generic ESP32-S3 devboard in project)
constexpr int PIN_NEOPIXEL = 48;

// Available workout beacons
struct BeaconProfile {
  uint8_t exerciseId;
  const char* beaconName;
  const char* exerciseName;
  uint8_t r, g, b;
  const char* hexColor;
};

const BeaconProfile BEACON_PROFILES[] = {
  { 0, "ACT-CURL",  "Bicep Curl",     0,   180, 255, "#00b4ff" }, // 0: Neon Cyan / Blue
  { 1, "ACT-RAISE", "Lateral Raise",  180, 0,   255, "#b400ff" }, // 1: Vibrant Purple
  { 2, "ACT-SQUAT", "Squat",          0,   255, 80,  "#00ff50" }, // 2: Emerald Green (extensible)
};
constexpr size_t NUM_BEACONS = sizeof(BEACON_PROFILES) / sizeof(BEACON_PROFILES[0]);

// Default beacon profile if not specified:
// 0 = Bicep Curl (Neon Cyan / Blue)
// 1 = Lateral Raise (Vibrant Purple)
// 2 = Squat (Emerald Green)
#if !defined(DEFAULT_BEACON_INDEX) && !defined(FORCE_BEACON_INDEX)
#define DEFAULT_BEACON_INDEX 0
#endif

// State
int g_currentBeaconIndex = 0;
Preferences g_prefs;
BLEAdvertising* g_pAdvertising = nullptr;

void updateNeoPixel() {
  const BeaconProfile& p = BEACON_PROFILES[g_currentBeaconIndex];
  rgbLedWriteOrdered(PIN_NEOPIXEL, LED_COLOR_ORDER_RGB, p.r, p.g, p.b);
}

void configureAndStartAdvertising() {
  const BeaconProfile& p = BEACON_PROFILES[g_currentBeaconIndex];

  if (g_pAdvertising != nullptr) {
    g_pAdvertising->stop();
  }

  // Set device name
  BLEDevice::init(p.beaconName);
  g_pAdvertising = BLEDevice::getAdvertising();

  BLEAdvertisementData advData;
  advData.setFlags(0x06); // General Discoverable + BR/EDR not supported
  advData.setName(p.beaconName);

  // Manufacturer Data: [0x19, 0x80] (Company ID) + [0xAC] + [exerciseId] + [R, G, B] + [TxPower]
  uint8_t mfgBytes[8];
  mfgBytes[0] = 0x19;
  mfgBytes[1] = 0x80;
  mfgBytes[2] = 0xAC; // Magic marker: AC = Activity Beacon
  mfgBytes[3] = p.exerciseId;
  mfgBytes[4] = p.r;
  mfgBytes[5] = p.g;
  mfgBytes[6] = p.b;
  mfgBytes[7] = (uint8_t)(-59); // Calibrated RSSI at 1m

  String mfgDataStr((char*)mfgBytes, sizeof(mfgBytes));
  advData.setManufacturerData(mfgDataStr);

  g_pAdvertising->setAdvertisementData(advData);
  g_pAdvertising->setMinInterval(160); // 100 ms (160 * 0.625ms)
  g_pAdvertising->setMaxInterval(240); // 150 ms
  g_pAdvertising->start();

  Serial.println();
  Serial.println("==================================================");
  Serial.printf("  BLE WORKOUT BEACON EMITTER ACTIVE\r\n");
  Serial.println("==================================================");
  Serial.printf("  Beacon Index:   %d\r\n", g_currentBeaconIndex);
  Serial.printf("  Beacon Name:    %s\r\n", p.beaconName);
  Serial.printf("  Exercise:       %s\r\n", p.exerciseName);
  Serial.printf("  RGB Color:      (%u, %u, %u) -> %s\r\n", p.r, p.g, p.b, p.hexColor);
  Serial.printf("  NeoPixel Pin:   GPIO %d\r\n", PIN_NEOPIXEL);
  Serial.printf("  MAC Address:    %s\r\n", BLEDevice::getAddress().toString().c_str());
  Serial.println("==================================================");
  Serial.println("Type '0' for Curl, '1' for Lateral Raise, '2' for Squat");
  Serial.println("==================================================");
}

void setBeaconProfile(int index) {
  if (index < 0 || index >= (int)NUM_BEACONS) return;
  g_currentBeaconIndex = index;
  g_prefs.putInt("beacon_idx", g_currentBeaconIndex);
  updateNeoPixel();
  configureAndStartAdvertising();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  g_prefs.begin("ble_beacon", false);

#ifdef FORCE_BEACON_INDEX
  g_currentBeaconIndex = FORCE_BEACON_INDEX;
  if (g_currentBeaconIndex < 0 || g_currentBeaconIndex >= (int)NUM_BEACONS) {
    g_currentBeaconIndex = 0;
  }
  g_prefs.putInt("beacon_idx", g_currentBeaconIndex);
#else
  g_currentBeaconIndex = g_prefs.getInt("beacon_idx", DEFAULT_BEACON_INDEX);
  if (g_currentBeaconIndex < 0 || g_currentBeaconIndex >= (int)NUM_BEACONS) {
    g_currentBeaconIndex = DEFAULT_BEACON_INDEX;
  }
#endif

  updateNeoPixel();
  configureAndStartAdvertising();
}

void loop() {
  // Handle serial commands to quickly switch beacon personality
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '0' || c == 'c' || c == 'C') {
      Serial.println("\r\n[CMD] Switching beacon to 0: Bicep Curl");
      setBeaconProfile(0);
    } else if (c == '1' || c == 'r' || c == 'R') {
      Serial.println("\r\n[CMD] Switching beacon to 1: Lateral Raise");
      setBeaconProfile(1);
    } else if (c == '2' || c == 's' || c == 'S') {
      Serial.println("\r\n[CMD] Switching beacon to 2: Squat");
      setBeaconProfile(2);
    } else if (c == 'h' || c == '?') {
      Serial.println("\r\nCommands: '0'=Bicep Curl (Cyan), '1'=Lateral Raise (Purple), '2'=Squat (Green)");
    }
  }

  // Keep NeoPixel solid in case of any external reset
  static uint32_t lastLedRefresh = 0;
  if (millis() - lastLedRefresh >= 1000) {
    lastLedRefresh = millis();
    updateNeoPixel();
  }

  delay(20);
}
