// Navigation/call/theme state decoded from the phone's JSON frames.
//
// Geometry lives in one north-up "map frame" in metres (x = east, y = north):
//  • v2 phones send a `map_area` (roads around an anchor) once, then every
//    `nav` frame carries the rider's position in it (`ax`/`ay`).
//  • v1 phones (current app) send only heading-up `route` + `junctionRoads`
//    per frame; those are rotated into a map frame whose origin is the rider
//    at that frame.
// Everything here is written only from the loop task.
#pragma once
#include <Arduino.h>
#include "board.h"

#define MAX_ROUTE_PTS   16
#define MAX_AREA_ROADS  192
#define MAX_ROAD_PTS    8
#define MAX_JROADS      8

struct Road {
  uint8_t n;
  uint8_t w;                    // 1 minor, 2 major
  int16_t x[MAX_ROAD_PTS];
  int16_t y[MAX_ROAD_PTS];
};

struct NavState {
  bool active = false;
  bool arrived = false;
  char dest[48] = "";
  long totalDist = 0;           // m
  long totalDur = 0;            // s
  char instruction[16] = "";
  char street[48] = "";
  int  distanceToTurn = 0;      // m
  long remainingDist = 0;       // m
  long remainingTime = 0;       // s
  int64_t eta = 0;              // epoch ms
  int  speedKmh = -1;           // -1 = phone didn't say
  bool v2 = false;              // geometry is in a phone-sent map area
  float riderX = 0, riderY = 0; // target pose in the map frame
  float heading = 0;            // deg clockwise from north
  int  routeN = 0;
  float routeX[MAX_ROUTE_PTS], routeY[MAX_ROUTE_PTS];
  uint32_t startedMs = 0;
  uint32_t lastFrameMs = 0;
  uint32_t seq = 0;             // bumps on every nav frame
};

struct MapArea {
  int id = 0;
  int n = 0;
  Road roads[MAX_AREA_ROADS];
};

struct JunctionRoads {          // v1 only, already in the map frame
  int n = 0;
  Road roads[MAX_JROADS];
};

enum CallPhase : uint8_t { CALL_NONE, CALL_RINGING, CALL_ACTIVE };
struct CallState {
  CallPhase phase = CALL_NONE;
  char caller[40] = "";
  char number[24] = "";
  uint32_t sinceMs = 0;
};

enum Theme : uint8_t { THEME_NIGHT = 0, THEME_DAY = 1 };
struct Settings {
  Theme theme = THEME_NIGHT;
  uint8_t brightness = 60;      // %, as last sent by the phone
  int16_t tzOffsetMin = DEFAULT_TZ_OFFSET_MIN;
};

extern NavState nav;
extern MapArea area;
extern JunctionRoads jroads;
extern CallState call;
extern Settings settings;
extern bool sosSent;
extern uint8_t chimeKind;      // last `chime` frame (0 test, 1 turn soon, 2 turn now)

// Events raised by frames, consumed (and cleared) by the app loop.
enum : uint32_t {
  EV_ROUTE_START = 1 << 0,
  EV_ROUTE_END   = 1 << 1,
  EV_NAV         = 1 << 2,
  EV_ARRIVED     = 1 << 3,
  EV_CALL        = 1 << 4,
  EV_THEME       = 1 << 5,
  EV_AREA        = 1 << 6,
  EV_SOS         = 1 << 7,
  EV_TIME        = 1 << 8,
  EV_CHIME       = 1 << 9,
};
extern uint32_t stateEvents;

void state_handle_frame(const char *line, size_t len);

// Wall clock from the phone (epoch ms, UTC); 0 until known.
int64_t clock_epoch_ms();
void clock_set_epoch_ms(int64_t ms);
