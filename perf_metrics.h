#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "pulse_ox.h"

#if ENABLE_PERF_METRICS

#include "esp_freertos_hooks.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

namespace PerfDebug {

// Idle counters
static volatile uint32_t s_idleTicksCore0 = 0;
static volatile uint32_t s_idleTicksCore1 = 0;

static bool IRAM_ATTR idleHookCore0() {
  s_idleTicksCore0++;
  return true;
}

static bool IRAM_ATTR idleHookCore1() {
  s_idleTicksCore1++;
  return true;
}

// Kinematics tracking
static uint64_t s_lastTotalTimeUs = 0;
static uint32_t s_lastIdleTime0Us = 0;
static uint32_t s_lastIdleTime1Us = 0;
static uint32_t s_lastIdleTicks0 = 0;
static uint32_t s_lastIdleTicks1 = 0;
static uint32_t s_lastPerfPrintMs = 0;

static float s_lastKinematicsDtMs = 20.0f;
static uint32_t s_lastKinematicsExecUs = 0;
static uint32_t s_maxKinematicsExecUs = 0;

// Idle hooks initialization
inline void initPerfHooks() {
  esp_register_freertos_idle_hook_for_cpu(idleHookCore0, 0);
  esp_register_freertos_idle_hook_for_cpu(idleHookCore1, 1);
  s_lastTotalTimeUs = esp_timer_get_time();
  s_lastIdleTime0Us = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(0);
  s_lastIdleTime1Us = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(1);
  s_lastIdleTicks0 = s_idleTicksCore0;
  s_lastIdleTicks1 = s_idleTicksCore1;
}

inline void recordKinematicsTiming(float dtMs, uint32_t execUs) {
  s_lastKinematicsDtMs = dtMs;
  s_lastKinematicsExecUs = execUs;
  if (execUs > s_maxKinematicsExecUs) {
    s_maxKinematicsExecUs = execUs;
  }
}

// Performance report
inline void printPerformanceReport() {
  uint64_t nowTotalUs = esp_timer_get_time();
  uint32_t nowIdle0Us = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(0);
  uint32_t nowIdle1Us = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(1);
  uint32_t nowTicks0 = s_idleTicksCore0;
  uint32_t nowTicks1 = s_idleTicksCore1;

  uint64_t dTotalUs = nowTotalUs - s_lastTotalTimeUs;
  uint32_t dIdle0Us = nowIdle0Us - s_lastIdleTime0Us;
  uint32_t dIdle1Us = nowIdle1Us - s_lastIdleTime1Us;
  uint32_t dTicks0 = nowTicks0 - s_lastIdleTicks0;
  uint32_t dTicks1 = nowTicks1 - s_lastIdleTicks1;

  s_lastTotalTimeUs = nowTotalUs;
  s_lastIdleTime0Us = nowIdle0Us;
  s_lastIdleTime1Us = nowIdle1Us;
  s_lastIdleTicks0 = nowTicks0;
  s_lastIdleTicks1 = nowTicks1;

  float idlePct0 = 0.0f;
  float idlePct1 = 0.0f;
  if (dTotalUs > 0) {
    idlePct0 = ((float)dIdle0Us * 100.0f) / (float)dTotalUs;
    idlePct1 = ((float)dIdle1Us * 100.0f) / (float)dTotalUs;
  }
  if (idlePct0 > 100.0f) idlePct0 = 100.0f;
  if (idlePct0 < 0.0f) idlePct0 = 0.0f;
  if (idlePct1 > 100.0f) idlePct1 = 100.0f;
  if (idlePct1 < 0.0f) idlePct1 = 0.0f;

  float loadPct0 = 100.0f - idlePct0;
  float loadPct1 = 100.0f - idlePct1;

  // Clocks & memory
  uint32_t cpuFreqMhz = getCpuFrequencyMhz();
  uint32_t apbFreqMhz = getApbFrequency() / 1000000;
  uint32_t xtalFreqMhz = getXtalFrequencyMhz();

  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minFreeHeap = ESP.getMinFreeHeap();
  uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  float heapFrag = 0.0f;
  if (freeHeap > 0 && maxAllocHeap <= freeHeap) {
    heapFrag = 100.0f - (((float)maxAllocHeap * 100.0f) / (float)freeHeap);
  }

  UBaseType_t loopTaskWatermark = uxTaskGetStackHighWaterMark(NULL);
  UBaseType_t pulseOxWatermark = (g_pulseOxTaskHandle != NULL) ? uxTaskGetStackHighWaterMark(g_pulseOxTaskHandle) : 0;

  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  String staIP = wifiConnected ? WiFi.localIP().toString() : "Not Connected";
  String staMac = WiFi.macAddress();
  int32_t channel = WiFi.channel();
  int8_t rssi = wifiConnected ? WiFi.RSSI() : 0;

  Serial.println();
  Serial.println(F("+-----------------------------------------------------------------------+"));
  Serial.println(F("|                  ESP32-S3 PERFORMANCE & SYSTEM DEBUG                  |"));
  Serial.println(F("+-----------------------------------------------------------------------+"));
  Serial.printf("| CPU Frequencies:  CPU: %u MHz | APB: %u MHz | XTAL: %u MHz\r\n", cpuFreqMhz, apbFreqMhz, xtalFreqMhz);
  Serial.printf("| Core 0 (System):  Idle: %5.1f%% | Load: %5.1f%% (Idle ticks: %u)\r\n", idlePct0, loadPct0, dTicks0);
  Serial.printf("| Core 1 (Kinemat): Idle: %5.1f%% | Load: %5.1f%% (Idle ticks: %u)\r\n", idlePct1, loadPct1, dTicks1);
  Serial.printf("| Kinematics Loop:  dt: %4.1f ms (%.1f Hz) | Exec: %u us (Max: %u us)\r\n",
                s_lastKinematicsDtMs,
                (s_lastKinematicsDtMs > 0.1f) ? (1000.0f / s_lastKinematicsDtMs) : 0.0f,
                s_lastKinematicsExecUs,
                s_maxKinematicsExecUs);
  Serial.printf("| Memory / Heap:    Free: %u B (%u KB) | Min Free: %u B | Frag: %.1f%%\r\n",
                freeHeap, freeHeap / 1024, minFreeHeap, heapFrag);
  Serial.printf("| Task Watermarks:  loopTask: %u B | pulse_ox_task: %u B\r\n",
                (uint32_t)(loopTaskWatermark * sizeof(StackType_t)),
                (uint32_t)(pulseOxWatermark * sizeof(StackType_t)));
  Serial.printf("| Wi-Fi Station:    SSID: \"%s\" | Status: %s | Ch: %ld\r\n",
                WIFI_SSID, wifiConnected ? "CONNECTED" : "CONNECTING", (long)channel);
  Serial.printf("| IP & Signal:      IP: %s | RSSI: %d dBm | MAC: %s\r\n",
                staIP.c_str(), rssi, staMac.c_str());
  Serial.printf("| Cloud Logging:    Google Sheets: %s\r\n",
                ENABLE_GOOGLE_SHEETS_LOGGING ? (wifiConnected ? "ACTIVE" : "AWAITING WIFI") : "DISABLED");
  Serial.printf("| System State:     Core Temp: %.1f C | Uptime: %lu s\r\n",
                getCoreTemp(), (unsigned long)(millis() / 1000));
  Serial.println(F("+-----------------------------------------------------------------------+"));
  Serial.println();
}

// Update and toggle
inline void update(bool plotterMode) {
  if (!Serial) return;
  if (!g_perfMetricsEnabled || plotterMode) return;
  uint32_t now = millis();
  if (now - s_lastPerfPrintMs >= PERF_METRICS_INTERVAL_MS) {
    s_lastPerfPrintMs = now;
    printPerformanceReport();
  }
}

inline void toggle() {
  g_perfMetricsEnabled = !g_perfMetricsEnabled;
  Serial.printf("\r\n[PERF DEBUG] Performance output %s\r\n", g_perfMetricsEnabled ? "ENABLED" : "DISABLED");
  if (g_perfMetricsEnabled) {
    printPerformanceReport();
  }
}

} // namespace PerfDebug

#else

// Disabled metrics fallback
namespace PerfDebug {
  inline void initPerfHooks() {}
  inline void recordKinematicsTiming(float, uint32_t) {}
  inline void printPerformanceReport() {}
  inline void update(bool) {}
  inline void toggle() {
    Serial.println(F("\r\n[PERF DEBUG] Performance metrics are disabled at compile time."));
  }
} // namespace PerfDebug

#endif
