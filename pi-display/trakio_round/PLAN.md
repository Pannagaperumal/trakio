# Trakio Round — build plan

Target: Waveshare **ESP32-C6-Touch-AMOLED-1.43** (466×466 round AMOLED, CO5300 QSPI,
FT6146 touch, QMI8658 IMU, PCF85063 RTC, ES8311 speaker, Li-ion + PWR button), paired
with the Trakio Android app. The visual reference is the interactive preview
(claude.ai artifact "Trakio Device Preview").

The CrowPanel target (`pi-display/trakio_crowpanel/`) keeps working throughout. Every
protocol change below is **additive**: old frames and fields stay as they are.

---

## Principles

**Premium look**
- Pure-black AMOLED canvas. Content floats and pixels are off everywhere else.
- One brand accent (mint `#12E29C`) used for meaning only: the route, progress and
  the turn arrow. Everything else is white or grey.
- Large Montserrat numerals, generous safe inset from the round edge (≥15%), and no
  text clipped by the circle.
- Smooth motion: page slides, eased map rotation/zoom, and a pop on arrival. Everything
  animates in the firmware, so it never stutters waiting for BLE.
- The map has no background grid or decoration. It shows only the real streets around
  the rider, the route and the rider arrow.

**Day / night**
| | Day (sunlight) | Night |
|---|---|---|
| Brightness | 100% | 35% (rider adjustable) |
| Minor roads | `#8A94AD`, 7 px | `#2E3750`, 6 px |
| Major roads | `#D5DAE6`, 11 px | `#4B5674`, 10 px |
| Route | mint, 18 px, white edge | mint, 16 px, dark-mint casing |
| Text | white, heavier weight | white, dimmed secondary |

The phone decides the mode from sunrise/sunset at the rider's location and sends a
`theme` frame. The rider can force Day/Night/Auto in the app.

**Low power: device**
1. Black UI → most AMOLED pixels are off. Brightness follows the theme.
2. LVGL redraws only dirty areas. The map layer redraws only when a frame moves it.
   Static pages tick at 1 Hz.
3. CPU at 80 MHz while riding, and light sleep between LVGL ticks when idle.
4. Screen off after 20 s idle when not navigating. The QMI8658 **wake-on-motion
   interrupt** turns it back on, so touch polling isn't needed.
5. Parked for >5 min → deep sleep. The IMU WoM interrupt wakes it (park guard),
   and the RTC keeps time.
6. BLE connection interval: 30–50 ms while navigating, 500 ms+ when idle (requested
   from the peripheral).

**Low power: phone**
1. **Map areas sent once, not every frame.** Nearby roads go over BLE as one
   `map_area` frame per ~1.2 km area (north-up, metres from an anchor). Each `nav`
   frame then carries only the rider's position in that area plus heading, and the
   device rotates the map itself. This cuts the old per-frame junction/road
   payload by ~90%.
2. Adaptive frame rate: 4 Hz when moving above 2 m/s, 1 Hz below that, a 5 s
   heartbeat when stopped, and nothing sent if nothing changed.
3. GPS interval follows speed: 1 s riding, 3 s when stopped. Location updates stop
   on arrival.
4. Road geometry is fetched from Overpass **once per route** (a corridor along the
   polyline) and cached, never per fix.
5. `CONNECTION_PRIORITY_BALANCED` while riding and `LOW_POWER` when idle.

**Seamless**
- Auto-reconnect (already present). On reconnect the phone replays `route_start`,
  `theme`, the current `map_area` and the latest `nav`, so the screen recovers in
  under a second.
- The device switches pages on its own: Nav on route start, Done on arrival, Home
  after that. Calls overlay any page.
- The device remembers the last theme and brightness in NVS, so they apply on boot
  before the phone connects.

---

## Protocol v2 (additive)

Phone → device (NUS RX, newline-delimited JSON, as today):
```
{"type":"theme","mode":"day"|"night","brightness":100}
{"type":"map_area","id":7,"roads":[{"w":2,"c":[[x,y],[x,y],...]}, ...]}
     // north-up metres from the area anchor; w: 1 minor, 2 major; ≤ 64 roads, ≤ 8 pts each
{"type":"nav", ...existing fields..., "area":7,"ax":-120,"ay":340,"street":"Market Street","speed":27}
     // ax/ay = rider position in the area frame (m); heading already in the frame
{"type":"call_state","state":"active"|"ended"}
{"type":"sos_status","state":"sent"}
```
Device → phone (new NUS TX **notify** characteristic `6E400003-…`):
```
{"type":"battery","percent":82,"charging":false,"mv":3950}
{"type":"imu_event","event":"crash","peakG":7.8}
{"type":"imu_event","event":"park_motion","peakG":2.4}
{"type":"call_action","action":"accept"|"decline"}
{"type":"sos_cancel"}
```

---

## Steps (each one is shippable on its own)

1. **Preview update.** Organic street map with no grid, a Day/Night theme, power
   indicators, and the polish described above. ✅ first
2. **Android: map areas + adaptive streaming.**
   - JS: fetch a road corridor (Overpass `way(around:…)` on the route polyline) once
     per route and add it to the route payload.
   - Kotlin: `MapAreaBuilder` clips roads to the current area and emits `map_area`
     when the rider nears the edge. Adds `area/ax/ay/street/speed` to `nav`. Adaptive
     emit rate. Unit tests next to `BleNavFrameBuilderTest`.
3. **Android: theme frames.** Sunrise/sunset calculation (no network), an Auto/Day/
   Night setting, and a resend on reconnect.
4. **Firmware bring-up (`pi-display/trakio_round/`).** PlatformIO + pioarduino
   (arduino-esp32 3.x, which is needed for the C6). CO5300 QSPI + LVGL 8.3 partial
   buffers (466×40), FT6146 touch, TCA9554 power rails. Pins come from the Rev1.3
   schematic. ⚠ This step needs the board to verify.
5. **Firmware: BLE + state.** Port the NUS receiver from `trakio_crowpanel/esp.cpp`,
   add the TX notify characteristic, and parse `theme` / `map_area` / new `nav`
   fields.
6. **Firmware: round UI.** Home / Nav map / Ride pages with swipe, the call screen +
   pill, the Arrived screen and day/night styles, matching the preview.
7. **Firmware: power.** Brightness per theme, idle screen-off, IMU wake-on-motion,
   deep sleep when parked, battery ADC → `battery` frames.
8. **IMU features.** Crash detection (peak > 6 g, then < 0.3 g variance for 2 s →
   10 s countdown), park guard, lean angle on the Ride page.
9. **Phone: device→phone handling.** Subscribe to TX notify. Device tab (battery,
   toggles). Accept/decline calls (`ANSWER_PHONE_CALLS`), the crash flow and the
   park-guard notification.
10. **SOS.** Send SMS with a location link to emergency contacts. Needs `SEND_SMS`
    and an explicit opt-in screen. Confirm the scope with the owner before building.
11. **Turn chimes** through the ES8311 (200 m / 50 m).

## Verification per step
- Android: `./gradlew :app:testDebugUnitTest` (frame builders are pure functions).
- Firmware: `pio run` compiles. Flash and check with `adb logcat -s TrakioBLE` +
  serial monitor.
- Power: measure current at the battery header (USB meter) idle / navigating /
  screen-off. Targets: < 25 mA navigating at night, < 1 mA deep sleep.
