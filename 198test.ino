#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <esp32-hal-rgb-led.h>
#include <math.h>

#include "config.h"
#include "pulse_ox.h"
#include "webpage.h"
#include "perf_metrics.h"

// vars
float restX_g = 0.0f;
float restY_g = 0.0f;
float restZ_g = 1.0f;
bool isCalibrated = false;

ExerciseType g_exercise = EXERCISE_CURL;
// Smoothed gravity reading while still: the direction that counts as "up" for the press
float upX_g = 0.0f;
float upY_g = 0.0f;
float upZ_g = 1.0f;

AccelerometerSample g_sensor = {0};
KinematicsState g_kinematics = {0};
RepMetrics g_currentRep = {0};
RepMetrics g_lastCompletedRep = {0};
constexpr size_t MAX_REP_HISTORY = 40;
RepMetrics g_repHistory[MAX_REP_HISTORY];
size_t g_repHistoryCount = 0;
volatile FsmState g_fsmState = STATE_IDLE;
uint32_t g_repCount = 0;

uint32_t g_lastSampleTimeMs = 0;
uint32_t g_completionFlashUntilMs = 0;
uint32_t g_lastTelemetryPrintMs = 0;
bool g_streamTelemetry = true;
bool g_paused = false;
bool g_plotterMode = false;

WebServer server(80);
WebSocketsServer webSocket(81);
size_t s_lastWsRepHistoryCount = 0;
uint32_t s_lastWsRepCount = 0;

void calibrateRestPosition();

// neopix RGB
void setNeoPixelColor(uint8_t red, uint8_t green, uint8_t blue) {
  rgbLedWriteOrdered(PIN_NEOPIXEL, LED_COLOR_ORDER_RGB, red, green, blue);
}

// filter
void sampleAccelerometer() {
  uint32_t sumX = 0, sumY = 0, sumZ = 0;
  constexpr int OVERSAMPLES = 4;
  for (int i = 0; i < OVERSAMPLES; i++) {
    sumX += analogReadMilliVolts(PIN_XOUT);
    sumY += analogReadMilliVolts(PIN_YOUT);
    sumZ += analogReadMilliVolts(PIN_ZOUT);
  }
  g_sensor.mvX = (float)(sumX / OVERSAMPLES);
  g_sensor.mvY = (float)(sumY / OVERSAMPLES);
  g_sensor.mvZ = (float)(sumZ / OVERSAMPLES);

  g_sensor.ax_g = (g_sensor.mvX - g_zeroG_X_mV) / g_sensitivity_mV_per_g;
  g_sensor.ay_g = (g_sensor.mvY - g_zeroG_Y_mV) / g_sensitivity_mV_per_g;
  g_sensor.az_g = (g_sensor.mvZ - g_zeroG_Z_mV) / g_sensitivity_mV_per_g;

  constexpr float ALPHA = 0.30f;
  g_sensor.filtered_ax = (ALPHA * g_sensor.ax_g) + ((1.0f - ALPHA) * g_sensor.filtered_ax);
  g_sensor.filtered_ay = (ALPHA * g_sensor.ay_g) + ((1.0f - ALPHA) * g_sensor.filtered_ay);
  g_sensor.filtered_az = (ALPHA * g_sensor.az_g) + ((1.0f - ALPHA) * g_sensor.filtered_az);
}

// kinematics
void computeKinematics(float dt_sec) {
  float dot = (g_sensor.filtered_ax * restX_g) +
              (g_sensor.filtered_ay * restY_g) +
              (g_sensor.filtered_az * restZ_g);

  float magCurrent = sqrtf((g_sensor.filtered_ax * g_sensor.filtered_ax) +
                           (g_sensor.filtered_ay * g_sensor.filtered_ay) +
                           (g_sensor.filtered_az * g_sensor.filtered_az));

  float magRest = sqrtf((restX_g * restX_g) + (restY_g * restY_g) + (restZ_g * restZ_g));
  float denom = magCurrent * magRest;

  float angle_deg = 0.0f;
  if (denom > 0.0001f) {
    float cosTheta = dot / denom;
    if (cosTheta > 1.0f) cosTheta = 1.0f;
    if (cosTheta < -1.0f) cosTheta = -1.0f;
    angle_deg = acosf(cosTheta) * (180.0f / (float)M_PI);
  }

  constexpr float ANGLE_LPF = 0.40f;
  g_kinematics.prevAngleDeg = g_kinematics.angleDeg;
  g_kinematics.angleDeg = (ANGLE_LPF * angle_deg) + ((1.0f - ANGLE_LPF) * g_kinematics.angleDeg);

  if (dt_sec > 0.0001f) {
    float instantVel = (g_kinematics.angleDeg - g_kinematics.prevAngleDeg) / dt_sec;
    constexpr float VEL_LPF = 0.35f;
    g_kinematics.angularVelocity = (VEL_LPF * instantVel) + ((1.0f - VEL_LPF) * g_kinematics.angularVelocity);
  }

  static float lastFiltAx = 0, lastFiltAy = 0, lastFiltAz = 0;
  float dAx = (dt_sec > 0.0001f) ? (g_sensor.filtered_ax - lastFiltAx) / dt_sec : 0.0f;
  float dAy = (dt_sec > 0.0001f) ? (g_sensor.filtered_ay - lastFiltAy) / dt_sec : 0.0f;
  float dAz = (dt_sec > 0.0001f) ? (g_sensor.filtered_az - lastFiltAz) / dt_sec : 0.0f;
  lastFiltAx = g_sensor.filtered_ax;
  lastFiltAy = g_sensor.filtered_ay;
  lastFiltAz = g_sensor.filtered_az;

  float rawJerk = sqrtf((dAx * dAx) + (dAy * dAy) + (dAz * dAz));
  g_kinematics.jerk = (0.35f * rawJerk) + (0.65f * g_kinematics.jerk);
}

// press tracking reset
void resetPressTracking() {
  upX_g = g_sensor.filtered_ax;
  upY_g = g_sensor.filtered_ay;
  upZ_g = g_sensor.filtered_az;
  g_kinematics.upAccel = 0.0f;
  g_kinematics.upVelocity = 0.0f;
  g_kinematics.heightCm = 0.0f;
  g_kinematics.sideAccel = 0.0f;
}

// linear kinematics (shoulder press)
void computeLinearKinematics(float dt_sec) {
  float ax = g_sensor.filtered_ax;
  float ay = g_sensor.filtered_ay;
  float az = g_sensor.filtered_az;

  // Learn which way is up only while resting, so the press itself doesn't skew it.
  // Adapt faster when the arm has clearly changed position (e.g. hands raised to the shoulders).
  if (g_fsmState == STATE_IDLE) {
    float upLpf = (g_kinematics.sideAccel > 0.30f) ? 0.15f : 0.03f;
    upX_g = (upLpf * ax) + ((1.0f - upLpf) * upX_g);
    upY_g = (upLpf * ay) + ((1.0f - upLpf) * upY_g);
    upZ_g = (upLpf * az) + ((1.0f - upLpf) * upZ_g);
  }

  float upMag = sqrtf((upX_g * upX_g) + (upY_g * upY_g) + (upZ_g * upZ_g));
  if (upMag < 0.0001f || dt_sec < 0.0001f) return;
  float ux = upX_g / upMag;
  float uy = upY_g / upMag;
  float uz = upZ_g / upMag;

  // Split the reading into the part along "up" and the part off to the side
  float along = (ax * ux) + (ay * uy) + (az * uz);
  float sx = ax - (along * ux);
  float sy = ay - (along * uy);
  float sz = az - (along * uz);
  g_kinematics.sideAccel = sqrtf((sx * sx) + (sy * sy) + (sz * sz));

  // Remove gravity; ignore tiny values so noise doesn't build up into speed
  float upAccel = along - upMag;
  if (fabsf(upAccel) < 0.03f) upAccel = 0.0f;
  g_kinematics.upAccel = upAccel;

  // Speed = running total of acceleration, slowly leaking back to 0 to limit drift
  g_kinematics.upVelocity += upAccel * 9.81f * dt_sec;
  g_kinematics.upVelocity *= 0.99f;

  if (g_fsmState == STATE_IDLE) {
    // While resting: ignore movement while "up" is still adjusting to a new arm position, and settle speed to 0
    if (g_kinematics.sideAccel > 0.30f || upMag < 0.85f) {
      g_kinematics.upVelocity = 0.0f;
    } else if (upAccel == 0.0f) {
      g_kinematics.upVelocity *= 0.80f;
    }
    g_kinematics.heightCm = 0.0f;
  } else {
    // Height = running total of speed during the rep
    g_kinematics.heightCm += g_kinematics.upVelocity * dt_sec * 100.0f;
  }
}

// form quality
void updateLiveQuality() {
  if (g_fsmState == STATE_IDLE) {
    g_kinematics.liveQuality = 100.0f;
    return;
  }

  float instantQ = 100.0f;

  if (g_kinematics.jerk > 2.0f) {
    float jerkExcess = g_kinematics.jerk - 2.0f;
    instantQ -= (jerkExcess * 16.0f);
    if (g_kinematics.jerk > 4.5f) {
      g_currentRep.flagHighJerk = true;
    }
  }

  if (g_exercise == EXERCISE_PRESS) {
    // Lowering too fast (m/s)
    if (g_kinematics.upVelocity < -0.50f) {
      float dropExcess = (-g_kinematics.upVelocity) - 0.50f;
      instantQ -= (dropExcess * 150.0f);
      if (g_kinematics.upVelocity < -0.80f) {
        g_currentRep.flagFastDrop = true;
      }
    }

    // Swaying or pressing at an angle
    if (g_kinematics.sideAccel > 0.30f) {
      instantQ -= (g_kinematics.sideAccel - 0.30f) * 120.0f;
    }
  } else {
    if (g_kinematics.angularVelocity < -90.0f) {
      float dropExcess = (-g_kinematics.angularVelocity) - 90.0f;
      instantQ -= (dropExcess * 0.70f);
      if (g_kinematics.angularVelocity < -160.0f) {
        g_currentRep.flagFastDrop = true;
      }
    }

    float lateralDev = fabsf(g_sensor.filtered_ay - restY_g);
    if (lateralDev > 0.45f) {
      instantQ -= (lateralDev - 0.45f) * 60.0f;
    }
  }

  if (instantQ < 0.0f) instantQ = 0.0f;
  if (instantQ > 100.0f) instantQ = 100.0f;

  if (instantQ < g_kinematics.liveQuality) {
    g_kinematics.liveQuality = (0.60f * g_kinematics.liveQuality) + (0.40f * instantQ);
  } else {
    g_kinematics.liveQuality = (0.75f * g_kinematics.liveQuality) + (0.25f * instantQ);
  }

  if (g_fsmState == STATE_CONCENTRIC || g_fsmState == STATE_PEAK || g_fsmState == STATE_ECCENTRIC) {
    g_currentRep.qualitySum += g_kinematics.liveQuality;
    g_currentRep.qualitySampleCount++;
  }
}

// neopix feedback
void updateNeoPixelFeedback() {
  uint32_t now = millis();

  if (now < g_completionFlashUntilMs) {
    if (g_lastCompletedRep.finalQualityFactor >= 75.0f) {
      setNeoPixelColor(0, 45, 0);
    } else {
      setNeoPixelColor(45, 18, 0);
    }
    return;
  }

  if (g_fsmState == STATE_IDLE) {
    static uint8_t pulse = 4;
    static bool dir = true;
    if (dir) {
      if (++pulse >= 20) dir = false;
    } else {
      if (--pulse <= 4) dir = true;
    }
    setNeoPixelColor(0, 0, pulse);
    return;
  }

  float q = g_kinematics.liveQuality;
  if (q < 0.0f) q = 0.0f;
  if (q > 100.0f) q = 100.0f;

  uint8_t r = 0, g = 0, b = 0;
  constexpr uint8_t MAX_BRIGHTNESS = 45;

  if (q >= 50.0f) {
    float ratio = (100.0f - q) / 50.0f;
    r = (uint8_t)(ratio * MAX_BRIGHTNESS);
    g = MAX_BRIGHTNESS;
    b = 0;
  } else {
    float ratio = q / 50.0f;
    r = MAX_BRIGHTNESS;
    g = (uint8_t)(ratio * MAX_BRIGHTNESS);
    b = 0;
  }

  setNeoPixelColor(r, g, b);
}

void printRepSummary();

// Curl FSM (forearm angle)
void processCurlFSM(uint32_t now) {
  switch (g_fsmState) {
    case STATE_IDLE: {
      if (g_kinematics.angleDeg > 28.0f && g_kinematics.angularVelocity > 15.0f) {
        g_fsmState = STATE_CONCENTRIC;
        g_currentRep = {0};
        g_currentRep.exercise = EXERCISE_CURL;
        g_currentRep.startTimeMs = now;
        g_currentRep.peakAngleDeg = g_kinematics.angleDeg;
        g_kinematics.liveQuality = 100.0f;
        if (!g_paused && !g_plotterMode) {
          Serial.println("\r\n[REP STARTED]");
        }
      }
      break;
    }

    case STATE_CONCENTRIC: {
      if (g_kinematics.angleDeg > g_currentRep.peakAngleDeg) {
        g_currentRep.peakAngleDeg = g_kinematics.angleDeg;
      }

      bool reachedPeak = (g_kinematics.angleDeg >= 70.0f && g_kinematics.angularVelocity <= 12.0f);
      bool alreadyDescending = (g_currentRep.peakAngleDeg >= 68.0f && g_kinematics.angleDeg <= (g_currentRep.peakAngleDeg - 5.0f) && g_kinematics.angularVelocity < 0.0f);

      if (reachedPeak || alreadyDescending) {
        g_fsmState = STATE_PEAK;
        g_currentRep.peakTimeMs = now;
        g_currentRep.concentricDurationSec = (float)(now - g_currentRep.startTimeMs) / 1000.0f;
      }

      if (g_kinematics.angleDeg < 20.0f) {
        g_fsmState = STATE_IDLE;
        if (!g_paused && !g_plotterMode) {
          Serial.printf("\r\n[REP ABORTED] Peak reached: %.1f° (needed >= 68°)\r\n", g_currentRep.peakAngleDeg);
        }
      }

      if (now - g_currentRep.startTimeMs > 6000) {
        g_fsmState = STATE_IDLE;
        if (!g_paused && !g_plotterMode) {
          Serial.println("\r\n[REP TIMEOUT]");
        }
      }
      break;
    }

    case STATE_PEAK: {
      if (g_kinematics.angleDeg > g_currentRep.peakAngleDeg) {
        g_currentRep.peakAngleDeg = g_kinematics.angleDeg;
      }

      if (g_kinematics.angleDeg < (g_currentRep.peakAngleDeg - 8.0f) && g_kinematics.angularVelocity < -5.0f) {
        g_fsmState = STATE_ECCENTRIC;
      }
      break;
    }

    case STATE_ECCENTRIC: {
      if (g_kinematics.angleDeg < 22.0f) {
        g_fsmState = STATE_COMPLETED;
        g_currentRep.completeTimeMs = now;
        g_currentRep.eccentricDurationSec = (float)(now - g_currentRep.peakTimeMs) / 1000.0f;
      }

      if (now - g_currentRep.startTimeMs > 8000) {
        g_fsmState = STATE_IDLE;
        if (!g_paused && !g_plotterMode) {
          Serial.println("\r\n[REP TIMEOUT]");
        }
      }
      break;
    }

    case STATE_COMPLETED:
      break;
  }
}

// Shoulder press FSM (vertical speed and height)
void processPressFSM(uint32_t now) {
  float vel = g_kinematics.upVelocity;
  float height = g_kinematics.heightCm;

  switch (g_fsmState) {
    case STATE_IDLE: {
      // Moving up, with the arm in its learned "up" position (not being repositioned)
      if (vel > 0.15f && g_kinematics.sideAccel < 0.30f) {
        g_fsmState = STATE_CONCENTRIC;
        g_currentRep = {0};
        g_currentRep.exercise = EXERCISE_PRESS;
        g_currentRep.startTimeMs = now;
        g_kinematics.liveQuality = 100.0f;
        if (!g_paused && !g_plotterMode) {
          Serial.println("\r\n[REP STARTED]");
        }
      }
      break;
    }

    case STATE_CONCENTRIC: {
      if (height > g_currentRep.peakHeightCm) {
        g_currentRep.peakHeightCm = height;
      }

      // Pushing has stopped: the top, if the arm went far enough
      if (vel < 0.05f) {
        if (g_currentRep.peakHeightCm >= 12.0f) {
          g_fsmState = STATE_PEAK;
          g_currentRep.peakTimeMs = now;
          g_currentRep.concentricDurationSec = (float)(now - g_currentRep.startTimeMs) / 1000.0f;
        } else {
          g_fsmState = STATE_IDLE;
          if (!g_paused && !g_plotterMode) {
            Serial.printf("\r\n[REP ABORTED] Pressed %.0f cm (needed >= 12 cm)\r\n", g_currentRep.peakHeightCm);
          }
        }
      }

      if (g_fsmState == STATE_CONCENTRIC && now - g_currentRep.startTimeMs > 6000) {
        g_fsmState = STATE_IDLE;
        if (!g_paused && !g_plotterMode) {
          Serial.println("\r\n[REP TIMEOUT]");
        }
      }
      break;
    }

    case STATE_PEAK: {
      if (height > g_currentRep.peakHeightCm) {
        g_currentRep.peakHeightCm = height;
      }

      if (vel < -0.15f) {
        g_fsmState = STATE_ECCENTRIC;
      }

      if (now - g_currentRep.startTimeMs > 10000) {
        g_fsmState = STATE_IDLE;
        if (!g_paused && !g_plotterMode) {
          Serial.println("\r\n[REP TIMEOUT]");
        }
      }
      break;
    }

    case STATE_ECCENTRIC: {
      // Stopped lowering, at least halfway back down
      if (vel > -0.05f && height < (g_currentRep.peakHeightCm * 0.5f)) {
        g_fsmState = STATE_COMPLETED;
        g_currentRep.completeTimeMs = now;
        g_currentRep.eccentricDurationSec = (float)(now - g_currentRep.peakTimeMs) / 1000.0f;
      }

      if (now - g_currentRep.startTimeMs > 10000) {
        g_fsmState = STATE_IDLE;
        if (!g_paused && !g_plotterMode) {
          Serial.println("\r\n[REP TIMEOUT]");
        }
      }
      break;
    }

    case STATE_COMPLETED:
      break;
  }
}

// Rep scoring (shared by all exercises)
void completeRep(uint32_t now) {
  g_repCount++;
  g_currentRep.repNumber = g_repCount;

  float avgLiveQ = (g_currentRep.qualitySampleCount > 0)
                     ? (g_currentRep.qualitySum / g_currentRep.qualitySampleCount)
                     : 80.0f;

  float romScore = 100.0f;
  if (g_currentRep.exercise == EXERCISE_PRESS) {
    if (g_currentRep.peakHeightCm < 30.0f) {
      romScore -= (30.0f - g_currentRep.peakHeightCm) * 3.0f;
      g_currentRep.flagLowROM = true;
    }
  } else {
    if (g_currentRep.peakAngleDeg < 85.0f) {
      romScore -= (85.0f - g_currentRep.peakAngleDeg) * 3.0f;
      g_currentRep.flagLowROM = true;
    }
  }
  if (romScore < 0.0f) romScore = 0.0f;

  g_currentRep.finalQualityFactor = (0.65f * avgLiveQ) + (0.35f * romScore);
  if (g_currentRep.finalQualityFactor > 100.0f) g_currentRep.finalQualityFactor = 100.0f;
  if (g_currentRep.finalQualityFactor < 0.0f) g_currentRep.finalQualityFactor = 0.0f;

  g_currentRep.avgBpm = getDisplayBpm();
  g_currentRep.avgSpo2 = getDisplaySpo2();

  g_lastCompletedRep = g_currentRep;

  if (g_repHistoryCount < MAX_REP_HISTORY) {
    g_repHistory[g_repHistoryCount++] = g_currentRep;
  } else {
    for (size_t i = 0; i < MAX_REP_HISTORY - 1; i++) {
      g_repHistory[i] = g_repHistory[i + 1];
    }
    g_repHistory[MAX_REP_HISTORY - 1] = g_currentRep;
  }

  printRepSummary();

  g_completionFlashUntilMs = now + 400;
  g_fsmState = STATE_IDLE;
}

// Rep FSM
void processRepFSM() {
  uint32_t now = millis();

  if (g_fsmState == STATE_COMPLETED) {
    completeRep(now);
  } else if (g_exercise == EXERCISE_PRESS) {
    processPressFSM(now);
  } else {
    processCurlFSM(now);
  }
}

// Rep summary logging
void printRepSummary() {
  if (g_paused || g_plotterMode) return;

  Serial.println();
  Serial.println("=========================================");
  Serial.printf("REP #%u COMPLETED\r\n", g_lastCompletedRep.repNumber);
  Serial.println("=========================================");
  Serial.printf("Exercise:            %s\r\n", exerciseName(g_lastCompletedRep.exercise));
  Serial.printf("Quality:             %.1f%%\r\n", g_lastCompletedRep.finalQualityFactor);
  if (g_lastCompletedRep.exercise == EXERCISE_PRESS) {
    Serial.printf("Press Height:        %.0f cm (estimated)\r\n", g_lastCompletedRep.peakHeightCm);
  } else {
    Serial.printf("Peak Angle:          %.1f deg\r\n", g_lastCompletedRep.peakAngleDeg);
  }
  Serial.printf("Concentric Duration: %.2f s\r\n", g_lastCompletedRep.concentricDurationSec);
  Serial.printf("Eccentric Duration:  %.2f s\r\n", g_lastCompletedRep.eccentricDurationSec);
  if (g_lastCompletedRep.avgBpm > 0) {
    Serial.printf("Heart Rate:          %d bpm (SpO2: %d%%)\r\n", g_lastCompletedRep.avgBpm, g_lastCompletedRep.avgSpo2);
  }
  Serial.printf("Core Temp:           %.1f °C\r\n", getCoreTemp());

  Serial.print("Diagnostics:         ");
  bool hasFlag = false;
  if (g_lastCompletedRep.flagLowROM) {
    Serial.print("[Low ROM] ");
    hasFlag = true;
  }
  if (g_lastCompletedRep.flagFastDrop) {
    Serial.print("[Lowered too fast] ");
    hasFlag = true;
  }
  if (g_lastCompletedRep.flagHighJerk) {
    Serial.print("[Jerky lift] ");
    hasFlag = true;
  }
  if (!hasFlag) {
    Serial.print("[Clean]");
  }
  Serial.println("\r\n=========================================\r\n");
}

// telemetry
void printTelemetry() {
  if (!Serial) return;
  if (g_paused || !g_streamTelemetry) return;

  int curBpm = getDisplayBpm();
  int curSpo2 = getDisplaySpo2();
  float coreTemp = getCoreTemp();

  if (g_plotterMode) {
    Serial.printf("Angle:%.1f,Vel:%.1f,Jerk:%.1f,Q:%.1f,UpVel:%.2f,Height:%.1f,BPM:%d,SpO2:%d,Temp:%.1f\r\n",
                  g_kinematics.angleDeg,
                  g_kinematics.angularVelocity,
                  g_kinematics.jerk,
                  g_kinematics.liveQuality,
                  g_kinematics.upVelocity,
                  g_kinematics.heightCm,
                  curBpm,
                  curSpo2,
                  coreTemp);
    return;
  }

  const char* stateStr = "IDLE";
  if (g_fsmState == STATE_CONCENTRIC) stateStr = "LIFTING";
  else if (g_fsmState == STATE_PEAK)  stateStr = "PEAK";
  else if (g_fsmState == STATE_ECCENTRIC) stateStr = "LOWERING";

  char pulseStr[36];
  if (!g_pulseOx.connected) {
    snprintf(pulseStr, sizeof(pulseStr), "HR: [Detached]");
  } else if (!g_pulseOx.fingerPresent) {
    snprintf(pulseStr, sizeof(pulseStr), "HR: [No Finger]");
  } else if (curBpm > 0) {
    snprintf(pulseStr, sizeof(pulseStr), "HR: %3d bpm | SpO2: %2d%%", curBpm, curSpo2);
  } else {
    snprintf(pulseStr, sizeof(pulseStr), "HR: [Measuring...]");
  }

  char motionStr[48];
  if (g_exercise == EXERCISE_PRESS) {
    snprintf(motionStr, sizeof(motionStr), "Height: %5.1fcm | Vel: %+5.2fm/s",
             g_kinematics.heightCm, g_kinematics.upVelocity);
  } else {
    snprintf(motionStr, sizeof(motionStr), "Angle: %5.1f° | Vel: %+6.1f°/s",
             g_kinematics.angleDeg, g_kinematics.angularVelocity);
  }

  Serial.printf("%s | Jerk: %4.1fg/s | Q: %5.1f%% | Reps: %2u | [%s] | %s | Temp: %4.1f°C\r\n",
                motionStr,
                g_kinematics.jerk,
                g_kinematics.liveQuality,
                g_repCount,
                stateStr,
                pulseStr,
                coreTemp);
}

// calibration
void calibrateRestPosition() {
  isCalibrated = false;
  Serial.println("\r\n[CALIBRATING] Keep your arm hanging straight and still at your side...");
  showOledCalibrating();
  setNeoPixelColor(30, 30, 0);

  float sumMvX = 0, sumMvY = 0, sumMvZ = 0;
  constexpr int SAMPLES = 100;
  for (int i = 0; i < SAMPLES; i++) {
    uint32_t sx = 0, sy = 0, sz = 0;
    for (int k = 0; k < 8; k++) {
      sx += analogReadMilliVolts(PIN_XOUT);
      sy += analogReadMilliVolts(PIN_YOUT);
      sz += analogReadMilliVolts(PIN_ZOUT);
    }
    sumMvX += (float)(sx / 8);
    sumMvY += (float)(sy / 8);
    sumMvZ += (float)(sz / 8);
    delay(10);
  }

  float avgMvX = sumMvX / SAMPLES;
  float avgMvY = sumMvY / SAMPLES;
  float avgMvZ = sumMvZ / SAMPLES;

  float diffX = fabsf(avgMvX - 1745.0f);
  float diffY = fabsf(avgMvY - 1745.0f);
  float diffZ = fabsf(avgMvZ - 1745.0f);

  if (diffZ >= diffX && diffZ >= diffY) {
    g_zeroG_X_mV = avgMvX;
    g_zeroG_Y_mV = avgMvY;
    g_zeroG_Z_mV = (g_zeroG_X_mV + g_zeroG_Y_mV) / 2.0f;
    float measuredSens = fabsf(avgMvZ - g_zeroG_Z_mV);
    if (measuredSens >= 250.0f && measuredSens <= 380.0f) {
      g_sensitivity_mV_per_g = measuredSens;
    }
  } else if (diffX >= diffY && diffX >= diffZ) {
    g_zeroG_Y_mV = avgMvY;
    g_zeroG_Z_mV = avgMvZ;
    g_zeroG_X_mV = (g_zeroG_Y_mV + g_zeroG_Z_mV) / 2.0f;
    float measuredSens = fabsf(avgMvX - g_zeroG_X_mV);
    if (measuredSens >= 250.0f && measuredSens <= 380.0f) {
      g_sensitivity_mV_per_g = measuredSens;
    }
  } else {
    g_zeroG_X_mV = avgMvX;
    g_zeroG_Z_mV = avgMvZ;
    g_zeroG_Y_mV = (g_zeroG_X_mV + g_zeroG_Z_mV) / 2.0f;
    float measuredSens = fabsf(avgMvY - g_zeroG_Y_mV);
    if (measuredSens >= 250.0f && measuredSens <= 380.0f) {
      g_sensitivity_mV_per_g = measuredSens;
    }
  }

  restX_g = (avgMvX - g_zeroG_X_mV) / g_sensitivity_mV_per_g;
  restY_g = (avgMvY - g_zeroG_Y_mV) / g_sensitivity_mV_per_g;
  restZ_g = (avgMvZ - g_zeroG_Z_mV) / g_sensitivity_mV_per_g;

  g_sensor.ax_g = restX_g;
  g_sensor.ay_g = restY_g;
  g_sensor.az_g = restZ_g;
  g_sensor.filtered_ax = restX_g;
  g_sensor.filtered_ay = restY_g;
  g_sensor.filtered_az = restZ_g;

  setNeoPixelColor(0, 40, 0);
  showOledCalibrating();
  delay(1200);
  isCalibrated = true;
  g_fsmState = STATE_IDLE;
  g_kinematics = {0};
  g_currentRep = {0};
  resetPressTracking();

  float magRest = sqrtf(restX_g * restX_g + restY_g * restY_g + restZ_g * restZ_g);
  Serial.println("[CALIBRATION COMPLETE] Arm rest baseline locked:");
  Serial.printf("  0g Biases   -> X: %.1fmV, Y: %.1fmV, Z: %.1fmV\r\n", g_zeroG_X_mV, g_zeroG_Y_mV, g_zeroG_Z_mV);
  Serial.printf("  Sensitivity -> %.1f mV/g\r\n", g_sensitivity_mV_per_g);
  Serial.printf("  Rest Vector -> X: %+.2fg, Y: %+.2fg, Z: %+.2fg (Total: %.2fg)\r\n\r\n", restX_g, restY_g, restZ_g, magRest);
}

// Help menu
void printHelp() {
  Serial.println();
  Serial.println("=========================================");
  Serial.println("                Commands                 ");
  Serial.println("=========================================");
  Serial.println("  's' or ' ' -> Pause / Resume output");
  Serial.println("  'p'        -> Toggle Serial Plotter mode");
  Serial.println("  'c'        -> Calibrate arm hang position");
  Serial.println("  'r'        -> Reset rep count to 0");
  Serial.println("  'e'        -> Switch exercise (curl / shoulder press)");
  Serial.println("  'o'        -> Pulse Oximeter status");
  Serial.println("  'g'        -> Toggle Google Sheets logging");
  Serial.println("  'w'        -> Show Wi-Fi / IP connection status");
  Serial.println("  'd'        -> Toggle performance debug metrics");
  Serial.println("  'h'        -> Show this menu");
  Serial.println("=========================================");
  Serial.println();
}

// Web server
void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleApiData() {
  const char* stateStr = "IDLE";
  if (g_fsmState == STATE_CONCENTRIC) stateStr = "LIFTING";
  else if (g_fsmState == STATE_PEAK)  stateStr = "PEAK";
  else if (g_fsmState == STATE_ECCENTRIC) stateStr = "LOWERING";

  String json = "{";
  json += "\"exercise\":\"" + String(g_exercise == EXERCISE_PRESS ? "press" : "curl") + "\",";
  json += "\"angle\":" + String(g_kinematics.angleDeg, 1) + ",";
  json += "\"height\":" + String(g_kinematics.heightCm, 1) + ",";
  json += "\"upVel\":" + String(g_kinematics.upVelocity, 2) + ",";
  json += "\"vel\":" + String(g_kinematics.angularVelocity, 1) + ",";
  json += "\"jerk\":" + String(g_kinematics.jerk, 1) + ",";
  json += "\"q\":" + String(g_kinematics.liveQuality, 1) + ",";
  json += "\"reps\":" + String(g_repCount) + ",";
  json += "\"state\":\"" + String(stateStr) + "\",";
  json += "\"bpm\":" + String(getDisplayBpm()) + ",";
  json += "\"spo2\":" + String(getDisplaySpo2()) + ",";
  json += "\"temp\":" + String(getCoreTemp(), 1) + ",";
  json += "\"finger\":" + String(g_pulseOx.fingerPresent ? "true" : "false") + ",";
  json += "\"pulseOxConnected\":" + String(g_pulseOx.connected ? "true" : "false") + ",";
  json += "\"pulseOxStatus\":\"" + String(getPulseOxStatusStr()) + "\",";
  json += "\"wifiConnected\":" + String((WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
  json += "\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Connecting...")) + "\",";
  json += "\"sheetsLogging\":" + String(ENABLE_GOOGLE_SHEETS_LOGGING ? "true" : "false") + ",";
  json += "\"history\":[";

  for (size_t i = 0; i < g_repHistoryCount; i++) {
    if (i > 0) json += ",";
    String diag = "";
    if (g_repHistory[i].flagLowROM) diag += "Low ROM ";
    if (g_repHistory[i].flagFastDrop) diag += "Lowered too fast ";
    if (g_repHistory[i].flagHighJerk) diag += "Jerky lift ";
    if (diag.length() == 0) diag = "Clean";

    bool isPress = (g_repHistory[i].exercise == EXERCISE_PRESS);
    json += "{\"rep\":" + String(g_repHistory[i].repNumber) + ",";
    json += "\"ex\":\"" + String(isPress ? "press" : "curl") + "\",";
    json += "\"q\":" + String(g_repHistory[i].finalQualityFactor, 1) + ",";
    json += "\"peak\":" + String(isPress ? g_repHistory[i].peakHeightCm : g_repHistory[i].peakAngleDeg, 1) + ",";
    json += "\"con\":" + String(g_repHistory[i].concentricDurationSec, 2) + ",";
    json += "\"ecc\":" + String(g_repHistory[i].eccentricDurationSec, 2) + ",";
    json += "\"bpm\":" + String(g_repHistory[i].avgBpm) + ",";
    json += "\"spo2\":" + String(g_repHistory[i].avgSpo2) + ",";
    json += "\"diag\":\"" + diag + "\"}";
  }
  json += "]}";

  server.send(200, "application/json", json);
}

// ws streaming
void broadcastWsData(bool forceHistory = false) {
  if (webSocket.connectedClients() == 0) return;

  const char* stateStr = "IDLE";
  if (g_fsmState == STATE_CONCENTRIC) stateStr = "LIFTING";
  else if (g_fsmState == STATE_PEAK)  stateStr = "PEAK";
  else if (g_fsmState == STATE_ECCENTRIC) stateStr = "LOWERING";

  String json = "{";
  json += "\"exercise\":\"" + String(g_exercise == EXERCISE_PRESS ? "press" : "curl") + "\",";
  json += "\"angle\":" + String(g_kinematics.angleDeg, 1) + ",";
  json += "\"height\":" + String(g_kinematics.heightCm, 1) + ",";
  json += "\"upVel\":" + String(g_kinematics.upVelocity, 2) + ",";
  json += "\"vel\":" + String(g_kinematics.angularVelocity, 1) + ",";
  json += "\"jerk\":" + String(g_kinematics.jerk, 1) + ",";
  json += "\"q\":" + String(g_kinematics.liveQuality, 1) + ",";
  json += "\"reps\":" + String(g_repCount) + ",";
  json += "\"state\":\"" + String(stateStr) + "\",";
  json += "\"bpm\":" + String(getDisplayBpm()) + ",";
  json += "\"spo2\":" + String(getDisplaySpo2()) + ",";
  json += "\"temp\":" + String(getCoreTemp(), 1) + ",";
  json += "\"finger\":" + String(g_pulseOx.fingerPresent ? "true" : "false") + ",";
  json += "\"pulseOxConnected\":" + String(g_pulseOx.connected ? "true" : "false") + ",";
  json += "\"pulseOxStatus\":\"" + String(getPulseOxStatusStr()) + "\",";
  json += "\"wifiConnected\":" + String((WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
  json += "\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Connecting...")) + "\",";
  json += "\"sheetsLogging\":" + String(ENABLE_GOOGLE_SHEETS_LOGGING ? "true" : "false");

  bool includeHistory = forceHistory || (g_repCount != s_lastWsRepCount) || (g_repHistoryCount != s_lastWsRepHistoryCount);
  if (includeHistory) {
    s_lastWsRepCount = g_repCount;
    s_lastWsRepHistoryCount = g_repHistoryCount;
    json += ",\"history\":[";
    for (size_t i = 0; i < g_repHistoryCount; i++) {
      if (i > 0) json += ",";
      String diag = "";
      if (g_repHistory[i].flagLowROM) diag += "Low ROM ";
      if (g_repHistory[i].flagFastDrop) diag += "Lowered too fast ";
      if (g_repHistory[i].flagHighJerk) diag += "Jerky lift ";
      if (diag.length() == 0) diag = "Clean";

      bool isPress = (g_repHistory[i].exercise == EXERCISE_PRESS);
      json += "{\"rep\":" + String(g_repHistory[i].repNumber) + ",";
      json += "\"ex\":\"" + String(isPress ? "press" : "curl") + "\",";
      json += "\"q\":" + String(g_repHistory[i].finalQualityFactor, 1) + ",";
      json += "\"peak\":" + String(isPress ? g_repHistory[i].peakHeightCm : g_repHistory[i].peakAngleDeg, 1) + ",";
      json += "\"con\":" + String(g_repHistory[i].concentricDurationSec, 2) + ",";
      json += "\"ecc\":" + String(g_repHistory[i].eccentricDurationSec, 2) + ",";
      json += "\"bpm\":" + String(g_repHistory[i].avgBpm) + ",";
      json += "\"spo2\":" + String(g_repHistory[i].avgSpo2) + ",";
      json += "\"diag\":\"" + diag + "\"}";
    }
    json += "]";
  }
  json += "}";

  webSocket.broadcastTXT(json);
}

// Exercise selection
void setExercise(ExerciseType ex) {
  g_exercise = ex;
  g_fsmState = STATE_IDLE;
  g_currentRep = {0};
  resetPressTracking();
  Serial.printf("\r\n[EXERCISE] Now tracking: %s\r\n", exerciseName(ex));
  if (ex == EXERCISE_PRESS) {
    Serial.println("[EXERCISE] Hold your hands at your shoulders for ~2 s before the first press.");
  }
  broadcastWsData(false);
}

void toggleExercise() {
  setExercise(g_exercise == EXERCISE_PRESS ? EXERCISE_CURL : EXERCISE_PRESS);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      break;
    case WStype_CONNECTED:
      broadcastWsData(true);
      break;
    case WStype_TEXT: {
      String cmd = "";
      if (payload && length > 0) {
        cmd.reserve(length);
        for (size_t i = 0; i < length; i++) {
          cmd += (char)payload[i];
        }
        cmd.trim();
      }
      if (cmd == "calibrate") {
        calibrateRestPosition();
      } else if (cmd == "reset") {
        g_repCount = 0;
        g_repHistoryCount = 0;
        g_lastCompletedRep = {0};
        s_lastWsRepCount = 0;
        s_lastWsRepHistoryCount = 0;
        Serial.println("\r\n[RESET] Rep count reset to 0.");
        broadcastWsData(true);
      } else if (cmd == "sheets_toggle") {
        ENABLE_GOOGLE_SHEETS_LOGGING = !ENABLE_GOOGLE_SHEETS_LOGGING;
        Serial.printf("\r\n[SHEETS] Google Sheets logging toggled: %s\r\n", ENABLE_GOOGLE_SHEETS_LOGGING ? "ENABLED" : "DISABLED");
        broadcastWsData(false);
      } else if (cmd == "exercise_toggle") {
        toggleExercise();
      }
      break;
    }
    default:
      break;
  }
}

void handleApiCommand() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    if (action == "calibrate") {
      calibrateRestPosition();
    } else if (action == "reset") {
      g_repCount = 0;
      g_repHistoryCount = 0;
      g_lastCompletedRep = {0};
      s_lastWsRepCount = 0;
      s_lastWsRepHistoryCount = 0;
      Serial.println("\r\n[RESET] Rep count reset to 0.");
      broadcastWsData(true);
    } else if (action == "sheets_toggle") {
      ENABLE_GOOGLE_SHEETS_LOGGING = !ENABLE_GOOGLE_SHEETS_LOGGING;
      Serial.printf("\r\n[SHEETS] Google Sheets logging toggled: %s\r\n", ENABLE_GOOGLE_SHEETS_LOGGING ? "ENABLED" : "DISABLED");
      broadcastWsData(false);
    } else if (action == "exercise_toggle") {
      toggleExercise();
    }
  }
  server.send(200, "text/plain", "OK");
}

// Setup
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  pinMode(PIN_XOUT, INPUT);
  pinMode(PIN_YOUT, INPUT);
  pinMode(PIN_ZOUT, INPUT);

  analogReadResolution(12);

  setNeoPixelColor(0, 0, 20);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setTimeOut(10);
  initOled();
  showOledBootProgress("WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println();
  Serial.printf("[WIFI] Connecting to '%s'", WIFI_SSID);

  uint32_t t0Wifi = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0Wifi < 8000)) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    Serial.println(F("[WIFI] Connected!"));
    Serial.printf("[WIFI] Obtained IP:  %s\r\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] Gateway:      %s\r\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("[WIFI] Signal RSSI:  %d dBm\r\n", WiFi.RSSI());
  } else {
    Serial.printf("[WIFI] Could not connect to '%s' within 8s. Continuing in background...\r\n", WIFI_SSID);
  }

  showOledBootProgress("Server");
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleApiData);
  server.on("/api/cmd", HTTP_GET, handleApiCommand);
  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  Serial.println();
  Serial.println("=========================================================");
  Serial.println("    honk honk honk honk honk honk honk honk honk honk    ");
  Serial.println("=========================================================");
  Serial.printf("Wi-Fi Network:      %s\r\n", WIFI_SSID);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Obtained IP:        %s\r\n", WiFi.localIP().toString().c_str());
    Serial.printf("Dashboard Webpage:  http://%s\r\n", WiFi.localIP().toString().c_str());
    Serial.printf("WebSocket Server:   ws://%s:81/\r\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("Obtained IP:        [Connecting...]");
    Serial.println("Dashboard Webpage:  [Awaiting IP]");
  }
  Serial.printf("Google Sheets Log:  %s\r\n", ENABLE_GOOGLE_SHEETS_LOGGING ? "ENABLED" : "DISABLED");
  Serial.println("=========================================================");
  printHelp();

  showOledBootProgress("Sensors");
  initPulseOx();
  PerfDebug::initPerfHooks();
  delay(100);

  showOledBootProgress("Calibrate");
  delay(300);
  calibrateRestPosition();
}

// Loop
void loop() {
  uint32_t now = millis();

  server.handleClient();
  webSocket.loop();
  updatePulseOx();

  while (Serial.available() > 0) {
    char cmd = (char)Serial.read();
    if (cmd == '\r' || cmd == '\n') continue;
    switch (cmd) {
      case ' ':
      case 's':
      case 'S':
        g_paused = !g_paused;
        Serial.printf("\r\n[OUTPUT] %s\r\n", g_paused ? "PAUSED" : "RESUMED");
        break;
      case 'p':
      case 'P':
        g_plotterMode = !g_plotterMode;
        if (!g_plotterMode) {
          Serial.println("\r\n[MODE] Switched to Terminal Table mode");
        }
        break;
      case 'c':
      case 'C':
        calibrateRestPosition();
        break;
      case 'r':
      case 'R':
        g_repCount = 0;
        g_repHistoryCount = 0;
        g_lastCompletedRep = {0};
        s_lastWsRepCount = 0;
        s_lastWsRepHistoryCount = 0;
        Serial.println("\r\n[RESET] Rep count reset to 0.");
        broadcastWsData(true);
        break;
      case 'o':
      case 'O':
        Serial.println("\r\n=========================================");
        Serial.println("         Pulse Oximeter Status           ");
        Serial.println("=========================================");
        Serial.printf("  Hardware Connected: %s\r\n", g_pulseOx.connected ? "YES" : "NO");
        Serial.printf("  Status:             %s\r\n", getPulseOxStatusStr());
        Serial.printf("  Finger Present:     %s\r\n", g_pulseOx.fingerPresent ? "YES" : "NO");
        Serial.printf("  Heart Rate:         %d bpm\r\n", getDisplayBpm());
        Serial.printf("  SpO2:               %d%%\r\n", getDisplaySpo2());
        Serial.printf("  Raw IR:             %u\r\n", g_pulseOx.rawIR);
        Serial.printf("  Raw Red:            %u\r\n", g_pulseOx.rawRed);
        Serial.printf("  ESP32-S3 Core Temp: %.1f °C\r\n", getCoreTemp());
        Serial.printf("  Google Sheets Log:  %s\r\n", ENABLE_GOOGLE_SHEETS_LOGGING ? "ENABLED" : "DISABLED");
        Serial.printf("  Wi-Fi Connection:   %s\r\n", (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString().c_str() : "Disconnected");
        Serial.println("=========================================\r\n");
        break;
      case 'g':
      case 'G':
        ENABLE_GOOGLE_SHEETS_LOGGING = !ENABLE_GOOGLE_SHEETS_LOGGING;
        Serial.printf("\r\n[SHEETS] Google Sheets logging %s\r\n", ENABLE_GOOGLE_SHEETS_LOGGING ? "ENABLED" : "DISABLED");
        break;
      case 'w':
      case 'W':
        triggerIpScreen(5000);
        Serial.println("\r\n=========================================");
        Serial.println("              Wi-Fi Status               ");
        Serial.println("=========================================");
        Serial.printf("  Configured SSID: %s\r\n", WIFI_SSID);
        Serial.printf("  Status:          %s\r\n", (WiFi.status() == WL_CONNECTED) ? "CONNECTED" : "DISCONNECTED");
        if (WiFi.status() == WL_CONNECTED) {
          Serial.printf("  IP Address:      %s\r\n", WiFi.localIP().toString().c_str());
          Serial.printf("  Gateway:         %s\r\n", WiFi.gatewayIP().toString().c_str());
          Serial.printf("  Subnet Mask:     %s\r\n", WiFi.subnetMask().toString().c_str());
          Serial.printf("  DNS:             %s\r\n", WiFi.dnsIP().toString().c_str());
          Serial.printf("  Signal (RSSI):   %d dBm\r\n", WiFi.RSSI());
          Serial.printf("  Dashboard URL:   http://%s\r\n", WiFi.localIP().toString().c_str());
        }
        Serial.printf("  Google Sheets:   %s\r\n", ENABLE_GOOGLE_SHEETS_LOGGING ? "ENABLED" : "DISABLED");
        Serial.printf("  Touch 11 (IP):   Raw: %u | Baseline: %u | Touched: %s\r\n",
                      g_lastTouchVal, g_touchBaseline, checkTouchIpSensor() ? "YES" : "NO");
        Serial.println("=========================================\r\n");
        break;
      case 'd':
      case 'D':
        PerfDebug::toggle();
        break;
      case 'e':
      case 'E':
        toggleExercise();
        break;
      case 'h':
      case 'H':
        printHelp();
        break;
      default:
        Serial.printf("[UNKNOWN] '%c'. Type 'h' for menu.\r\n", cmd);
        break;
    }
  }

  // Kinematics loop
  if (now - g_lastSampleTimeMs >= SAMPLE_INTERVAL_MS) {
    uint32_t sampleStartUs = micros();
    float dt_sec = (float)(now - g_lastSampleTimeMs) / 1000.0f;
    g_lastSampleTimeMs = now;

    sampleAccelerometer();
    computeKinematics(dt_sec);
    computeLinearKinematics(dt_sec);
    updateLiveQuality();
    processRepFSM();
    updateNeoPixelFeedback();

    PerfDebug::recordKinematicsTiming(dt_sec * 1000.0f, micros() - sampleStartUs);
  }

  // Telemetry loop
  if (now - g_lastTelemetryPrintMs >= 100) {
    g_lastTelemetryPrintMs = now;
    printTelemetry();
  }

  // WebSocket broadcast (20 Hz)
  static uint32_t s_lastWsBroadcastMs = 0;
  if (now - s_lastWsBroadcastMs >= 50) {
    s_lastWsBroadcastMs = now;
    broadcastWsData();
  }

  // Touch IP check
  static uint32_t s_lastTouchCheckMs = 0;
  static bool s_lastTouchState = false;
  if (now - s_lastTouchCheckMs >= 60) {
    s_lastTouchCheckMs = now;
    bool isTouched = checkTouchIpSensor();
    if (isTouched) {
      triggerIpScreen(4000);
      if (!s_lastTouchState) {
        setNeoPixelColor(0, 20, 40);
        Serial.printf("\r\n[TOUCH] Touch 11 (GPIO 11) detected! (Val: %u, Base: %u) -> Showing IP on OLED: %s\r\n",
                      g_lastTouchVal, g_touchBaseline,
                      (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString().c_str() : "[Connecting...]");
      }
    }
    s_lastTouchState = isTouched;
  }

  PerfDebug::update(g_plotterMode);

  delay(2);
}
