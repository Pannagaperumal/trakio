// Shared navigation state decoded from the phone's BLE frames.
// esp.cpp defines these; ui.cpp reads them to render the LVGL UI.
#pragma once
#include <Arduino.h>

#define MAX_ROUTE_POINTS 16

// Junction context roads sent with each nav frame. The branch flagged
// `highlighted` is the one the rider takes — draw it emphasised.
#define MAX_JUNCTION_ROADS       8
#define MAX_JUNCTION_ROAD_POINTS 4

// ── Trip summary (set at route_start) ──
extern String     tripDestination;   // destination name
extern long       tripTotalDist;     // total route distance, metres
extern long       tripTotalDur;      // total route duration, seconds

// ── Live navigation state ──
extern volatile bool navActive;      // true between route_start and route_end
extern int        navHeading;        // degrees 0..359 (route is already heading-up)
extern int        navDistanceToTurn; // metres to next maneuver
extern String     navInstruction;    // "LEFT" / "RIGHT" / "STRAIGHT" / "UTURN" / ...
extern long       navRemainingDist;  // metres left to destination
extern long       navRemainingTime;  // seconds left to destination
extern long long  navEta;            // arrival time, epoch milliseconds
extern bool       navArrived;

// ── Upcoming route polyline, heading-up local frame in metres ──
// routeX = right (+) / left (-), routeY = forward (+, up on screen). [0]=rider.
extern int        routeX[MAX_ROUTE_POINTS];
extern int        routeY[MAX_ROUTE_POINTS];
extern int        routeLen;

// ── Junction context roads, same heading-up local-metres frame as route ──
// junctionHighlighted[i] marks the branch the rider should follow.
extern int        junctionRoadCount;
extern int        junctionRoadLen[MAX_JUNCTION_ROADS];
extern bool       junctionHighlighted[MAX_JUNCTION_ROADS];
extern int        junctionX[MAX_JUNCTION_ROADS][MAX_JUNCTION_ROAD_POINTS];
extern int        junctionY[MAX_JUNCTION_ROADS][MAX_JUNCTION_ROAD_POINTS];

// ── Incoming call ──
extern bool       callActive;
extern String     callCaller;
extern String     callNumber;
