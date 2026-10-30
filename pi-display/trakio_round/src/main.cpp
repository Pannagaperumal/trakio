// Trakio Round — ESP32-C6 1.43" AMOLED ride display.
//
// Everything runs on the Arduino loop task (LVGL, I2C, frame decoding); the
// BLE host task only appends received bytes (see link.cpp).
//
// Power, in order of what saves the most:
//   • AMOLED black UI; brightness follows day/night.
//   • Screen dims after 1 min idle and turns off after 2 min (never while
//     navigating or while a call/crash/arrival screen is up).
//   • Screen off → LVGL stops, CPU drops to 80 MHz, IMU sampling to 20 Hz.
//   • Off for 5 min with no phone and no ride → deep sleep; motion, a touch
//     or the PWR key wakes it (all routed to LP GPIOs).
//   • BLE: 30–50 ms interval only while riding, 200–400 ms otherwise; slow
//     advertising after the first 30 s.
#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>
#include <esp_sleep.h>
#include "board.h"
#include "hal.h"
#include "display.h"
#include "link.h"
#include "sensors.h"
#include "state.h"
#include "ui.h"
#include "audio.h"

#define DIM_AFTER_MS    (60UL * 1000)
#define OFF_AFTER_MS    (2UL * 60 * 1000)
#define SLEEP_AFTER_MS  (5UL * 60 * 1000)
#define PARKED_AFTER_MS (3UL * 60 * 1000)

enum Power { PWR_ON, PWR_DIM, PWR_OFF };
static Power power = PWR_ON;
static uint32_t lastActivityMs = 0, offSinceMs = 0;
static Preferences prefs;
static bool themeFromPhone = false;
static bool clockStored = false;

// crash countdown
static int crashLeft = -1;          // -1 idle, 0 = SOS sent, >0 counting
static uint32_t crashTickMs = 0;
static String pendingParkAlert;     // sent once the phone reconnects

static uint8_t themedBrightness() { return settings.brightness; }

static void setPower(Power p) {
  if (p == power) return;
  if (p == PWR_ON) {
    setCpuFrequencyMhz(160);
    display_wake();
    display_set_brightness(themedBrightness());
    ui_note_wake();
  } else if (p == PWR_DIM) {
    display_set_brightness(max<uint8_t>(4, themedBrightness() / 5));
  } else {
    display_sleep();
    setCpuFrequencyMhz(80);
    offSinceMs = millis();
  }
  power = p;
}

static void activity() {
  lastActivityMs = millis();
  setPower(PWR_ON);
}

static void enterDeepSleep() {
  Serial.println("[power] deep sleep — motion, touch or PWR wakes it");
  Serial.flush();
  link_stop();
  display_sleep();
  imu_arm_wake_on_motion();
  delay(30);
  exio_read_inputs();               // clear a pending expander interrupt first
  esp_deep_sleep_enable_gpio_wakeup((1ULL << PIN_EXIO_INT) | (1ULL << PIN_PWR_KEY), ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

// Day/night by local time until the phone takes over with `theme` frames.
static void autoTheme() {
  if (themeFromPhone || clock_epoch_ms() <= 0) return;
  time_t s = (time_t)(clock_epoch_ms() / 1000) + settings.tzOffsetMin * 60;
  struct tm t;
  gmtime_r(&s, &t);
  int minutes = t.tm_hour * 60 + t.tm_min;
  Theme want = minutes >= 6 * 60 + 20 && minutes < 18 * 60 + 25 ? THEME_DAY : THEME_NIGHT;
  if (want == settings.theme) return;
  settings.theme = want;
  settings.brightness = want == THEME_DAY ? 100 : 45;
  ui_on_events(EV_THEME);
  if (power == PWR_ON) display_set_brightness(settings.brightness);
}

// ── callbacks from the UI ────────────────────────────────
void app_call_button(bool accept) {
  link_send(accept ? "{\"type\":\"call_action\",\"action\":\"accept\"}" : "{\"type\":\"call_action\",\"action\":\"decline\"}");
  if (!accept) {
    call.phase = CALL_NONE;
    ui_on_events(EV_CALL);
  }
  activity();
}

void app_crash_ok() {
  link_send("{\"type\":\"sos_cancel\"}");
  crashLeft = -1;
  sosSent = false;
  ui_crash_show(-1);
  activity();
}

// ── setup / loop ─────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  hal_init();                       // power hold first
  setenv("TZ", TZ_POSIX, 1);        // Asia/Kolkata
  tzset();
  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  Serial.printf("\n[trakio] boot, wake cause %d\n", (int)wake);

  prefs.begin("trakio", false);
  settings.theme = (Theme)prefs.getUChar("theme", THEME_NIGHT);
  settings.brightness = prefs.getUChar("bright", 60);

  sensors_init();
  if (audio_init()) audio_chime(0);  // boot chime doubles as a speaker check
  rtc_load_clock();
  display_init();
  ui_init();
  ui_set_battery(battery_percent(), battery_on_usb());
  autoTheme();
  display_set_brightness(themedBrightness());
  link_init();
  lastActivityMs = millis();
  Serial.printf("[trakio] ready, free heap %u\n", (unsigned)ESP.getFreeHeap());
}

// Same newline-delimited JSON frames as BLE, typed over USB serial — e.g. a
// {"type":"time",...} frame to set the clock without a phone.
// Typing `crash` runs the crash flow without a real impact (USB only, for testing).
static bool testCrash = false;

static void serialPoll() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf == "crash") testCrash = true;
      else if (buf.length()) state_handle_frame(buf.c_str(), buf.length());
      buf = "";
    } else if (buf.length() < 512) {
      buf += c;
    }
  }
}

void loop() {
  uint32_t now = millis();

  // BLE (and USB serial) frames → state → UI
  link_poll();
  serialPoll();
  audio_poll();
  uint32_t ev = stateEvents;
  stateEvents = 0;
  if (ev) {
    if (ev & EV_CHIME) audio_chime(chimeKind);
    if (ev & EV_THEME) {
      themeFromPhone = true;
      prefs.putUChar("theme", settings.theme);
      prefs.putUChar("bright", settings.brightness);
      if (power == PWR_ON) display_set_brightness(settings.brightness);
    }
    if (ev & (EV_ROUTE_START | EV_ROUTE_END)) {
      link_set_riding(nav.active);
      imu_set_parked(false);
      if (ev & EV_ROUTE_START) imu_level_here();
    }
    if (ev & EV_SOS && sosSent) { crashLeft = 0; ui_crash_sos_sent(); }
    // Rider tapped "I'm OK" on the phone: stop the countdown here too.
    if (ev & EV_SOS && !sosSent && crashLeft > 0) { crashLeft = -1; ui_crash_show(-1); }
    // `time` frames are authoritative: always persist them. The clock implied
    // by nav frames is saved once per boot.
    if (((ev & EV_TIME) || ((ev & EV_NAV) && !clockStored)) && clock_epoch_ms() > 0) {
      rtc_store_clock();
      clockStored = true;
    }
    if (ev & (EV_ROUTE_START | EV_ARRIVED | EV_CALL)) activity();
    ui_on_events(ev);
  }

  // link status + battery
  static bool wasUp = false, wasSub = false;
  bool up = link_connected();
  if (up != wasUp) {
    wasUp = up;
    ui_set_link(up);
    ui_on_events(EV_NAV);           // refresh the home hint
    if (up) activity();
  }
  static uint32_t lastBatt = 0, lastBattSent = 0;
  static int lastPct = -1;
  bool sub = link_subscribed();
  if (now - lastBatt > 30000 || !lastBatt || (sub && !wasSub)) {
    lastBatt = now;
    int pct = battery_percent();
    bool usb = battery_on_usb();
    ui_set_battery(pct, usb);
    if (sub && (abs(pct - lastPct) >= 2 || now - lastBattSent > 60000 || !wasSub)) {
      link_send(String("{\"type\":\"battery\",\"percent\":") + pct + ",\"charging\":" + (usb ? "true" : "false") +
                ",\"mv\":" + battery_millivolts() + "}");
      lastPct = pct;
      lastBattSent = now;
    }
    autoTheme();
  }
  if (sub && pendingParkAlert.length()) { link_send(pendingParkAlert); pendingParkAlert = ""; }
  wasSub = sub;

  // sensors: crash + park guard
  sensors_poll(nav.active);
  float g;
  bool crashed = imu_take_crash(g) && nav.active;
  if (testCrash) {
    testCrash = false;
    crashed = true;
    g = 7.5f;
    Serial.println("[test] simulated crash");
  }
  if (crashed && crashLeft < 0) {
    crashLeft = 10;
    crashTickMs = now;
    activity();
    ui_crash_show(crashLeft);
    audio_chime(3);
    link_send(String("{\"type\":\"imu_event\",\"event\":\"crash\",\"peakG\":") + String(g, 1) + "}");
  }
  if (crashLeft > 0 && now - crashTickMs >= 1000) {
    crashTickMs += 1000;
    crashLeft--;
    if (crashLeft > 0) { ui_crash_show(crashLeft); audio_chime(3); }
    else { link_send("{\"type\":\"sos_request\"}"); ui_crash_sos_sent(); audio_chime(4); }
    lastActivityMs = now;
  }
  imu_set_parked(!nav.active && now - lastActivityMs > PARKED_AFTER_MS);
  if (imu_take_park_motion(g)) {
    String f = String("{\"type\":\"imu_event\",\"event\":\"park_motion\",\"peakG\":") + String(g, 1) + "}";
    if (link_subscribed()) link_send(f); else pendingParkAlert = f;
    Serial.println("[guard] parked bike moved");
  }
  if (nav.active) ui_set_lean(imu_lean_deg());

  // keys
  KeyEvent k = pwr_key_poll();
  if (k == KEY_SHORT) {
    if (power == PWR_ON) { setPower(PWR_OFF); lastActivityMs = now - OFF_AFTER_MS; }
    else activity();
  } else if (k == KEY_LONG) {
    Serial.println("[power] PWR held — switching off");
    board_power_off();              // on battery this cuts power here
    enterDeepSleep();               // on USB, sleep instead
  }

  // idle policy
  bool busy = nav.active || ui_overlay_active() || crashLeft >= 0;
  if (busy) lastActivityMs = now;
  if (power != PWR_ON) {
    static uint32_t lastTouchPoll = 0;
    if (now - lastTouchPoll > 100) { lastTouchPoll = now; if (touch_poll_pressed()) activity(); }
    if (now - imu_last_motion_ms() < 200 && power == PWR_DIM) activity();
  } else if (touch_last_ms() > lastActivityMs) {
    lastActivityMs = touch_last_ms();
  }
  uint32_t idle = now - lastActivityMs;
  if (power == PWR_ON && idle > DIM_AFTER_MS) setPower(PWR_DIM);
  if (power == PWR_DIM && idle > OFF_AFTER_MS) setPower(PWR_OFF);
  // No deep sleep while a computer is on the USB port (it would drop the serial console).
  if (power == PWR_OFF && !busy && !link_connected() && !(bool)Serial && now - offSinceMs > SLEEP_AFTER_MS) enterDeepSleep();

  // render
  if (power != PWR_OFF) {
    ui_update();
    uint32_t wait = lv_timer_handler();
    delay(min<uint32_t>(wait, 8));
  } else {
    delay(40);
  }
}
