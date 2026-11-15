#pragma once

#include <Arduino.h>
#include <atomic>

// Pinout
constexpr int PIN_ZOUT = 5;
constexpr int PIN_YOUT = 6;
constexpr int PIN_XOUT = 7;
constexpr int PIN_NEOPIXEL = 48;
constexpr int PIN_I2C_SDA = 3;
constexpr int PIN_I2C_SCL = 4;
constexpr int PIN_TOUCH_IP = 11;

// Sensor & kinematic constants
inline float g_zeroG_X_mV = 1745.0f;
inline float g_zeroG_Y_mV = 1745.0f;
inline float g_zeroG_Z_mV = 1745.0f;
inline float g_sensitivity_mV_per_g = 315.0f;
constexpr uint32_t SAMPLE_INTERVAL_MS = 20;

// Pulse oximeter constants
constexpr uint32_t PULSE_OX_FINGER_THRESHOLD = 10000;
constexpr unsigned long PULSE_OX_KEEP_LAST_MS = 8000;
constexpr int PULSE_OX_WINDOW = 100;
constexpr int PULSE_OX_STEP = 25;

// Google Sheets logging
inline bool ENABLE_GOOGLE_SHEETS_LOGGING = true;
constexpr uint32_t GOOGLE_SHEETS_INTERVAL_MS = 4000;
inline const char* GOOGLE_SHEETS_URL = "https://script.google.com/macros/s/AKfycbz9-rOHnXI12fe7s2rbl4aASGnCsq5rFBiSPKpXRylRA3bS0BUmj3QGihcWTZO9wntr_Q/exec";

// Wi-Fi credentials
inline const char* WIFI_SSID = "iPhone Ivanna";
inline const char* WIFI_PASS = "5261769199";

// SoftAP fallback
inline const char* AP_SSID = "esp32ap";
inline const char* AP_PASS = "12345678";

// Performance debugging
#define ENABLE_PERF_METRICS true
inline bool g_perfMetricsEnabled = true;
constexpr uint32_t PERF_METRICS_INTERVAL_MS = 2000;

// Data types
enum FsmState {
  STATE_IDLE,
  STATE_CONCENTRIC,
  STATE_PEAK,
  STATE_ECCENTRIC,
  STATE_COMPLETED
};

// Exercises
enum ExerciseType {
  EXERCISE_CURL,   // tracked by forearm angle
  EXERCISE_PRESS   // shoulder press, tracked by vertical movement
};

inline const char* exerciseName(ExerciseType ex) {
  return (ex == EXERCISE_PRESS) ? "Shoulder Press" : "Bicep Curl";
}

extern ExerciseType g_exercise;
extern volatile FsmState g_fsmState;
extern std::atomic<uint32_t> g_repCount;
extern bool isCalibrated;
struct KinematicsState;
extern KinematicsState g_kinematics;
struct RepMetrics;
extern RepMetrics g_lastCompletedRep;

struct AccelerometerSample {
  float mvX, mvY, mvZ;
  float ax_g, ay_g, az_g;
  float filtered_ax, filtered_ay, filtered_az;
};

struct KinematicsState {
  float angleDeg;
  float prevAngleDeg;
  float angularVelocity;
  float jerk;
  float liveQuality;
  float upAccel;     // vertical acceleration with gravity removed, g (+ = up)
  float upVelocity;  // vertical speed, m/s (+ = up)
  float heightCm;    // height gained since the rep started, cm
  float sideAccel;   // acceleration off the vertical, g (sway or tilt)
};

struct PulseOxState {
  bool connected;
  bool fingerPresent;
  bool haveBpm;
  bool haveSpo2;
  bool latestBpmValid;
  bool latestSpo2Valid;
  float bpm;
  float spo2;
  uint32_t lastBpmTime;
  uint32_t lastSpo2Time;
  uint32_t lastFingerTime;
  uint32_t rawIR;
  uint32_t rawRed;
};

struct RepMetrics {
  uint32_t repNumber;
  uint32_t startTimeMs;
  uint32_t peakTimeMs;
  uint32_t completeTimeMs;
  float concentricDurationSec;
  float eccentricDurationSec;
  float peakAngleDeg;
  float peakHeightCm;
  ExerciseType exercise;
  float qualitySum;
  uint32_t qualitySampleCount;
  float finalQualityFactor;
  bool flagFastDrop;
  bool flagHighJerk;
  bool flagLowROM;
  int avgBpm;
  int avgSpo2;
};
