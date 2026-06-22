# Trakio Navigator — CrowPanel ESP32 2.8" flash target

Arduino sketch for the **Elecrow CrowPanel ESP32 HMI 2.8 inch** board
(ESP32-WROOM-32 + ILI9341 320×240 SPI display, model DIS04028H).

All the source lives **in this folder** and compiles as normal Arduino
translation units (one `.cpp` = one unit). Open `trakio_crowpanel.ino` in the
Arduino IDE; it is intentionally empty — `setup()`/`loop()` come from `esp.cpp`:

```
trakio_crowpanel.ino  — empty entry point (Arduino requires the .ino)
esp.cpp               — BLE receiver + display/LVGL init  (setup()/loop())
ui.cpp                — LVGL UI (HOME / NAV / DONE screens + call toast)
nav_state.h           — shared nav-state declarations (esp.cpp <-> ui.cpp)
ui.h, crowpanel_tft.h — UI entry points + board pin config
lv_conf.h             — LVGL config (also copy to your libraries folder, step 3)
```

> ⚠️ Do **not** `#include "esp.cpp"`/`"ui.cpp"` into the `.ino`. Compiling them
> as one merged translation unit corrupts the ESP32 BLE library's C++ class
> layout and crashes inside `createService()`. Keep them as separate files.

---

## 1. Install the board core + libraries

In **Tools → Board → Boards Manager** install:

- **esp32 by Espressif Systems** — core **3.3.x** (tested on 3.3.10) or 2.0.x.
  The BLE code compiles on both (`getValue()` returns `String` on 3.x,
  `std::string` on 2.x — handled with `auto`).

In **Sketch → Include Library → Manage Libraries** install:

| Library      | Version |
|--------------|---------|
| `TFT_eSPI`   | 2.5.x   |
| `lvgl`       | 8.3.x (8.3.3 recommended) |
| `ArduinoJson`| 7.x     |

ESP32 BLE (BLEDevice/BLEServer) ships with the esp32 core — nothing to install.

---

## 2. Configure TFT_eSPI for the CrowPanel pins  ← **REQUIRED, do not skip**

TFT_eSPI compiles as its **own** translation unit, so the pin defines in
`crowpanel_tft.h` are **not** enough on their own — the library must be told the
pins directly or the screen stays **black**.

Open `Arduino/libraries/TFT_eSPI/User_Setup.h`, delete its contents, and paste:

```cpp
#define USER_SETUP_INFO "CrowPanel ESP32 2.8 DIS04028H"

#define ILI9341_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   27

#define TOUCH_CS 33

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_GLCD
#define SMOOTH_FONT

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  16000000
#define SPI_TOUCH_FREQUENCY  2500000
```

(These are the same pins as `pi-display/crowpanel_tft.h`. Make sure
`User_Setup_Select.h` still has its default `#include <User_Setup.h>` active.)

---

## 3. Place lv_conf.h where LVGL looks for it

LVGL includes `../../lv_conf.h` relative to `libraries/lvgl/src/` — i.e. it
looks in your **libraries** folder, **not** the sketch folder. Copy the conf
from this sketch up one level:

```
cp lv_conf.h  <Arduino sketchbook>/libraries/lv_conf.h
```

(The copy in this folder is the source of truth; keep them in sync. It enables
the Montserrat 14/22/28/40 fonts and the BAR/LINE/ANIMATION widgets the UI
needs.)

---

## 4. Arduino IDE board settings (Tools menu)

| Setting           | Value |
|-------------------|-------|
| Board             | **ESP32 Dev Module** |
| Upload Speed      | 921600 |
| CPU Frequency     | 240 MHz |
| Flash Frequency   | 80 MHz |
| Flash Size        | 4 MB (32 Mb) |
| **Partition Scheme** | **Huge APP (3 MB No OTA / 1 MB SPIFFS)** ← BLE+LVGL won't fit the default 1.2 MB app |
| PSRAM             | Disabled |
| Port              | your `/dev/ttyUSB*` (or `/dev/ttyACM*`) |

The "Huge APP" partition is important — the default partition is too small and
gives a "Sketch too big" error once BLE + LVGL + TFT_eSPI are linked.

---

## 5. Build & upload

Press **Upload** (→). If it sits at *"Connecting…"* put the board in boot mode:

1. Hold **BOOT**
2. Tap **RST**
3. Release **RST**
4. Release **BOOT** once upload begins

Open **Serial Monitor @ 115200**. You should see:

```
BLE READY — advertising as Trakio-Navigator
```

and the display shows the **HOME** screen (breathing ring + "TRAKIO / Ready to
ride"). When the phone app connects and streams frames, the screen switches to
**NAV**, then **DONE** on arrival; incoming calls appear as a red toast.

### Arduino CLI (optional)

```
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
  /home/pannaga/Desktop/pro/trakio/pi-display/trakio_crowpanel

arduino-cli upload -p /dev/ttyUSB0 \
  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
  /home/pannaga/Desktop/pro/trakio/pi-display/trakio_crowpanel
```

---

## How state change & data rendering work

- The phone streams newline-delimited JSON frames over the Nordic UART BLE
  service (`route_start`, `nav`, `incoming_call`, `route_end`).
- BLE writes arrive on a **separate FreeRTOS task**. To keep the heap safe, that
  task only appends raw bytes to `rxBuffer` (guarded by `rxMutex`). All JSON
  decoding — and every write to the nav-state that the UI reads — happens in
  `loop()` via `drainRxFrames()`, so it never races with `ui_update()`.
- `ui_update()` (also in `loop()`) picks the screen from the decoded state:
  `navArrived → DONE`, `!navActive → HOME`, otherwise `NAV`; `callActive`
  overlays the toast on top of whichever screen is active.

## Troubleshooting

- **Black screen** → TFT_eSPI `User_Setup.h` not configured (step 2), or the
  backlight pin is wrong. Confirm `BLE READY` prints on serial first.
- **Colors inverted / blue⇄red swapped** → flip `LV_COLOR_16_SWAP` (0↔1) in
  `lv_conf.h`, or toggle `tft.invertDisplay(true/false)` after `tft.begin()` in
  `esp.cpp`.
- **"Sketch too big"** → set the Huge APP partition scheme (step 4).
- **`lv_conf.h` not found / font errors** → the conf isn't in the libraries
  folder (step 3).
- **Mirrored / rotated image** → change `tft.setRotation(1)` in `esp.cpp`
  (try 1 or 3 for landscape).
