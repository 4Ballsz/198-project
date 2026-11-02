#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config.h"

inline Adafruit_SSD1306 g_oled(128, 64, &Wire, -1);
inline bool g_oledConnected = false;

namespace {
  uint32_t s_lastOledUpdate = 0;
  uint32_t s_lastOledProbe = 0;
}

inline void showOledCalibrating() {
  if (!g_oledConnected) return;
  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);
  g_oled.setTextSize(1);
  g_oled.setCursor(20, 6);
  g_oled.println(F("CALIBRATING..."));
  g_oled.drawFastHLine(0, 18, 128, SSD1306_WHITE);
  g_oled.setCursor(4, 23);
  g_oled.printf("WiFi: %s\n", WIFI_SSID);
  g_oled.setCursor(4, 35);
  if (WiFi.status() == WL_CONNECTED) {
    g_oled.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    g_oled.println(F("IP: Connecting..."));
  }
  g_oled.setCursor(4, 49);
  g_oled.println(F("Keep arm still..."));
  g_oled.display();
}

inline uint32_t g_showIpScreenUntilMs = 0;
inline uint32_t g_lastTouchVal = 0;
inline uint32_t g_touchBaseline = 0;
inline bool g_touchInitialized = false;

inline void triggerIpScreen(uint32_t durationMs = 4000) {
  g_showIpScreenUntilMs = millis() + durationMs;
}

inline bool checkTouchIpSensor() {
  uint32_t val = (uint32_t)touchRead(PIN_TOUCH_IP);
  g_lastTouchVal = val;

  if (!g_touchInitialized) {
    if (val > 0) {
      g_touchBaseline = val;
      g_touchInitialized = true;
    }
    return false;
  }

  if (val == 0) return false;

  // On ESP32-S3 (touch sensor v2), capacitance increases when touched
  uint32_t delta = (val > g_touchBaseline) ? (val - g_touchBaseline) : 0;
  uint32_t threshold = g_touchBaseline / 6; // ~16% delta threshold
  if (threshold < 5000) threshold = 5000;

  bool isTouched = (delta > threshold);

  if (!isTouched) {
    if (val < g_touchBaseline) {
      // Rapid downward adaptation if baseline was initialized while touched
      g_touchBaseline = (g_touchBaseline * 3 + val) / 4;
    } else {
      // Smoothly adapt baseline to ambient environmental drift
      g_touchBaseline = (g_touchBaseline * 31 + val) / 32;
    }
  }

  return isTouched;
}

inline void showOledIpInfo() {
  if (!g_oledConnected) return;
  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);

  if (WiFi.status() != WL_CONNECTED) {
    g_oled.setTextSize(1);
    g_oled.setCursor(22, 28);
    g_oled.print(F("Connecting..."));
    g_oled.display();
    return;
  }

  String ipStr = WiFi.localIP().toString();

  // If IP fits on a single line in big size 2 font (<= 10 chars, width <= 120px)
  if (ipStr.length() <= 10) {
    g_oled.setTextSize(2);
    int16_t x = (128 - (int16_t)(ipStr.length() * 12)) / 2;
    if (x < 0) x = 0;
    g_oled.setCursor(x, 24);
    g_oled.print(ipStr);
  } else {
    // For standard IPv4 (e.g. 10.227.255.235 or 192.168.1.100), split cleanly at the second dot
    // into two big size-2 centered lines without any "IP: " label
    int dotCount = 0;
    int splitIdx = -1;
    for (int i = 0; i < (int)ipStr.length(); i++) {
      if (ipStr[i] == '.') {
        dotCount++;
        if (dotCount == 2) {
          splitIdx = i; // keep second dot on the first line
          break;
        }
      }
    }

    String line1 = (splitIdx != -1) ? ipStr.substring(0, splitIdx + 1) : ipStr;
    String line2 = (splitIdx != -1) ? ipStr.substring(splitIdx + 1) : "";

    g_oled.setTextSize(2);

    int16_t x1 = (128 - (int16_t)(line1.length() * 12)) / 2;
    if (x1 < 0) x1 = 0;
    g_oled.setCursor(x1, 14);
    g_oled.print(line1);

    if (line2.length() > 0) {
      int16_t x2 = (128 - (int16_t)(line2.length() * 12)) / 2;
      if (x2 < 0) x2 = 0;
      g_oled.setCursor(x2, 36);
      g_oled.print(line2);
    }
  }

  g_oled.display();
}

inline void showOledBootProgress(const char* oneWordStatus) {
  if (!g_oledConnected) return;
  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);

  // Top header: "BOOTING..." centered (y = 8)
  g_oled.setTextSize(1);
  g_oled.setCursor(32, 8);
  g_oled.print(F("BOOTING..."));

  // Sleek divider line
  g_oled.drawFastHLine(14, 22, 100, SSD1306_WHITE);

  // One word showing what is currently booting up, centered
  size_t len = strlen(oneWordStatus);
  int textSize = (len <= 8) ? 2 : 1;
  int charWidth = (textSize == 2) ? 12 : 6;
  int16_t wordWidth = (int16_t)(len * charWidth);
  int16_t x = (128 - wordWidth) / 2;
  if (x < 0) x = 0;
  int16_t y = (textSize == 2) ? 36 : 38;

  g_oled.setTextSize(textSize);
  g_oled.setCursor(x, y);
  g_oled.print(oneWordStatus);

  g_oled.display();
}

inline bool initOled() {
  if (g_oledConnected) return true;
  // Try 0x3C (standard default) then 0x3D (alternate address)
  if (g_oled.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false)) {
    g_oledConnected = true;
    Serial.println(F("[OLED] SSD1306 128x64 display initialized at 0x3C!"));
  } else if (g_oled.begin(SSD1306_SWITCHCAPVCC, 0x3D, true, false)) {
    g_oledConnected = true;
    Serial.println(F("[OLED] SSD1306 128x64 display initialized at 0x3D!"));
  } else {
    g_oledConnected = false;
    Serial.println(F("[OLED] SSD1306 display not detected on I2C bus."));
    return false;
  }

  // Rotate screen 180 degrees per user request
  g_oled.setRotation(2);
  return true;
}

// Forward declarations of vitals accessors from pulse_ox.h
int getDisplayBpm();
int getDisplaySpo2();
float getCoreTemp();
const char* getPulseOxStatusStr();

inline void updateOledScreen() {
  uint32_t now = millis();

  // If display was not detected at boot, periodically re-probe every 4 seconds
  if (!g_oledConnected) {
    if (now - s_lastOledProbe >= 4000) {
      s_lastOledProbe = now;
      initOled();
    }
    return;
  }

  // Refresh display at ~10 Hz (every 100 ms)
  if (now - s_lastOledUpdate < 100) return;
  s_lastOledUpdate = now;

  // While calibrating on boot, keep calibration and IP screen active
  if (!isCalibrated) {
    showOledCalibrating();
    return;
  }

  // If Touch 11 was touched, display dedicated IP information screen
  if (millis() < g_showIpScreenUntilMs) {
    showOledIpInfo();
    return;
  }

  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);

  // -------------------------------------------------------------------------
  // ROW 1: Rep Counter & FSM State (y = 0..8)
  // -------------------------------------------------------------------------
  g_oled.setTextSize(1);
  g_oled.setCursor(0, 0);
  g_oled.printf("REPS: %u", (unsigned int)g_repCount);

  const char* stateStr = "IDLE";
  if (g_fsmState == STATE_CONCENTRIC) stateStr = "LIFT";
  else if (g_fsmState == STATE_PEAK)  stateStr = "PEAK";
  else if (g_fsmState == STATE_ECCENTRIC) stateStr = "DOWN";
  else if (g_fsmState == STATE_COMPLETED) stateStr = "DONE";

  // Right-align state badge
  int16_t stateX = 128 - (strlen(stateStr) * 6);
  g_oled.setCursor(stateX, 0);
  g_oled.print(stateStr);

  g_oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // -------------------------------------------------------------------------
  // ROW 2: Live Kinematics - Angle & Form Quality Q (y = 13..21)
  // -------------------------------------------------------------------------
  g_oled.setCursor(0, 13);
  g_oled.printf("Ang: %4.1f\xF7", g_kinematics.angleDeg);

  g_oled.setCursor(76, 13);
  g_oled.printf("Q: %3.0f%%", g_kinematics.liveQuality);

  g_oled.drawFastHLine(0, 23, 128, SSD1306_WHITE);

  // -------------------------------------------------------------------------
  // ROW 3: Vitals - Heart Rate & SpO2 (y = 26..34)
  // Only display O2 label when readings are ready to prevent text overlap
  // -------------------------------------------------------------------------
  int curBpm = getDisplayBpm();
  int curSpo2 = getDisplaySpo2();

  g_oled.setCursor(0, 26);
  if (curBpm > 0 && curSpo2 > 0) {
    g_oled.printf("HR: %3d bpm", curBpm);
    g_oled.setCursor(76, 26);
    g_oled.printf("O2: %2d%%", curSpo2);
  } else {
    // Full width status message when measuring or waiting for finger
    const char* oxStat = getPulseOxStatusStr();
    g_oled.printf("HR: %s", oxStat);
  }

  g_oled.drawFastHLine(0, 36, 128, SSD1306_WHITE);

  // -------------------------------------------------------------------------
  // ROW 4: Diagnostics - Core Temp & Last Rep Peak (y = 39..47)
  // -------------------------------------------------------------------------
  g_oled.setCursor(0, 39);
  g_oled.printf("Temp: %.1fC", getCoreTemp());

  if (g_lastCompletedRep.repNumber > 0) {
    g_oled.setCursor(76, 39);
    g_oled.printf("Pk: %2.0f\xF7", g_lastCompletedRep.peakAngleDeg);
  }

  // -------------------------------------------------------------------------
  // ROW 5: Real-Time Dynamic Curl Angle Gauge (y = 51..63)
  // Progress bar scales 0 deg -> 0%, 90 deg -> 100%
  // -------------------------------------------------------------------------
  g_oled.drawRect(0, 52, 128, 11, SSD1306_WHITE);
  int fillWidth = (int)((g_kinematics.angleDeg / 90.0f) * 122.0f);
  if (fillWidth > 122) fillWidth = 122;
  if (fillWidth < 0) fillWidth = 0;
  if (fillWidth > 0) {
    g_oled.fillRect(3, 55, fillWidth, 5, SSD1306_WHITE);
  }

  // Mark the 70 deg peak threshold on the gauge bar
  int peakMarkerX = 3 + (int)((70.0f / 90.0f) * 122.0f);
  g_oled.drawFastVLine(peakMarkerX, 52, 3, SSD1306_WHITE);
  g_oled.drawFastVLine(peakMarkerX, 60, 3, SSD1306_WHITE);

  g_oled.display();
}
