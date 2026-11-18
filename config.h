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
inline const char* WIFI_SSID = "Pixel_7267";
inline const char* WIFI_PASS = "thyshalldata";

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

// Exercises. Both are tracked by how far the forearm has rotated from the calibrated
// arm-hanging position: an accelerometer measures tilt reliably, but not position.
// Values match the beacon exercise IDs (0 = ACT-CURL, 1 = ACT-RAISE).
enum ExerciseType {
  EXERCISE_CURL,          // forearm swings up in front, elbow bends (~90-140 deg)
  EXERCISE_LATERAL_RAISE  // straight arm lifts out to the side to shoulder height (~90 deg)
};

inline const char* exerciseName(ExerciseType ex) {
  return (ex == EXERCISE_LATERAL_RAISE) ? "Lateral Raise" : "Bicep Curl";
}

// Short label for the OLED and the web JSON
inline const char* exerciseShortName(ExerciseType ex) {
  return (ex == EXERCISE_LATERAL_RAISE) ? "RAISE" : "CURL";
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
  ExerciseType exercise;
  float qualitySum;
  uint32_t qualitySampleCount;
  float finalQualityFactor;
  bool flagFastDrop;
  bool flagHighJerk;
  bool flagLowROM;
  bool flagTooHigh;   // lateral raise lifted well above shoulder height
  int avgBpm;
  int avgSpo2;
};

constexpr size_t MAX_REP_HISTORY = 40;
extern RepMetrics g_currentRep;
extern RepMetrics g_repHistory[MAX_REP_HISTORY];
extern size_t g_repHistoryCount;

// Sets tracking
struct SetRecord {
  uint32_t setNumber;
  ExerciseType exercise;
  uint32_t repCount;
  float avgQuality;
  uint32_t durationSec;
  int avgBpm;
  int avgSpo2;
};

constexpr size_t MAX_SET_HISTORY = 20;
extern SetRecord g_setHistory[MAX_SET_HISTORY];
extern size_t g_setHistoryCount;
extern uint32_t g_currentSetNumber;
extern uint32_t g_currentSetStartTimeMs;

// Active BLE Beacon State
struct ActiveBeaconState {
  bool valid;
  uint8_t exerciseId;
  char beaconName[16];
  char exerciseName[24];
  int rssi;
  uint8_t r, g, b;
  char hexColor[10];
  uint32_t lastSeenMs;
  uint32_t lastChangeTimeMs;
  char macAddress[18];
};

extern ActiveBeaconState g_activeBeacon;
extern bool g_beaconValid;
extern uint32_t g_beaconChangeScreenUntilMs;

inline bool isBeaconValid() {
  return g_beaconValid;
}

inline bool isBeaconChangeScreenActive() {
  return millis() < g_beaconChangeScreenUntilMs;
}
