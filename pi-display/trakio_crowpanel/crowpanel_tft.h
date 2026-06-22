// CrowPanel ESP32 HMI 2.8-inch integrated board (DIS04028H)
// TFT_eSPI reads these compile-time definitions so the sketch can build
// without editing the global User_Setup.h inside the library.
#pragma once

#define USER_SETUP_LOADED

#define ILI9341_DRIVER

#define TFT_WIDTH 240
#define TFT_HEIGHT 320

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1
#define TFT_BL   27

#define TOUCH_CS 33

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  16000000
#define SPI_TOUCH_FREQUENCY 2500000