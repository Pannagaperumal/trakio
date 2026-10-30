// Waveshare ESP32-C6-Touch-AMOLED-1.43 — pins and bus addresses.
// Source: Waveshare schematic Rev1.3 + their Arduino examples.
#pragma once
#include <Arduino.h>

// ── QSPI AMOLED (CO5300, driven with the SH8601-compatible esp_lcd driver) ──
#define PIN_LCD_CS    10
#define PIN_LCD_CLK   11
#define PIN_LCD_D0    4
#define PIN_LCD_D1    5
#define PIN_LCD_D2    6
#define PIN_LCD_D3    7
#define PIN_LCD_RST   3
#define LCD_RES       466
#define LCD_X_OFFSET  6       // panel RAM is 6 px wider than the visible circle

// ── Shared I2C bus: touch, IMU, RTC, IO expander ──
#define PIN_I2C_SDA   18
#define PIN_I2C_SCL   8
#define I2C_ADDR_TOUCH  0x38  // FT6146
#define I2C_ADDR_IMU    0x6B  // QMI8658
#define I2C_ADDR_RTC    0x51  // PCF85063
#define I2C_ADDR_EXIO   0x20  // TCA9554 (A0..A2 = 0)

// ── TCA9554 expander lines ──
#define EXIO_TP_INT    0
#define EXIO_RTC_INT   1
#define EXIO_LCD_EN    2
#define EXIO_TE        3
#define EXIO_IMU_INT1  4
#define EXIO_IMU_INT2  5
#define EXIO_BAT_EN    6     // must be held high or the board powers off on battery
#define EXIO_PA_EN     7     // speaker amplifier

// ── Direct GPIO ──
#define PIN_EXIO_INT  1      // TCA9554 #INT (open drain, 10k pull-up) — LP GPIO, wakes deep sleep
#define PIN_PWR_KEY   2      // PWR button, active low — LP GPIO, wakes deep sleep
#define PIN_BOOT_KEY  9
#define PIN_BAT_ADC   0      // VBAT / 2

// Time zone: Asia/Kolkata (IST, UTC+5:30, no daylight saving).
// The RTC keeps UTC; the UI adds this offset. A phone `time` frame can override it.
#define TZ_NAME               "Asia/Kolkata"
#define TZ_POSIX              "IST-5:30"
#define DEFAULT_TZ_OFFSET_MIN 330
