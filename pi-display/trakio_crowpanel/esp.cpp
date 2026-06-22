// Trakio Navigator — ESP32 BLE receiver
//
// Receives newline-delimited JSON frames from the phone over the Nordic UART
// Service (NUS) and decodes them for the LVGL UI.
//
// Frame types streamed by the Android app:
//   {"type":"route_start","destination":"MG Road","totalDistance":5200,"totalDuration":900}
//   {"type":"nav","heading":87,"distanceToTurn":120,"instruction":"RIGHT",
//    "route":[[0,0],[-5,20],...],"remainingDistance":4200,"remainingTime":760,
//    "eta":1718712345678,
//    "junctionRoads":[{"highlighted":true,"coords":[[0,40],[6,70]]},
//                     {"highlighted":false,"coords":[[-30,40],[-60,40]]}],
//    "arrived":false}
//   {"type":"incoming_call","caller":"Mom","number":"+91...","call_id":"..."}
//   {"type":"route_end"}
//
// `route` and every junction-road `coords` share one HEADING-UP (track-up)
// local frame in metres: [0,0] is the rider, +y = forward (up on screen),
// +x = right. The direction of travel is always up, so the position arrow
// stays fixed pointing up and the map turns on bends. Scale to your display.
// `junctionRoads` are the cross-streets near the next maneuver; the
// "highlighted":true entry is the branch the route takes.
//
// Requires the ArduinoJson library (v7.x) — install via the Library Manager.

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <esp_bt.h>        // esp_bt_controller_mem_release
#include <ArduinoJson.h>
#include "crowpanel_tft.h"
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "nav_state.h"   // MAX_ROUTE_POINTS + shared state declarations
#include "ui.h"

// Give the Arduino loop task (which runs setup()) headroom over its 8 KB
// default — the BLE GATT-server setup call chain is deep.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#define SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

static constexpr uint8_t SERVICE_UUID_BYTES[16] = {
  0x6E, 0x40, 0x00, 0x01, 0xB5, 0xA3, 0xF3, 0x93,
  0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E,
};

static constexpr uint8_t CHARACTERISTIC_UUID_BYTES[16] = {
  0x6E, 0x40, 0x00, 0x02, 0xB5, 0xA3, 0xF3, 0x93,
  0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E,
};

static BLEUUID makeUuid(const uint8_t (&bytes)[16]) {
  return BLEUUID(const_cast<uint8_t *>(bytes), sizeof(bytes), true);
}

static constexpr uint16_t SCREEN_WIDTH = 320;
static constexpr uint16_t SCREEN_HEIGHT = 240;
static constexpr uint16_t LVGL_BUFFER_ROWS = 20;

static TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t drawBuffer;
static lv_color_t drawPixels[SCREEN_WIDTH * LVGL_BUFFER_ROWS];
static lv_disp_drv_t displayDriver;
static uint32_t lastLvglTickMs = 0;

// ── Trip summary (set once at route_start) ──
String          tripDestination = "";       // destination name
long            tripTotalDist   = 0;        // total route distance, metres
long            tripTotalDur    = 0;        // total route duration, seconds

// ── Decoded navigation state (read these from your LVGL render loop) ──
volatile bool   navActive       = false;   // true between route_start and route_end
int             navHeading      = 0;        // degrees, 0..359
int             navDistanceToTurn = 0;      // metres to next maneuver
String          navInstruction  = "";       // "LEFT" / "RIGHT" / "STRAIGHT" / ...
long            navRemainingDist = 0;       // metres left to destination
long            navRemainingTime = 0;       // seconds left to destination
long long       navEta          = 0;        // arrival time, epoch milliseconds
bool            navArrived      = false;
int             routeX[MAX_ROUTE_POINTS];   // local metres, +x = right
int             routeY[MAX_ROUTE_POINTS];   // local metres, +y = forward
int             routeLen        = 0;

// ── Junction context roads (same local frame as route) ──
int             junctionRoadCount = 0;
int             junctionRoadLen[MAX_JUNCTION_ROADS]   = {0};
bool            junctionHighlighted[MAX_JUNCTION_ROADS] = {false};
int             junctionX[MAX_JUNCTION_ROADS][MAX_JUNCTION_ROAD_POINTS];
int             junctionY[MAX_JUNCTION_ROADS][MAX_JUNCTION_ROAD_POINTS];

// ── Incoming call state ──
bool    callActive = false;
String  callCaller = "";
String  callNumber = "";

// Reassembly buffer: frames arrive split across BLE packets and are
// terminated by '\n'. We accumulate bytes until we have a full line.
//
// BLE writes land in a separate FreeRTOS task (RxCallbacks::onWrite), but ALL
// frame decoding — which mutates the Strings/arrays that ui_update() reads —
// happens in loop(). The mutex below guards ONLY the shared rxBuffer; the
// nav-state itself is therefore only ever touched from the loop task, so it
// can never be reallocated mid-read (which would corrupt the heap).
static String            rxBuffer = "";
static SemaphoreHandle_t rxMutex  = nullptr;

static void displayFlush(lv_disp_drv_t *display, const lv_area_t *area, lv_color_t *colorBuffer) {
  const uint32_t width = (uint32_t)(area->x2 - area->x1 + 1);
  const uint32_t height = (uint32_t)(area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, width, height);
  tft.pushColors((uint16_t *)&colorBuffer->full, width * height, true);
  tft.endWrite();

  lv_disp_flush_ready(display);
}

static void handleFrame(const String &line) {
  if (line.length() == 0) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  const char *type = doc["type"] | "";

  if (strcmp(type, "route_start") == 0) {
    navActive       = true;
    callActive      = false;
    tripDestination = (const char *)(doc["destination"]   | "Destination");
    tripTotalDist   = doc["totalDistance"] | 0;
    tripTotalDur    = doc["totalDuration"] | 0;
    Serial.print("ROUTE START -> ");
    Serial.print(tripDestination);
    Serial.print(" (");
    Serial.print(tripTotalDist);
    Serial.print("m, ");
    Serial.print(tripTotalDur);
    Serial.println("s)");

  } else if (strcmp(type, "nav") == 0) {
    navActive         = true;
    // NOTE: do NOT clear callActive here — nav frames stream continuously, so
    // clearing it would wipe an incoming-call toast before it's even seen. The
    // toast is dismissed by a "call_end" frame, route_end, or a timeout in the UI.
    navHeading        = doc["heading"]        | 0;
    navDistanceToTurn = doc["distanceToTurn"] | 0;
    navInstruction    = (const char *)(doc["instruction"] | "");
    navRemainingDist  = doc["remainingDistance"] | 0;
    navRemainingTime  = doc["remainingTime"]     | 0;
    navEta            = doc["eta"]               | 0LL;
    navArrived        = doc["arrived"]        | false;

    routeLen = 0;
    JsonArray route = doc["route"].as<JsonArray>();
    for (JsonArray pt : route) {
      if (routeLen >= MAX_ROUTE_POINTS) break;
      routeX[routeLen] = pt[0] | 0;
      routeY[routeLen] = pt[1] | 0;
      routeLen++;
    }

    // Junction context roads (cross-streets near the maneuver). Each road
    // carries a `highlighted` flag and a short polyline in the same frame.
    junctionRoadCount = 0;
    JsonArray roads = doc["junctionRoads"].as<JsonArray>();
    for (JsonObject road : roads) {
      if (junctionRoadCount >= MAX_JUNCTION_ROADS) break;
      int r = junctionRoadCount;
      junctionHighlighted[r] = road["highlighted"] | false;
      int n = 0;
      JsonArray coords = road["coords"].as<JsonArray>();
      for (JsonArray pt : coords) {
        if (n >= MAX_JUNCTION_ROAD_POINTS) break;
        junctionX[r][n] = pt[0] | 0;
        junctionY[r][n] = pt[1] | 0;
        n++;
      }
      if (n < 2) continue;   // need at least a segment to draw
      junctionRoadLen[r] = n;
      junctionRoadCount++;
    }
    // (no per-frame NAV log — nav frames stream several times a second)

  } else if (strcmp(type, "incoming_call") == 0) {
    callActive = true;
    callCaller = (const char *)(doc["caller"] | "Incoming call");
    callNumber = (const char *)(doc["number"] | "");
    Serial.print("CALL from ");
    Serial.println(callCaller);

  } else if (strcmp(type, "call_end") == 0) {
    callActive = false;
    Serial.println("CALL END");

  } else if (strcmp(type, "route_end") == 0) {
    navActive  = false;
    callActive = false;
    routeLen   = 0;
    junctionRoadCount = 0;
    Serial.println("ROUTE END");
  }
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    // getValue() returns std::string on esp32 core 2.x and Arduino String on
    // 3.x — `auto` + c_str()/length() compiles against both.
    auto value = characteristic->getValue();
    if (value.length() == 0 || rxMutex == nullptr) return;

    // Runs in the BLE task: only append raw bytes. Decoding happens in loop().
    if (xSemaphoreTake(rxMutex, portMAX_DELAY) == pdTRUE) {
      rxBuffer += value.c_str();
      // Guard against a runaway buffer if a newline never arrives.
      if (rxBuffer.length() > 4096) rxBuffer = "";
      xSemaphoreGive(rxMutex);
    }
  }
};

// Pull every complete (newline-terminated) frame out of rxBuffer and decode it.
// Called from loop() so handleFrame() — and the nav-state it writes — only ever
// runs in the loop task, never concurrently with ui_update().
static void drainRxFrames() {
  if (rxMutex == nullptr) return;

  String batch;
  if (xSemaphoreTake(rxMutex, portMAX_DELAY) == pdTRUE) {
    int lastNl = rxBuffer.lastIndexOf('\n');
    if (lastNl >= 0) {
      batch = rxBuffer.substring(0, lastNl + 1);   // all complete lines
      rxBuffer.remove(0, lastNl + 1);              // keep the partial tail
    }
    xSemaphoreGive(rxMutex);                        // release before parsing
  }

  int nl;
  while ((nl = batch.indexOf('\n')) >= 0) {
    String line = batch.substring(0, nl);
    batch.remove(0, nl + 1);
    line.trim();
    handleFrame(line);
  }
}

// Restart advertising when a client disconnects so the phone can reconnect.
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) {
    Serial.println("BLE client connected");
  }
  void onDisconnect(BLEServer *server) {
    Serial.println("BLE client disconnected — re-advertising");
    navActive  = false;
    callActive = false;
    server->startAdvertising();
  }
};

void setup() {
  Serial.begin(115200);

  rxMutex = xSemaphoreCreateMutex();   // guard rxBuffer before BLE can fire

  // We only use BLE, never Bluetooth Classic — hand that controller RAM back.
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  BLEDevice::init("Trakio-Navigator");
  BLEDevice::setMTU(247);  // ask for a larger MTU so frames need fewer chunks

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  // UUIDs are built from raw bytes (not parsed from strings) to keep BLEUUID
  // off the esp32 3.x String-parsing path.
  BLEUUID serviceUuid = makeUuid(SERVICE_UUID_BYTES);
  BLEService *service = server->createService(serviceUuid);

  // Accept both write-with-response and write-without-response so the phone
  // can use whichever is faster.
  BLEUUID characteristicUuid = makeUuid(CHARACTERISTIC_UUID_BYTES);
  BLECharacteristic *characteristic = service->createCharacteristic(
      characteristicUuid,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  characteristic->setCallbacks(new RxCallbacks());

  service->start();

  BLEAdvertising *advertising = server->getAdvertising();
  advertising->addServiceUUID(serviceUuid);
  advertising->setScanResponse(true);
  advertising->start();

  Serial.println("BLE READY — advertising as Trakio-Navigator");

  // ── Display + LVGL ──────────────────────────────────────
  lv_init();

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);

  lv_disp_draw_buf_init(&drawBuffer, drawPixels, nullptr, SCREEN_WIDTH * LVGL_BUFFER_ROWS);
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = SCREEN_WIDTH;
  displayDriver.ver_res = SCREEN_HEIGHT;
  displayDriver.flush_cb = displayFlush;
  displayDriver.draw_buf = &drawBuffer;
  lv_disp_drv_register(&displayDriver);

  lastLvglTickMs = millis();
  ui_init();
}

void loop() {
  const uint32_t now = millis();
  lv_tick_inc(now - lastLvglTickMs);
  lastLvglTickMs = now;

  drainRxFrames();      // decode any BLE frames received since the last loop
  lv_timer_handler();   // drive LVGL rendering
  ui_update();          // push the latest decoded nav state into the UI
  delay(5);
}
