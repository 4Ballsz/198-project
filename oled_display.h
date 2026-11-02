#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config.h"

// Display instance
inline Adafruit_SSD1306 g_oled(128, 64, &Wire, -1);
inline bool g_oledConnected = false;

namespace {
  uint32_t s_lastOledUpdate = 0;
  uint32_t s_lastOledProbe = 0;
}

// Calibration screen
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

// Touch IP trigger
inline uint32_t g_showIpScreenUntilMs = 0;
inline uint32_t g_lastTouchVal = 0;
inline uint32_t g_touchBaseline = 0;
inline bool g_touchInitialized = false;

inline void triggerIpScreen(uint32_t durationMs = 4000) {
  g_showIpScreenUntilMs = millis() + durationMs;
}

// Touch sensor check
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

  uint32_t delta = (val > g_touchBaseline) ? (val - g_touchBaseline) : 0;
  uint32_t threshold = g_touchBaseline / 6;
  if (threshold < 5000) threshold = 5000;

  bool isTouched = (delta > threshold);

  if (!isTouched) {
    if (val < g_touchBaseline) {
      g_touchBaseline = (g_touchBaseline * 3 + val) / 4;
    } else {
      g_touchBaseline = (g_touchBaseline * 31 + val) / 32;
    }
  }

  return isTouched;
}

// IP info screen
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

  if (ipStr.length() <= 10) {
    g_oled.setTextSize(2);
    int16_t x = (128 - (int16_t)(ipStr.length() * 12)) / 2;
    if (x < 0) x = 0;
    g_oled.setCursor(x, 24);
    g_oled.print(ipStr);
  } else {
    int dotCount = 0;
    int splitIdx = -1;
    for (int i = 0; i < (int)ipStr.length(); i++) {
      if (ipStr[i] == '.') {
        dotCount++;
        if (dotCount == 2) {
          splitIdx = i;
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

// Boot progress screen
inline void showOledBootProgress(const char* oneWordStatus) {
  if (!g_oledConnected) return;
  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);

  g_oled.setTextSize(1);
  g_oled.setCursor(32, 8);
  g_oled.print(F("BOOTING..."));

  g_oled.drawFastHLine(14, 22, 100, SSD1306_WHITE);

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

// OLED initialization
inline bool initOled() {
  if (g_oledConnected) return true;
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

  g_oled.setRotation(2);
  return true;
}

// Vitals forward declarations
int getDisplayBpm();
int getDisplaySpo2();
float getCoreTemp();
const char* getPulseOxStatusStr();

// Screen update
inline void updateOledScreen() {
  uint32_t now = millis();

  if (!g_oledConnected) {
    if (now - s_lastOledProbe >= 4000) {
      s_lastOledProbe = now;
      initOled();
    }
    return;
  }

  if (now - s_lastOledUpdate < 100) return;
  s_lastOledUpdate = now;

  if (!isCalibrated) {
    showOledCalibrating();
    return;
  }

  if (millis() < g_showIpScreenUntilMs) {
    showOledIpInfo();
    return;
  }

  g_oled.clearDisplay();
  g_oled.setTextColor(SSD1306_WHITE);

  // Rep counter & state
  g_oled.setTextSize(1);
  g_oled.setCursor(0, 0);
  g_oled.printf("REPS: %u", (unsigned int)g_repCount);

  const char* stateStr = "IDLE";
  if (g_fsmState == STATE_CONCENTRIC) stateStr = "LIFT";
  else if (g_fsmState == STATE_PEAK)  stateStr = "PEAK";
  else if (g_fsmState == STATE_ECCENTRIC) stateStr = "DOWN";
  else if (g_fsmState == STATE_COMPLETED) stateStr = "DONE";

  int16_t stateX = 128 - (strlen(stateStr) * 6);
  g_oled.setCursor(stateX, 0);
  g_oled.print(stateStr);

  g_oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // Live kinematics
  g_oled.setCursor(0, 13);
  g_oled.printf("Ang: %4.1f\xF7", g_kinematics.angleDeg);

  g_oled.setCursor(76, 13);
  g_oled.printf("Q: %3.0f%%", g_kinematics.liveQuality);

  g_oled.drawFastHLine(0, 23, 128, SSD1306_WHITE);

  // Vitals
  int curBpm = getDisplayBpm();
  int curSpo2 = getDisplaySpo2();

  g_oled.setCursor(0, 26);
  if (curBpm > 0 && curSpo2 > 0) {
    g_oled.printf("HR: %3d bpm", curBpm);
    g_oled.setCursor(76, 26);
    g_oled.printf("O2: %2d%%", curSpo2);
  } else {
    const char* oxStat = getPulseOxStatusStr();
    g_oled.printf("HR: %s", oxStat);
  }

  g_oled.drawFastHLine(0, 36, 128, SSD1306_WHITE);

  // Diagnostics
  g_oled.setCursor(0, 39);
  g_oled.printf("Temp: %.1fC", getCoreTemp());

  if (g_lastCompletedRep.repNumber > 0) {
    g_oled.setCursor(76, 39);
    g_oled.printf("Pk: %2.0f\xF7", g_lastCompletedRep.peakAngleDeg);
  }

  // Curl angle gauge
  g_oled.drawRect(0, 52, 128, 11, SSD1306_WHITE);
  int fillWidth = (int)((g_kinematics.angleDeg / 90.0f) * 122.0f);
  if (fillWidth > 122) fillWidth = 122;
  if (fillWidth < 0) fillWidth = 0;
  if (fillWidth > 0) {
    g_oled.fillRect(3, 55, fillWidth, 5, SSD1306_WHITE);
  }

  int peakMarkerX = 3 + (int)((70.0f / 90.0f) * 122.0f);
  g_oled.drawFastVLine(peakMarkerX, 52, 3, SSD1306_WHITE);
  g_oled.drawFastVLine(peakMarkerX, 60, 3, SSD1306_WHITE);

  g_oled.display();
}
