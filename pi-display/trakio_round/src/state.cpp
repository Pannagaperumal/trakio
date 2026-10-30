#include "state.h"
#include <ArduinoJson.h>
#include <math.h>
#include "board.h"

NavState nav;
MapArea area;
JunctionRoads jroads;
CallState call;
Settings settings;
bool sosSent = false;
uint8_t chimeKind = 0;
uint32_t stateEvents = 0;

static int64_t clockOffsetMs = 0;   // epoch = millis() + offset
static bool clockFromTimeFrame = false;

int64_t clock_epoch_ms() { return clockOffsetMs ? (int64_t)millis() + clockOffsetMs : 0; }
void clock_set_epoch_ms(int64_t ms) { clockOffsetMs = ms - (int64_t)millis(); }

static void copyStr(char *dst, size_t cap, const char *src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = 0;
}

// heading-up local (x right, y forward) → north-up map frame around (ox, oy)
static void localToMap(float lx, float ly, float hRad, float ox, float oy, float &mx, float &my) {
  float c = cosf(hRad), s = sinf(hRad);
  mx = ox + lx * c + ly * s;
  my = oy - lx * s + ly * c;
}

static void onRouteStart(JsonDocument &doc) {
  nav = NavState();
  nav.active = true;
  copyStr(nav.dest, sizeof(nav.dest), doc["destination"] | "Destination");
  nav.totalDist = doc["totalDistance"] | 0;
  nav.totalDur = doc["totalDuration"] | 0;
  nav.startedMs = millis();
  jroads.n = 0;
  stateEvents |= EV_ROUTE_START;
}

static void onNav(JsonDocument &doc) {
  if (!nav.active) {                       // joined mid-ride (e.g. after a reboot)
    nav.active = true;
    nav.startedMs = millis();
    stateEvents |= EV_ROUTE_START;
  }
  nav.heading = doc["heading"] | 0;
  nav.distanceToTurn = doc["distanceToTurn"] | 0;
  copyStr(nav.instruction, sizeof(nav.instruction), doc["instruction"] | "STRAIGHT");
  if (doc["street"].is<const char *>()) copyStr(nav.street, sizeof(nav.street), doc["street"]);
  nav.remainingDist = doc["remainingDistance"] | 0;
  nav.remainingTime = doc["remainingTime"] | 0;
  nav.eta = doc["eta"] | (int64_t)0;
  nav.speedKmh = doc["speed"] | -1;
  bool arrived = doc["arrived"] | false;

  // The phone's clock: eta − remainingTime. Good to a second, until a `time` frame arrives.
  if (!clockFromTimeFrame && nav.eta > 0) {
    int64_t phoneNow = nav.eta - (int64_t)nav.remainingTime * 1000;
    if (llabs(phoneNow - clock_epoch_ms()) > 2000) clock_set_epoch_ms(phoneNow);
  }

  int areaId = doc["area"] | 0;
  nav.v2 = areaId != 0 && areaId == area.id;
  float ox = nav.v2 ? (float)(doc["ax"] | 0) : 0.0f;
  float oy = nav.v2 ? (float)(doc["ay"] | 0) : 0.0f;
  float h = nav.heading * (float)M_PI / 180.0f;
  nav.riderX = ox;
  nav.riderY = oy;

  nav.routeN = 0;
  for (JsonArray pt : doc["route"].as<JsonArray>()) {
    if (nav.routeN >= MAX_ROUTE_PTS) break;
    localToMap(pt[0] | 0, pt[1] | 0, h, ox, oy, nav.routeX[nav.routeN], nav.routeY[nav.routeN]);
    nav.routeN++;
  }

  // v1 junction context roads → drawn as minor streets
  jroads.n = 0;
  if (!nav.v2) {
    for (JsonObject road : doc["junctionRoads"].as<JsonArray>()) {
      if (jroads.n >= MAX_JROADS) break;
      Road &r = jroads.roads[jroads.n];
      r.n = 0;
      r.w = 1;
      for (JsonArray pt : road["coords"].as<JsonArray>()) {
        if (r.n >= MAX_ROAD_PTS) break;
        float mx, my;
        localToMap(pt[0] | 0, pt[1] | 0, h, ox, oy, mx, my);
        r.x[r.n] = (int16_t)lroundf(mx);
        r.y[r.n] = (int16_t)lroundf(my);
        r.n++;
      }
      if (r.n >= 2) jroads.n++;
    }
  }

  nav.lastFrameMs = millis();
  nav.seq++;
  stateEvents |= EV_NAV;
  if (arrived && !nav.arrived) stateEvents |= EV_ARRIVED;
  nav.arrived = arrived;
}

static void onMapArea(JsonDocument &doc) {
  area.id = doc["id"] | 0;
  area.n = 0;
  for (JsonObject road : doc["roads"].as<JsonArray>()) {
    if (area.n >= MAX_AREA_ROADS) break;
    Road &r = area.roads[area.n];
    r.n = 0;
    r.w = (uint8_t)(road["w"] | 1);
    for (JsonArray pt : road["c"].as<JsonArray>()) {
      if (r.n >= MAX_ROAD_PTS) break;
      r.x[r.n] = pt[0] | 0;
      r.y[r.n] = pt[1] | 0;
      r.n++;
    }
    if (r.n >= 2) area.n++;
  }
  stateEvents |= EV_AREA;
}

void state_handle_frame(const char *line, size_t len) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line, len);
  if (err) {
    Serial.printf("[link] bad frame (%s), %u bytes\n", err.c_str(), (unsigned)len);
    return;
  }
  const char *type = doc["type"] | "";

  if (!strcmp(type, "nav")) {
    onNav(doc);
  } else if (!strcmp(type, "map_area")) {
    onMapArea(doc);
    Serial.printf("[link] map_area #%d: %d roads, %u bytes\n", area.id, area.n, (unsigned)len);
  } else if (!strcmp(type, "route_start")) {
    onRouteStart(doc);
    Serial.printf("[link] route_start → %s, %ld m\n", nav.dest, nav.totalDist);
  } else if (!strcmp(type, "route_end")) {
    nav.active = false;
    nav.routeN = 0;
    jroads.n = 0;
    stateEvents |= EV_ROUTE_END;
  } else if (!strcmp(type, "incoming_call")) {
    call.phase = CALL_RINGING;
    copyStr(call.caller, sizeof(call.caller), doc["caller"] | "Incoming call");
    copyStr(call.number, sizeof(call.number), doc["number"] | "");
    call.sinceMs = millis();
    stateEvents |= EV_CALL;
  } else if (!strcmp(type, "call_state") || !strcmp(type, "call_end")) {
    const char *st = doc["state"] | "ended";
    if (call.phase != CALL_NONE && !strcmp(st, "active")) {
      call.phase = CALL_ACTIVE;
      call.sinceMs = millis();
    } else {
      call.phase = CALL_NONE;
    }
    stateEvents |= EV_CALL;
  } else if (!strcmp(type, "theme")) {
    settings.theme = !strcmp(doc["mode"] | "night", "day") ? THEME_DAY : THEME_NIGHT;
    settings.brightness = constrain((int)(doc["brightness"] | settings.brightness), 5, 100);
    stateEvents |= EV_THEME;
  } else if (!strcmp(type, "time")) {
    clock_set_epoch_ms(doc["epoch"] | (int64_t)0);
    settings.tzOffsetMin = doc["tz"] | settings.tzOffsetMin;
    clockFromTimeFrame = true;
    stateEvents |= EV_TIME;
  } else if (!strcmp(type, "chime")) {
    chimeKind = doc["kind"] | 0;
    stateEvents |= EV_CHIME;
  } else if (!strcmp(type, "sos_status")) {
    sosSent = !strcmp(doc["state"] | "", "sent");
    stateEvents |= EV_SOS;
  } else {
    Serial.printf("[link] ignored frame type '%s'\n", type);
  }
}
