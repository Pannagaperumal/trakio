package com.pannaga.trakiomaps

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class MapAreaBuilderTest {
  private val lat0 = 12.9716
  private val lng0 = 77.5946
  private val mPerDegLng = 111320.0 * Math.cos(Math.toRadians(lat0))

  // Road from (east, north) metres to (east, north) metres, relative to lat0/lng0.
  private fun road(major: Boolean, name: String, vararg pts: Pair<Double, Double>) = CorridorRoad(
    major = major,
    name = name,
    coords = pts.map { (e, n) -> PiNavigationService.LatLng(lat0 + n / 111320.0, lng0 + e / mPerDegLng) },
  )

  @Test
  fun buildsNorthUpFrameRelativeToAnchor() {
    val area = MapAreaBuilder.build(
      id = 7, anchorLat = lat0, anchorLng = lng0,
      corridor = listOf(road(true, "Market Street", -200.0 to 0.0, 200.0 to 0.0)),
    )
    val json = JSONObject(area.frame)
    assertEquals("map_area", json.getString("type"))
    assertEquals(7, json.getInt("id"))
    val c = json.getJSONArray("roads").getJSONObject(0).getJSONArray("c")
    assertEquals(2, json.getJSONArray("roads").getJSONObject(0).getInt("w"))
    assertTrue(abs(c.getJSONArray(0).getInt(0) + 200) <= 1)
    assertTrue(abs(c.getJSONArray(1).getInt(0) - 200) <= 1)
    assertTrue(abs(c.getJSONArray(0).getInt(1)) <= 1)
  }

  @Test
  fun clipsRoadsToTheAreaSquare() {
    val area = MapAreaBuilder.build(
      id = 1, anchorLat = lat0, anchorLng = lng0,
      corridor = listOf(road(false, "", -2000.0 to 100.0, 2000.0 to 100.0)),
    )
    val pts = area.roads.single().pts
    assertEquals(-MapAreaBuilder.AREA_HALF_M, pts.first().x, 1.0)
    assertEquals(MapAreaBuilder.AREA_HALF_M, pts.last().x, 1.0)
  }

  @Test
  fun dropsRoadsOutsideTheArea() {
    val area = MapAreaBuilder.build(
      id = 1, anchorLat = lat0, anchorLng = lng0,
      corridor = listOf(road(true, "Far", 5000.0 to 5000.0, 5200.0 to 5000.0)),
    )
    assertTrue(area.roads.isEmpty())
  }

  @Test
  fun capsPointsPerRoadAndRoadCount() {
    val wiggly = (0..60).map { i -> (i * 10.0 - 300.0) to (if (i % 2 == 0) 0.0 else 40.0) }.toTypedArray()
    val corridor = (0 until 100).map { road(it % 3 == 0, "r$it", *wiggly) }
    val area = MapAreaBuilder.build(id = 1, anchorLat = lat0, anchorLng = lng0, corridor = corridor)
    assertTrue(area.roads.size <= MapAreaBuilder.MAX_ROADS)
    assertTrue(area.roads.all { it.pts.size <= MapAreaBuilder.MAX_POINTS })
    assertTrue(area.frame.toByteArray().size <= MapAreaBuilder.MAX_FRAME_BYTES)
  }

  @Test
  fun prefersMajorRoads() {
    val corridor = (0 until 200).map { road(it < 5, "r$it", (it - 100.0) to -300.0, (it - 100.0) to 300.0) }
    val area = MapAreaBuilder.build(id = 1, anchorLat = lat0, anchorLng = lng0, corridor = corridor, maxRoads = 10)
    assertEquals(5, area.roads.count { it.major })
  }

  @Test
  fun splitsRoadThatLeavesAndReentersTheArea() {
    val runs = MapAreaBuilder.clipToBox(
      listOf(
        MapAreaBuilder.Pt(-100.0, 0.0), MapAreaBuilder.Pt(100.0, 0.0),
        MapAreaBuilder.Pt(100.0, 900.0), MapAreaBuilder.Pt(200.0, 900.0),
        MapAreaBuilder.Pt(200.0, 0.0), MapAreaBuilder.Pt(300.0, 0.0),
      ),
      half = 600.0,
    )
    assertEquals(2, runs.size)
  }

  @Test
  fun findsStreetUnderTheRider() {
    val area = MapAreaBuilder.build(
      id = 1, anchorLat = lat0, anchorLng = lng0,
      corridor = listOf(
        road(true, "Market Street", -300.0 to 0.0, 300.0 to 0.0),
        road(false, "Side Lane", 0.0 to 100.0, 0.0 to 300.0),
      ),
    )
    assertEquals("Market Street", area.streetAt(MapAreaBuilder.Pt(50.0, 8.0)))
    assertEquals("Side Lane", area.streetAt(MapAreaBuilder.Pt(5.0, 200.0)))
    assertNull(area.streetAt(MapAreaBuilder.Pt(200.0, 200.0)))
  }

  @Test
  fun reanchorsNearTheEdge() {
    val area = MapAreaBuilder.build(id = 1, anchorLat = lat0, anchorLng = lng0, corridor = emptyList())
    assertFalse(MapAreaBuilder.needsNewArea(area, lat0 + 200 / 111320.0, lng0, 250.0))
    assertTrue(MapAreaBuilder.needsNewArea(area, lat0 + 400 / 111320.0, lng0, 250.0))
    assertTrue(MapAreaBuilder.needsNewArea(null, lat0, lng0, 250.0))
  }

  @Test
  fun anchorAheadFollowsHeading() {
    val east = MapAreaBuilder.anchorAhead(lat0, lng0, 90.0, 300.0)
    val p = MapAreaBuilder.project(east.lat, east.lng, lat0, lng0)
    assertEquals(300.0, p.x, 1.0)
    assertEquals(0.0, p.y, 1.0)
  }
}
