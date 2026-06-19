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
#include <ArduinoJson.h>
#include <lvgl.h>
#include "nav_state.h"   // MAX_ROUTE_POINTS + shared state declarations
#include "ui.h"

#define SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

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
static String rxBuffer = "";

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
    callActive        = false;
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

    Serial.print("NAV ");
    Serial.print(navInstruction);
    Serial.print(" in ");
    Serial.print(navDistanceToTurn);
    Serial.print("m, hdg ");
    Serial.print(navHeading);
    Serial.print(", ");
    Serial.print(routeLen);
    Serial.print(" pts, ");
    Serial.print(junctionRoadCount);
    Serial.println(" jct roads");

  } else if (strcmp(type, "incoming_call") == 0) {
    callActive = true;
    callCaller = (const char *)(doc["caller"] | "Incoming call");
    callNumber = (const char *)(doc["number"] | "");
    Serial.print("CALL from ");
    Serial.println(callCaller);

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
    std::string value = characteristic->getValue();
    if (value.empty()) return;

    rxBuffer += String(value.c_str());

    // Drain every complete (newline-terminated) frame in the buffer.
    int nl;
    while ((nl = rxBuffer.indexOf('\n')) >= 0) {
      String line = rxBuffer.substring(0, nl);
      rxBuffer.remove(0, nl + 1);
      line.trim();
      handleFrame(line);
    }

    // Guard against a runaway buffer if a newline never arrives.
    if (rxBuffer.length() > 1024) rxBuffer = "";
  }
};

// Restart advertising when a client disconnects so the phone can reconnect.
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) {
    Serial.println("BLE client connected");
  }
  void onDisconnect(BLEServer *server) {
    Serial.println("BLE client disconnected — re-advertising");
    navActive  = false;
    callActive = false;
    BLEDevice::startAdvertising();
  }
};

void setup() {
  Serial.begin(115200);

  BLEDevice::init("Trakio-Navigator");
  BLEDevice::setMTU(247);  // ask for a larger MTU so frames need fewer chunks

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(SERVICE_UUID);

  // Accept both write-with-response and write-without-response so the phone
  // can use whichever is faster.
  BLECharacteristic *characteristic = service->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  characteristic->setCallbacks(new RxCallbacks());

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("BLE READY — advertising as Trakio-Navigator");

  // ── Display + LVGL ──────────────────────────────────────
  lv_init();
  // TODO: initialise YOUR panel and register an LVGL display + draw buffer
  //   here. This is hardware-specific — e.g. a 320x240 landscape ILI9341/ST7789
  //   panel via TFT_eSPI + lv_disp_drv_register(), or the LVGL Arduino example.
  //   ui_init() must run AFTER a display is registered (it draws on the active
  //   screen).
  ui_init();
}

void loop() {
  lv_timer_handler();   // drive LVGL rendering
  ui_update();          // push the latest decoded nav state into the UI
  delay(5);
}
