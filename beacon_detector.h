#pragma once

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

// Forward declarations
void broadcastWsData(bool forceHistory = false);
void resetPressTracking();
int getDisplayBpm();
int getDisplaySpo2();

// Beacon detection constants
constexpr uint32_t BEACON_TIMEOUT_MS = 7000;      // 7 seconds without packet = beacon lost (was 4000)
constexpr int MIN_BEACON_RSSI = -84;               // Minimum RSSI to consider in-range (was -82)
constexpr float HYSTERESIS_RSSI_DB = 7.0f;         // 7 dB advantage needed to switch active beacons (was 4.0)
constexpr float RSSI_EMA_ALPHA = 0.30f;            // RSSI smoothing factor

struct ObservedBeacon {
  uint8_t exerciseId;
  char beaconName[16];
  char exerciseName[24];
  uint8_t r, g, b;
  char hexColor[10];
  int lastRawRssi;
  float filteredRssi;
  uint32_t lastSeenMs;
  char mac[18];
};

namespace {
  constexpr size_t MAX_OBSERVED_BEACONS = 8;
  ObservedBeacon s_observedBeacons[MAX_OBSERVED_BEACONS];
  size_t s_observedBeaconCount = 0;
  SemaphoreHandle_t s_beaconMutex = NULL;
  TaskHandle_t s_bleScanTaskHandle = NULL;
}

// Finalize the current set and prepare for a new exercise
inline void finalizeCurrentSet(ExerciseType nextExercise) {
  uint32_t reps = g_repCount.load();
  if (reps > 0) {
    SetRecord rec = {0};
    rec.setNumber = g_currentSetNumber;
    rec.exercise = g_exercise;
    rec.repCount = reps;

    // Calculate average quality of reps for this completed set
    float qSum = 0.0f;
    uint32_t qCount = 0;
    int bpmSum = 0, spo2Sum = 0, vitalsCount = 0;

    for (size_t i = 0; i < g_repHistoryCount; i++) {
      if (g_repHistory[i].exercise == g_exercise) {
        qSum += g_repHistory[i].finalQualityFactor;
        qCount++;
        if (g_repHistory[i].avgBpm > 0) {
          bpmSum += g_repHistory[i].avgBpm;
          spo2Sum += g_repHistory[i].avgSpo2;
          vitalsCount++;
        }
      }
    }

    rec.avgQuality = (qCount > 0) ? (qSum / qCount) : 80.0f;
    rec.avgBpm = (vitalsCount > 0) ? (bpmSum / vitalsCount) : getDisplayBpm();
    rec.avgSpo2 = (vitalsCount > 0) ? (spo2Sum / vitalsCount) : getDisplaySpo2();

    uint32_t now = millis();
    rec.durationSec = (now >= g_currentSetStartTimeMs) ? ((now - g_currentSetStartTimeMs) / 1000) : 0;

    if (g_setHistoryCount < MAX_SET_HISTORY) {
      g_setHistory[g_setHistoryCount++] = rec;
    } else {
      for (size_t i = 0; i < MAX_SET_HISTORY - 1; i++) {
        g_setHistory[i] = g_setHistory[i + 1];
      }
      g_setHistory[MAX_SET_HISTORY - 1] = rec;
    }

    Serial.println();
    Serial.println("==================================================");
    Serial.printf("  SET #%u FINISHED: %u reps of %s\r\n", rec.setNumber, rec.repCount, exerciseName(rec.exercise));
    Serial.printf("  Avg Quality: %.1f%% | Duration: %u s\r\n", rec.avgQuality, rec.durationSec);
    if (rec.avgBpm > 0) {
      Serial.printf("  Avg HR: %d bpm | SpO2: %d%%\r\n", rec.avgBpm, rec.avgSpo2);
    }
    Serial.println("==================================================");

    g_currentSetNumber++;
  }

  // Reset reps to 0 for the new activity
  g_repCount = 0;
  g_currentSetStartTimeMs = millis();
  g_exercise = nextExercise;
  g_fsmState = STATE_IDLE;
  g_currentRep = {0};
  resetPressTracking();
}

// BLE advertisement callback
class WorkoutBeaconAdvertisedCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    // Fast zero-allocation rejection of foreign devices in dense RF rooms:
    // Read raw payload bytes directly without creating Arduino String objects on heap.
    const uint8_t* payload = advertisedDevice.getPayload();
    size_t payloadLen = advertisedDevice.getPayloadLength();
    if (!payload || payloadLen < 4) return;

    bool matched = false;
    uint8_t exId = 0;
    uint8_t red = 0, green = 180, blue = 255;
    char bName[16] = {0};
    char exName[24] = {0};
    char hex[10] = "#00b4ff";

    size_t idx = 0;
    bool foundMfg = false;
    bool foundName = false;
    uint8_t fallbackExId = 0;

    // Scan AD structures in raw payload
    while (idx < payloadLen) {
      uint8_t adLen = payload[idx];
      if (adLen == 0 || (idx + 1 + adLen) > payloadLen) break;
      uint8_t adType = payload[idx + 1];
      const uint8_t* adData = &payload[idx + 2];
      uint8_t dataLen = adLen - 1;

      // 1. Check Manufacturer Specific Data (0xFF)
      // Marker: [0x19, 0x80] (Company ID) + [0xAC] (Magic Activity Marker) + [exId] + [R, G, B]
      if (adType == 0xFF && dataLen >= 7) {
        if (adData[0] == 0x19 && adData[1] == 0x80 && adData[2] == 0xAC) {
          exId = adData[3];
          red = adData[4];
          green = adData[5];
          blue = adData[6];
          foundMfg = true;
          break; // Primary activity marker confirmed, stop scanning payload
        }
      }
      // 2. Fallback check by local name (0x09 = Complete, 0x08 = Shortened)
      else if (!foundName && (adType == 0x09 || adType == 0x08) && dataLen >= 8) {
        if (memcmp(adData, "ACT-", 4) == 0) {
          if (memcmp(adData + 4, "CURL", 4) == 0) {
            fallbackExId = 0;
            foundName = true;
          } else if (memcmp(adData + 4, "PRES", 4) == 0) {
            fallbackExId = 1;
            foundName = true;
          } else if (memcmp(adData + 4, "SQUA", 4) == 0) {
            fallbackExId = 2;
            foundName = true;
          }
        }
      }
      idx += 1 + adLen;
    }

    if (!foundMfg && !foundName) {
      return; // Discard hundreds of ambient non-workout devices in < 20 ns with 0 allocations
    }

    if (!foundMfg && foundName) {
      exId = fallbackExId;
      if (exId == 1) {
        red = 180; green = 0; blue = 255;
      } else if (exId == 2) {
        red = 0; green = 255; blue = 80;
      } else {
        red = 0; green = 180; blue = 255;
      }
    }

    matched = true;
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", red, green, blue);

    if (exId == 1) {
      strncpy(bName, "ACT-PRESS", sizeof(bName) - 1);
      strncpy(exName, "Shoulder Press", sizeof(exName) - 1);
    } else if (exId == 2) {
      strncpy(bName, "ACT-SQUAT", sizeof(bName) - 1);
      strncpy(exName, "Squat", sizeof(exName) - 1);
    } else {
      strncpy(bName, "ACT-CURL", sizeof(bName) - 1);
      strncpy(exName, "Bicep Curl", sizeof(exName) - 1);
    }

    int rawRssi = advertisedDevice.getRSSI();
    // Only format MAC string for valid matching workout beacons
    String macStr = advertisedDevice.getAddress().toString();
    uint32_t now = millis();

    if (s_beaconMutex != NULL && xSemaphoreTake(s_beaconMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
      bool found = false;
      for (size_t i = 0; i < s_observedBeaconCount; i++) {
        if (strcmp(s_observedBeacons[i].mac, macStr.c_str()) == 0) {
          s_observedBeacons[i].lastRawRssi = rawRssi;
          s_observedBeacons[i].filteredRssi = (RSSI_EMA_ALPHA * (float)rawRssi) + ((1.0f - RSSI_EMA_ALPHA) * s_observedBeacons[i].filteredRssi);
          s_observedBeacons[i].lastSeenMs = now;
          s_observedBeacons[i].exerciseId = exId;
          s_observedBeacons[i].r = red;
          s_observedBeacons[i].g = green;
          s_observedBeacons[i].b = blue;
          strncpy(s_observedBeacons[i].hexColor, hex, sizeof(s_observedBeacons[i].hexColor) - 1);
          strncpy(s_observedBeacons[i].beaconName, bName, sizeof(s_observedBeacons[i].beaconName) - 1);
          strncpy(s_observedBeacons[i].exerciseName, exName, sizeof(s_observedBeacons[i].exerciseName) - 1);
          found = true;
          break;
        }
      }

      if (!found && s_observedBeaconCount < MAX_OBSERVED_BEACONS) {
        ObservedBeacon& b = s_observedBeacons[s_observedBeaconCount++];
        b.exerciseId = exId;
        b.r = red;
        b.g = green;
        b.b = blue;
        b.lastRawRssi = rawRssi;
        b.filteredRssi = (float)rawRssi;
        b.lastSeenMs = now;
        strncpy(b.hexColor, hex, sizeof(b.hexColor) - 1);
        strncpy(b.beaconName, bName, sizeof(b.beaconName) - 1);
        strncpy(b.exerciseName, exName, sizeof(b.exerciseName) - 1);
        strncpy(b.mac, macStr.c_str(), sizeof(b.mac) - 1);
      }
      xSemaphoreGive(s_beaconMutex);
    }
  }
};

// BLE background scanning task
inline void bleScanTask(void* pvParameters) {
  BLEDevice::init("WorkoutMonitor");
  BLEScan* pBLEScan = BLEDevice::getScan();
  // shouldParse = false is critical in dense RF environments:
  // It prevents BLEAdvertisedDevice::parseAdvertisement from leaking heap memory
  // and pushing thousands of elements into m_serviceUUIDs vector on every duplicate packet.
  pBLEScan->setAdvertisedDeviceCallbacks(new WorkoutBeaconAdvertisedCallbacks(), true, false);
  pBLEScan->setActiveScan(false);
  // 100% continuous duty cycle: listening across channels 37, 38, 39 without deaf gaps
  pBLEScan->setInterval(160); // 100 ms
  pBLEScan->setWindow(160);   // 100 ms (100% duty cycle, eliminates the 50ms blind window)

  while (true) {
    // 3-second scan window: ensures continuous listening while regularly freeing
    // any accumulated foreign device MAC entries to keep heap usage tightly bounded.
    pBLEScan->start(3, false);
    pBLEScan->clearResults();
  }
}

// Evaluate closest beacon and update tracking state
inline void updateBeaconDetector() {
  uint32_t now = millis();
  if (s_beaconMutex == NULL) return;

  if (xSemaphoreTake(s_beaconMutex, pdMS_TO_TICKS(15)) != pdTRUE) return;

  // Prune timed-out beacons
  size_t writeIdx = 0;
  for (size_t i = 0; i < s_observedBeaconCount; i++) {
    if (now - s_observedBeacons[i].lastSeenMs <= BEACON_TIMEOUT_MS) {
      if (writeIdx != i) {
        s_observedBeacons[writeIdx] = s_observedBeacons[i];
      }
      writeIdx++;
    }
  }
  s_observedBeaconCount = writeIdx;

  // Find candidate beacons meeting minimum RSSI threshold
  int bestIdx = -1;
  float bestRssi = -999.0f;

  for (size_t i = 0; i < s_observedBeaconCount; i++) {
    if (s_observedBeacons[i].filteredRssi >= MIN_BEACON_RSSI) {
      if (s_observedBeacons[i].filteredRssi > bestRssi) {
        bestRssi = s_observedBeacons[i].filteredRssi;
        bestIdx = (int)i;
      }
    }
  }

  if (bestIdx == -1) {
    // No good beacon detected
    if (g_beaconValid) {
      g_beaconValid = false;
      g_activeBeacon.valid = false;
      Serial.println(F("\r\n[BLE BEACON] No good beacon in range (RSSI < -82dBm or timed out). Activity tracking PAUSED."));
      broadcastWsData(false);
    }
    xSemaphoreGive(s_beaconMutex);
    return;
  }

  const ObservedBeacon& candidate = s_observedBeacons[bestIdx];

  // Determine whether to switch active beacon
  bool shouldSwitch = false;

  if (!g_beaconValid) {
    // Was invalid, now candidate is good
    shouldSwitch = true;
  } else {
    // Find current active beacon's filtered RSSI
    float currentActiveFilteredRssi = -999.0f;
    bool currentStillPresent = false;
    for (size_t i = 0; i < s_observedBeaconCount; i++) {
      if (strcmp(s_observedBeacons[i].mac, g_activeBeacon.macAddress) == 0) {
        currentActiveFilteredRssi = s_observedBeacons[i].filteredRssi;
        currentStillPresent = (currentActiveFilteredRssi >= MIN_BEACON_RSSI);
        break;
      }
    }

    if (!currentStillPresent) {
      shouldSwitch = true;
    } else if (candidate.exerciseId != g_activeBeacon.exerciseId) {
      // Hysteresis: only switch if candidate is significantly stronger
      if (candidate.filteredRssi > (currentActiveFilteredRssi + HYSTERESIS_RSSI_DB)) {
        shouldSwitch = true;
      }
    }
  }

  if (shouldSwitch) {
    // Only finalize set and announce if the exercise actually changed (or on first detection).
    // If the signal was briefly lost and re-acquired for the SAME exercise, resume without resetting reps!
    bool isSameExerciseReacquired = (!g_beaconValid && (g_activeBeacon.exerciseId == candidate.exerciseId) && (g_activeBeacon.beaconName[0] != '\0'));
    bool exerciseActuallyChanged = !isSameExerciseReacquired && (g_activeBeacon.beaconName[0] == '\0' || (g_activeBeacon.exerciseId != candidate.exerciseId));

    g_beaconValid = true;
    g_activeBeacon.valid = true;
    g_activeBeacon.exerciseId = candidate.exerciseId;
    strncpy(g_activeBeacon.beaconName, candidate.beaconName, sizeof(g_activeBeacon.beaconName) - 1);
    strncpy(g_activeBeacon.exerciseName, candidate.exerciseName, sizeof(g_activeBeacon.exerciseName) - 1);
    g_activeBeacon.rssi = (int)candidate.filteredRssi;
    g_activeBeacon.r = candidate.r;
    g_activeBeacon.g = candidate.g;
    g_activeBeacon.b = candidate.b;
    strncpy(g_activeBeacon.hexColor, candidate.hexColor, sizeof(g_activeBeacon.hexColor) - 1);
    strncpy(g_activeBeacon.macAddress, candidate.mac, sizeof(g_activeBeacon.macAddress) - 1);
    g_activeBeacon.lastSeenMs = candidate.lastSeenMs;
    g_activeBeacon.lastChangeTimeMs = now;

    if (isSameExerciseReacquired) {
      // Smoothly resume existing set without wiping reps or hijacking the OLED screen
      Serial.printf("\r\n[BLE BEACON] Signal re-acquired: %s (%s) @ %d dBm. Resuming Set #%u (Reps: %u).\r\n",
                    candidate.beaconName, candidate.exerciseName, (int)candidate.filteredRssi,
                    g_currentSetNumber, g_repCount.load());
    } else {
      g_beaconChangeScreenUntilMs = now + 2500; // Trigger OLED announcement screen

      Serial.println();
      Serial.println("==================================================");
      Serial.printf("  [BLE BEACON] ACTIVE BEACON: %s (%s)\r\n", candidate.beaconName, candidate.exerciseName);
      Serial.printf("  Signal RSSI: %d dBm | MAC: %s\r\n", (int)candidate.filteredRssi, candidate.mac);
      Serial.println("==================================================");

      if (exerciseActuallyChanged) {
        finalizeCurrentSet((ExerciseType)candidate.exerciseId);
      }
    }
    broadcastWsData(true);
  } else {
    // Keep active beacon RSSI and last seen updated
    g_activeBeacon.rssi = (int)candidate.filteredRssi;
    g_activeBeacon.lastSeenMs = candidate.lastSeenMs;
  }

  xSemaphoreGive(s_beaconMutex);
}

// Initialize BLE beacon scanner background task
inline void initBeaconDetector() {
  if (s_beaconMutex == NULL) {
    s_beaconMutex = xSemaphoreCreateMutex();
  }

  if (s_bleScanTaskHandle == NULL) {
    xTaskCreatePinnedToCore(
      bleScanTask,
      "ble_scan_task",
      6144,
      NULL,
      1,
      &s_bleScanTaskHandle,
      0
    );
    Serial.println(F("[BLE BEACON] Scanner task initialized on Core 0"));
  }
}

// Print detailed beacon status to Serial
inline void logBeaconStatus() {
  Serial.println();
  Serial.println("==================================================");
  Serial.println("           BLE Workout Beacon Status              ");
  Serial.println("==================================================");
  Serial.printf("  Beacon Detection Valid:  %s\r\n", g_beaconValid ? "YES" : "NO (Tracking Paused)");
  if (g_beaconValid) {
    Serial.printf("  Active Beacon:           %s (%s)\r\n", g_activeBeacon.beaconName, g_activeBeacon.exerciseName);
    Serial.printf("  Signal RSSI:             %d dBm (Threshold: %d dBm)\r\n", g_activeBeacon.rssi, MIN_BEACON_RSSI);
    Serial.printf("  Beacon MAC:              %s\r\n", g_activeBeacon.macAddress);
    Serial.printf("  Active Set Number:       #%u\r\n", g_currentSetNumber);
    Serial.printf("  Current Set Reps:        %u\r\n", g_repCount.load());
  } else {
    Serial.println("  No recognized workout beacon currently in range.");
  }

  Serial.println("--------------------------------------------------");
  Serial.printf("  Total Beacons Detected:  %u\r\n", (unsigned int)s_observedBeaconCount);

  if (s_beaconMutex != NULL && xSemaphoreTake(s_beaconMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    uint32_t now = millis();
    for (size_t i = 0; i < s_observedBeaconCount; i++) {
      uint32_t ageMs = now - s_observedBeacons[i].lastSeenMs;
      Serial.printf("   [%u] %s | %s | RSSI: %d (Filt: %.1f) | Age: %u ms | %s\r\n",
                    (unsigned int)i + 1,
                    s_observedBeacons[i].beaconName,
                    s_observedBeacons[i].exerciseName,
                    s_observedBeacons[i].lastRawRssi,
                    s_observedBeacons[i].filteredRssi,
                    ageMs,
                    s_observedBeacons[i].mac);
    }
    xSemaphoreGive(s_beaconMutex);
  }

  Serial.println("--------------------------------------------------");
  Serial.printf("  Completed Sets Count:    %u\r\n", (unsigned int)g_setHistoryCount);
  for (size_t i = 0; i < g_setHistoryCount; i++) {
    Serial.printf("   Set #%u: %u reps of %s (Quality: %.1f%%, Dur: %us)\r\n",
                  g_setHistory[i].setNumber,
                  g_setHistory[i].repCount,
                  exerciseName(g_setHistory[i].exercise),
                  g_setHistory[i].avgQuality,
                  g_setHistory[i].durationSec);
  }
  Serial.println("==================================================");
}
