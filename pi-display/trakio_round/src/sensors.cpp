#include "sensors.h"
#include <Wire.h>
#include <SensorQMI8658.hpp>
#include <SensorPCF85063.hpp>
#include <math.h>
#include <time.h>
#include "board.h"
#include "state.h"

static SensorQMI8658 qmi;
static SensorPCF85063 rtc;
static bool imuOk = false, rtcOk = false;

static uint32_t lastSampleMs = 0, lastMotionMs = 0;
static float gPrev = 1.0f;
static float leanRaw = 0, leanZero = 0, leanSmooth = 0;

// Crash: a hard impact (> 6 g) followed by ~2 s of lying still.
#define CRASH_IMPACT_G   6.0f
#define CRASH_STILL_MS   2000
#define CRASH_WINDOW_MS  6000
static uint32_t impactMs = 0, stillSinceMs = 0;
static float impactPeak = 0;
static bool crashFlag = false;

// Park guard: > 0.25 g of disturbance for more than a second while parked.
static bool parked = false;
static uint32_t disturbSinceMs = 0;
static float disturbPeak = 0;
static bool parkFlag = false;
static uint32_t parkCooldownMs = 0;

void sensors_init() {
  imuOk = qmi.begin(Wire, I2C_ADDR_IMU);
  if (imuOk) {
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_16G, SensorQMI8658::ACC_ODR_LOWPOWER_128Hz, SensorQMI8658::LPF_MODE_0);
    qmi.disableGyroscope();
    qmi.enableAccelerometer();
  }
  rtcOk = rtc.begin(Wire);
  Serial.printf("[sensors] imu=%s rtc=%s\n", imuOk ? "ok" : "missing", rtcOk ? "ok" : "missing");
}

void sensors_poll(bool riding) {
  if (!imuOk) return;
  uint32_t now = millis();
  if (now - lastSampleMs < (riding ? 10u : 50u)) return;
  lastSampleMs = now;

  float x, y, z;
  if (!qmi.getAccelerometer(x, y, z)) return;
  float g = sqrtf(x * x + y * y + z * z);

  if (fabsf(g - gPrev) > 0.08f) lastMotionMs = now;
  gPrev = gPrev * 0.8f + g * 0.2f;

  // Lean: roll of the gravity vector around the bike's forward axis (board y).
  if (g > 0.5f && g < 1.6f) leanRaw = atan2f(x, z) * 180.0f / (float)M_PI;
  leanSmooth += (leanRaw - leanZero - leanSmooth) * 0.15f;

  // Crash detector
  if (g > CRASH_IMPACT_G) {
    if (!impactMs) impactPeak = 0;
    impactMs = now;
    impactPeak = max(impactPeak, g);
    stillSinceMs = 0;
  } else if (impactMs) {
    bool still = fabsf(g - 1.0f) < 0.15f;
    if (still && !stillSinceMs) stillSinceMs = now;
    if (!still) stillSinceMs = 0;
    if (stillSinceMs && now - stillSinceMs > CRASH_STILL_MS) {
      crashFlag = true;
      impactMs = 0;
    } else if (now - impactMs > CRASH_WINDOW_MS) {
      impactMs = 0;
    }
  }

  // Park guard
  if (parked && now > parkCooldownMs) {
    float d = fabsf(g - 1.0f);
    if (d > 0.25f) {
      if (!disturbSinceMs) { disturbSinceMs = now; disturbPeak = 0; }
      disturbPeak = max(disturbPeak, g);
      if (now - disturbSinceMs > 1000) {
        parkFlag = true;
        disturbSinceMs = 0;
        parkCooldownMs = now + 60000;     // one alert a minute at most
      }
    } else if (disturbSinceMs && now - disturbSinceMs > 1500) {
      disturbSinceMs = 0;
    }
  }
}

uint32_t imu_last_motion_ms() { return lastMotionMs; }
float imu_lean_deg() { return leanSmooth; }
void imu_level_here() { leanZero = leanRaw; leanSmooth = 0; }

bool imu_take_crash(float &peakG) {
  if (!crashFlag) return false;
  crashFlag = false;
  peakG = impactPeak;
  return true;
}

bool imu_take_park_motion(float &peakG) {
  if (!parkFlag) return false;
  parkFlag = false;
  peakG = disturbPeak;
  return true;
}

void imu_set_parked(bool p) {
  if (p && !parked) parkCooldownMs = millis() + 5000;   // ignore the bump of parking itself
  parked = p;
  disturbSinceMs = 0;
}

void imu_arm_wake_on_motion() {
  if (!imuOk) return;
  // 150 mg threshold, low-power 21 Hz, on INT1 (initial level high).
  qmi.configWakeOnMotion(150, SensorQMI8658::ACC_ODR_LOWPOWER_21Hz, SensorQMI8658::INTERRUPT_PIN_1, 1, 0x20);
}

// ── RTC (keeps UTC) ──────────────────────────────────────
void rtc_load_clock() {
  if (!rtcOk) return;
  RTC_DateTime t = rtc.getDateTime();
  if (t.getYear() < 2024 || t.getYear() > 2099) return;   // never set
  struct tm tmv = {};
  tmv.tm_year = t.getYear() - 1900;
  tmv.tm_mon = t.getMonth() - 1;
  tmv.tm_mday = t.getDay();
  tmv.tm_hour = t.getHour();
  tmv.tm_min = t.getMinute();
  tmv.tm_sec = t.getSecond();
  setenv("TZ", "UTC0", 1);            // RTC holds UTC
  tzset();
  time_t epoch = mktime(&tmv);
  setenv("TZ", TZ_POSIX, 1);          // back to Asia/Kolkata for local time
  tzset();
  clock_set_epoch_ms((int64_t)epoch * 1000);
  Serial.printf("[sensors] clock from RTC: %04u-%02u-%02u %02u:%02u UTC\n", t.getYear(), t.getMonth(), t.getDay(), t.getHour(), t.getMinute());
}

void rtc_store_clock() {
  int64_t ms = clock_epoch_ms();
  if (!rtcOk || ms <= 0) return;
  time_t s = (time_t)(ms / 1000);
  struct tm tmv;
  gmtime_r(&s, &tmv);
  rtc.setDateTime(RTC_DateTime(tmv));
}
