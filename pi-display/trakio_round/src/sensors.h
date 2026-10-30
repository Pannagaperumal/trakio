// QMI8658 motion sensor (accelerometer only — the gyro stays off to save power)
// and the PCF85063 RTC.
#pragma once
#include <Arduino.h>

void sensors_init();
void sensors_poll(bool riding);        // samples at 100 Hz riding, 20 Hz otherwise
uint32_t imu_last_motion_ms();         // millis() of the last noticeable movement
float imu_lean_deg();                  // + = left, smoothed, relative to the level pose
void imu_level_here();                 // take the current pose as upright (route start)
bool imu_take_crash(float &peakG);     // true once per detected crash
bool imu_take_park_motion(float &peakG);
void imu_set_parked(bool parked);      // park guard: report sustained movement
void imu_arm_wake_on_motion();         // before deep sleep: INT1 → EXIO4 → GPIO1

void rtc_load_clock();                 // seed the wall clock at boot
void rtc_store_clock();                // after the phone set it
