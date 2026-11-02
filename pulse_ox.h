#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include "config.h"

// Global pulse oximeter state
inline PulseOxState g_pulseOx = {
  .connected = false,
  .fingerPresent = false,
  .haveBpm = false,
  .haveSpo2 = false,
  .latestBpmValid = false,
  .latestSpo2Valid = false,
  .bpm = 0.0f,
  .spo2 = 0.0f,
  .lastBpmTime = 0,
  .lastSpo2Time = 0,
  .lastFingerTime = 0,
  .rawIR = 0,
  .rawRed = 0
};

inline volatile float g_coreTemp = 25.0f;
inline float getCoreTemp() { return g_coreTemp; }

inline MAX30105 g_pulseSensor;

inline TaskHandle_t g_pulseOxTaskHandle = NULL;

namespace {
  uint32_t s_irBuffer[PULSE_OX_WINDOW];
  uint32_t s_redBuffer[PULSE_OX_WINDOW];
  int s_pulseOxSamples = 0;
  volatile bool s_sheetsTaskRunning = false;
  uint32_t s_lastSheetsPostMs = 0;
}

struct SheetsPayload {
  int bpm;
  int spo2;
};

inline void sheetsTask(void* param) {
  SheetsPayload* p = (SheetsPayload*)param;
  if (ENABLE_GOOGLE_SHEETS_LOGGING && WiFi.status() == WL_CONNECTED && GOOGLE_SHEETS_URL && strlen(GOOGLE_SHEETS_URL) > 0) {
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(8000);
    if (http.begin(GOOGLE_SHEETS_URL)) {
      http.addHeader("Content-Type", "application/json");

      String jsonPayload = "{\"bpm\":" + String(p->bpm) + ",\"spo2\":" + String(p->spo2) + "}";
      int httpResponseCode = http.POST(jsonPayload);
      if (httpResponseCode > 0) {
        Serial.printf("[SHEETS] Vitals uploaded (BPM: %d, SpO2: %d%%). HTTP: %d\r\n", p->bpm, p->spo2, httpResponseCode);
      } else {
        Serial.printf("[SHEETS] Error uploading vitals. Code: %d (%s)\r\n", httpResponseCode, http.errorToString(httpResponseCode).c_str());
      }
      http.end();
    } else {
      Serial.println(F("[SHEETS] HTTPClient.begin failed to initialize."));
    }
  }
  delete p;
  s_lastSheetsPostMs = millis();
  s_sheetsTaskRunning = false;
  vTaskDelete(NULL);
}

inline void triggerGoogleSheetsPost(int bpm, int spo2) {
  if (!ENABLE_GOOGLE_SHEETS_LOGGING) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!GOOGLE_SHEETS_URL || strlen(GOOGLE_SHEETS_URL) == 0) return;

  uint32_t now = millis();
  if (s_sheetsTaskRunning || (now - s_lastSheetsPostMs < GOOGLE_SHEETS_INTERVAL_MS)) {
    return;
  }

  s_sheetsTaskRunning = true;
  s_lastSheetsPostMs = now;
  SheetsPayload* p = new SheetsPayload{bpm, spo2};
  // Allocate 12KB stack for sheets_task to support mbedTLS / HTTPS handshake and redirect buffers
  BaseType_t res = xTaskCreatePinnedToCore(sheetsTask, "sheets_task", 12288, p, 1, NULL, 0);
  if (res != pdPASS) {
    delete p;
    s_sheetsTaskRunning = false;
    Serial.println(F("[SHEETS] Failed to spawn sheets_task (insufficient heap)!"));
  }
}

inline void resetPulseOxReadings() {
  s_pulseOxSamples = 0;
  g_pulseOx.haveBpm = false;
  g_pulseOx.haveSpo2 = false;
  g_pulseOx.latestBpmValid = false;
  g_pulseOx.latestSpo2Valid = false;
}

inline void calculatePulseOxReadings() {
  int32_t rawBpm = 0;
  int32_t rawSpo2 = 0;
  int8_t bpmValid = 0;
  int8_t spo2Valid = 0;

  maxim_heart_rate_and_oxygen_saturation(
    s_irBuffer, PULSE_OX_WINDOW, s_redBuffer,
    &rawSpo2, &spo2Valid,
    &rawBpm, &bpmValid
  );

  uint32_t irMin = s_irBuffer[0];
  uint32_t irMax = s_irBuffer[0];
  bool clipped = false;

  for (int i = 0; i < PULSE_OX_WINDOW; i++) {
    if (s_irBuffer[i] < irMin) irMin = s_irBuffer[i];
    if (s_irBuffer[i] > irMax) irMax = s_irBuffer[i];

    if (s_irBuffer[i] >= 260000 || s_redBuffer[i] >= 260000) {
      clipped = true;
    }
  }

  g_pulseOx.latestBpmValid = (bpmValid != 0) && (rawBpm > 0) && !clipped;
  g_pulseOx.latestSpo2Valid = (spo2Valid != 0) && (rawSpo2 > 0) && (rawSpo2 <= 100) && !clipped;

  uint32_t now = millis();

  // Smooth accepted readings, restarting after a long gap
  if (g_pulseOx.latestBpmValid) {
    if (!g_pulseOx.haveBpm || (now - g_pulseOx.lastBpmTime >= PULSE_OX_KEEP_LAST_MS)) {
      g_pulseOx.bpm = (float)rawBpm;
    } else {
      g_pulseOx.bpm = 0.7f * g_pulseOx.bpm + 0.3f * (float)rawBpm;
    }
    g_pulseOx.haveBpm = true;
    g_pulseOx.lastBpmTime = now;
  }

  if (g_pulseOx.latestSpo2Valid) {
    if (!g_pulseOx.haveSpo2 || (now - g_pulseOx.lastSpo2Time >= PULSE_OX_KEEP_LAST_MS)) {
      g_pulseOx.spo2 = (float)rawSpo2;
    } else {
      g_pulseOx.spo2 = 0.7f * g_pulseOx.spo2 + 0.3f * (float)rawSpo2;
    }
    g_pulseOx.haveSpo2 = true;
    g_pulseOx.lastSpo2Time = now;
  }

  if (g_pulseOx.latestBpmValid && g_pulseOx.latestSpo2Valid) {
    triggerGoogleSheetsPost((int)(g_pulseOx.bpm + 0.5f), (int)(g_pulseOx.spo2 + 0.5f));
  }

  // Slide buffer: retain 3 seconds (75 samples) and collect 1 second (25 samples) of new data
  for (int i = PULSE_OX_STEP; i < PULSE_OX_WINDOW; i++) {
    s_irBuffer[i - PULSE_OX_STEP] = s_irBuffer[i];
    s_redBuffer[i - PULSE_OX_STEP] = s_redBuffer[i];
  }
  s_pulseOxSamples = PULSE_OX_WINDOW - PULSE_OX_STEP;
}

bool initOled();
void updateOledScreen();

inline void pulseOxTask(void* pvParameters) {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setTimeOut(10); // 10ms short timeout to prevent bus blocking
  initOled();

  uint32_t lastTempRead = 0;

  while (true) {
    uint32_t now = millis();

    // Read chip temperature on Core 0 ONLY when arm is idle, at most once every 5 seconds
    // to strictly prevent SAR ADC contention with Core 1 accelerometer sampling
    if ((now - lastTempRead >= 5000) && (g_fsmState == STATE_IDLE)) {
      lastTempRead = now;
      float t = temperatureRead();
      if (!isnan(t)) {
        g_coreTemp = t;
      }
    }

    if (!g_pulseOx.connected) {
      // First try configured pins (SDA=3, SCL=4)
      Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
      Wire.setTimeOut(10);
      Wire.beginTransmission(0x57);
      byte error = Wire.endTransmission();
      if (error == 0 && g_pulseSensor.begin(Wire, I2C_SPEED_FAST)) {
        g_pulseSensor.setup(60, 4, 2, 100, 411, 16384);
        g_pulseSensor.clearFIFO();
        g_pulseOx.connected = true;
        Serial.println(F("[PULSE OX] MAX30105 sensor connected and initialized (pins 3 & 4)!"));
      } else {
        // Fallback to reference pins (SDA=1, SCL=2)
        Wire.begin(1, 2);
        Wire.setTimeOut(10);
        Wire.beginTransmission(0x57);
        error = Wire.endTransmission();
        if (error == 0 && g_pulseSensor.begin(Wire, I2C_SPEED_FAST)) {
          g_pulseSensor.setup(60, 4, 2, 100, 411, 16384);
          g_pulseSensor.clearFIFO();
          g_pulseOx.connected = true;
          Serial.println(F("[PULSE OX] MAX30105 sensor connected and initialized (pins 1 & 2)!"));
        }
      }
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    g_pulseSensor.check();

    while (g_pulseSensor.available()) {
      uint32_t infrared = g_pulseSensor.getFIFOIR();
      uint32_t redLight = g_pulseSensor.getFIFORed();
      g_pulseSensor.nextSample();

      g_pulseOx.rawIR = infrared;
      g_pulseOx.rawRed = redLight;

      if (infrared < PULSE_OX_FINGER_THRESHOLD) {
        s_pulseOxSamples = 0;
        g_pulseOx.latestBpmValid = false;
        g_pulseOx.latestSpo2Valid = false;

        if (g_pulseOx.fingerPresent && (now - g_pulseOx.lastFingerTime > 500)) {
          g_pulseOx.fingerPresent = false;
          resetPulseOxReadings();
        }
        continue;
      }

      if (!g_pulseOx.fingerPresent) {
        resetPulseOxReadings();
        g_pulseOx.fingerPresent = true;
      }

      g_pulseOx.lastFingerTime = now;

      if (s_pulseOxSamples < PULSE_OX_WINDOW) {
        s_irBuffer[s_pulseOxSamples] = infrared;
        s_redBuffer[s_pulseOxSamples] = redLight;
        s_pulseOxSamples++;

        if (s_pulseOxSamples == PULSE_OX_WINDOW) {
          calculatePulseOxReadings();
        }
      }
    }

    updateOledScreen();
    vTaskDelay(pdMS_TO_TICKS(40)); // 25Hz poll rate for sensor FIFO
  }
}

inline bool initPulseOx() {
  // Launch background task on Core 0 so it never interferes with Core 1 rep kinematics
  if (g_pulseOxTaskHandle == NULL) {
    xTaskCreatePinnedToCore(
      pulseOxTask,
      "pulse_ox_task",
      8192,
      NULL,
      1,
      &g_pulseOxTaskHandle,
      0
    );
  }
  return true;
}

inline void updatePulseOx() {
  // Handled fully asynchronously by pulseOxTask on Core 0!
}

inline int getDisplayBpm() {
  uint32_t now = millis();
  if (g_pulseOx.haveBpm && (now - g_pulseOx.lastBpmTime < PULSE_OX_KEEP_LAST_MS)) {
    return (int)(g_pulseOx.bpm + 0.5f);
  }
  return 0;
}

inline int getDisplaySpo2() {
  uint32_t now = millis();
  if (g_pulseOx.haveSpo2 && (now - g_pulseOx.lastSpo2Time < PULSE_OX_KEEP_LAST_MS)) {
    return (int)(g_pulseOx.spo2 + 0.5f);
  }
  return 0;
}

inline const char* getPulseOxStatusStr() {
  if (!g_pulseOx.connected) return "Disconnected";
  if (!g_pulseOx.fingerPresent) return "Place finger";
  int bpm = getDisplayBpm();
  int spo2 = getDisplaySpo2();
  if (bpm <= 0 && spo2 <= 0) return "Measuring...";
  return "OK";
}

#include "oled_display.h"
