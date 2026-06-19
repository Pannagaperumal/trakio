package com.pannaga.trakiomaps

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BleNavFrameBuilderTest {
  @Test
  fun buildsBleNavPayloadWithCompactJunctionRoads() {
    val step = PiNavigationService.RouteStep(
      instructions = "Turn right onto Market Road",
      maneuver = "turn-right",
      endLocation = PiNavigationService.LatLng(12.917822, 77.573551),
      distanceM = 120.0,
      durationS = 18.0,
      junctionRoads = PiNavigationService.JunctionRoads(
        highlightedBranchId = "take-right",
        roads = listOf(
          PiNavigationService.JunctionRoad(
            id = "straight",
            name = "Straight Road",
            highway = "primary",
            coords = listOf(
              PiNavigationService.LatLng(12.917822, 77.573551),
              PiNavigationService.LatLng(12.918000, 77.573551),
            ),
          ),
          PiNavigationService.JunctionRoad(
            id = "take-right",
            name = "Market Road",
            highway = "primary",
            coords = listOf(
              PiNavigationService.LatLng(12.917822, 77.573551),
              PiNavigationService.LatLng(12.917860, 77.573700),
              PiNavigationService.LatLng(12.917910, 77.573820),
            ),
          ),
        ),
      ),
    )

    val payload = BleNavFrameBuilder.buildNavFrame(
      lat = 12.917700,
      lng = 77.573400,
      heading = 0.0,
      distToEnd = 95.0,
      step = step,
      navStepIdx = 0,
      routeCoords = listOf(
        PiNavigationService.LatLng(12.917700, 77.573400),
        PiNavigationService.LatLng(12.917760, 77.573460),
        PiNavigationService.LatLng(12.917822, 77.573551),
        PiNavigationService.LatLng(12.917860, 77.573700),
      ),
      remainingStepDistances = listOf(250.0, 400.0),
      remainingStepDurations = listOf(40.0, 65.0),
      lookAheadMeters = 200.0,
      maxRoutePoints = 10,
      maxJunctionRoadPoints = 3,
      nowMillis = 1_700_000_000_000L,
    )

    println("BLE payload: $payload")

    val json = JSONObject(payload)
    assertEquals("nav", json.getString("type"))
    assertEquals("RIGHT", json.getString("instruction"))
    assertTrue(json.getJSONArray("route").length() >= 2)
    assertEquals(2, json.getJSONArray("junctionRoads").length())

    val highlightedRoad = json.getJSONArray("junctionRoads").getJSONObject(1)
    assertTrue(highlightedRoad.getBoolean("highlighted"))
    assertTrue(highlightedRoad.getJSONArray("coords").length() <= 3)
  }
}