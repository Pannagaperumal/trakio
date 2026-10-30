package com.pannaga.trakiomaps

import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.roundToInt
import kotlin.math.sin

// Road from the per-route Overpass corridor (fetched once in JS).
data class CorridorRoad(val major: Boolean, val name: String, val coords: List<PiNavigationService.LatLng>)

// Protocol v2 `map_area`: the streets around the rider, sent once per ~1.2 km
// square instead of on every nav frame. Coordinates are north-up metres from
// the area anchor (x = east, y = north); the device rotates the map itself.
internal object MapAreaBuilder {
  const val AREA_HALF_M = 600.0
  const val MAX_ROADS = 64
  const val MAX_POINTS = 8
  // Stay under the CrowPanel's 4 KB RX line buffer: that firmware ignores
  // map_area but would otherwise drop it mid-line and lose the next frame too.
  const val MAX_FRAME_BYTES = 3500
  private const val MIN_RUN_M = 15.0
  private const val METERS_PER_DEG = 111320.0

  data class Pt(val x: Double, val y: Double)
  data class AreaRoad(val major: Boolean, val name: String, val pts: List<Pt>)

  class MapArea(
    val id: Int,
    val anchorLat: Double,
    val anchorLng: Double,
    val roads: List<AreaRoad>,
    val frame: String,
  ) {
    fun localXY(lat: Double, lng: Double): Pt = project(lat, lng, anchorLat, anchorLng)

    // Name of the closest named road within maxDistM of the local point.
    fun streetAt(p: Pt, maxDistM: Double = 25.0): String? {
      var best: String? = null
      var bestDist = maxDistM
      for (road in roads) {
        if (road.name.isBlank()) continue
        for (i in 1 until road.pts.size) {
          val d = pointSegDist(p, road.pts[i - 1], road.pts[i])
          if (d < bestDist) {
            bestDist = d
            best = road.name
          }
        }
      }
      return best
    }
  }

  fun build(
    id: Int,
    anchorLat: Double,
    anchorLng: Double,
    corridor: List<CorridorRoad>,
    halfSize: Double = AREA_HALF_M,
    maxRoads: Int = MAX_ROADS,
    maxPoints: Int = MAX_POINTS,
    maxFrameBytes: Int = MAX_FRAME_BYTES,
  ): MapArea {
    data class Run(val major: Boolean, val name: String, val pts: List<Pt>, val dist: Double)

    val runs = mutableListOf<Run>()
    for (road in corridor) {
      if (road.coords.size < 2) continue
      val pts = road.coords.map { project(it.lat, it.lng, anchorLat, anchorLng) }
      for (run in clipToBox(pts, halfSize)) {
        if (length(run) < MIN_RUN_M) continue
        val simplified = simplify(run, maxPoints)
        runs += Run(road.major, road.name, simplified, simplified.minOf { hypot(it.x, it.y) })
      }
    }

    // Major roads first, then whatever is closest to the rider.
    val picked = runs.sortedWith(compareByDescending<Run> { it.major }.thenBy { it.dist }).take(maxRoads)
    val areaRoads = picked.map { AreaRoad(it.major, it.name, it.pts) }.toMutableList()

    var frame = encode(id, areaRoads)
    while (frame.toByteArray(Charsets.UTF_8).size > maxFrameBytes && areaRoads.isNotEmpty()) {
      areaRoads.removeAt(areaRoads.lastIndex)
      frame = encode(id, areaRoads)
    }
    return MapArea(id, anchorLat, anchorLng, areaRoads, frame)
  }

  // Anchor the next area a little ahead of the rider so most of it is road to come.
  fun anchorAhead(lat: Double, lng: Double, headingDeg: Double, aheadM: Double): PiNavigationService.LatLng {
    val rad = Math.toRadians(headingDeg)
    return PiNavigationService.LatLng(
      lat = lat + aheadM * cos(rad) / METERS_PER_DEG,
      lng = lng + aheadM * sin(rad) / (METERS_PER_DEG * cos(Math.toRadians(lat))),
    )
  }

  // True once the rider is close enough to the area edge that the screen
  // would start showing empty space.
  fun needsNewArea(area: MapArea?, lat: Double, lng: Double, edgeMarginM: Double): Boolean {
    if (area == null) return true
    val p = area.localXY(lat, lng)
    val limit = AREA_HALF_M - edgeMarginM
    return abs(p.x) > limit || abs(p.y) > limit
  }

  fun project(lat: Double, lng: Double, lat0: Double, lng0: Double): Pt = Pt(
    x = (lng - lng0) * METERS_PER_DEG * cos(Math.toRadians(lat0)),
    y = (lat - lat0) * METERS_PER_DEG,
  )

  private fun encode(id: Int, roads: List<AreaRoad>): String {
    val arr = JSONArray()
    for (road in roads) {
      val c = JSONArray()
      for (p in road.pts) c.put(JSONArray().put(p.x.roundToInt()).put(p.y.roundToInt()))
      arr.put(JSONObject().put("w", if (road.major) 2 else 1).put("c", c))
    }
    return JSONObject().put("type", "map_area").put("id", id).put("roads", arr).toString()
  }

  // Liang–Barsky clip of each segment to the square; consecutive clipped
  // segments that share an endpoint are joined into one run.
  internal fun clipToBox(pts: List<Pt>, half: Double): List<List<Pt>> {
    val out = mutableListOf<MutableList<Pt>>()
    var current: MutableList<Pt>? = null
    for (i in 1 until pts.size) {
      val seg = clipSegment(pts[i - 1], pts[i], half)
      if (seg == null) {
        current = null
        continue
      }
      val (a, b) = seg
      val cur = current
      if (cur != null && cur.last() == a) {
        cur += b
      } else {
        current = mutableListOf(a, b).also { out += it }
      }
      // A segment that leaves the box ends the run.
      if (b != pts[i]) current = null
    }
    return out
  }

  private fun clipSegment(a: Pt, b: Pt, half: Double): Pair<Pt, Pt>? {
    val dx = b.x - a.x
    val dy = b.y - a.y
    var t0 = 0.0
    var t1 = 1.0
    val p = doubleArrayOf(-dx, dx, -dy, dy)
    val q = doubleArrayOf(a.x + half, half - a.x, a.y + half, half - a.y)
    for (k in 0 until 4) {
      if (p[k] == 0.0) {
        if (q[k] < 0) return null
      } else {
        val r = q[k] / p[k]
        if (p[k] < 0) { if (r > t1) return null; if (r > t0) t0 = r }
        else { if (r < t0) return null; if (r < t1) t1 = r }
      }
    }
    val start = if (t0 == 0.0) a else Pt(a.x + t0 * dx, a.y + t0 * dy)
    val end = if (t1 == 1.0) b else Pt(a.x + t1 * dx, a.y + t1 * dy)
    return start to end
  }

  // Ramer–Douglas–Peucker with a growing tolerance until the run fits.
  internal fun simplify(pts: List<Pt>, maxPoints: Int): List<Pt> {
    if (pts.size <= maxPoints) return pts
    var tol = 2.0
    while (tol < 512.0) {
      val r = rdp(pts, tol)
      if (r.size <= maxPoints) return r
      tol *= 2
    }
    // Pathological input: fall back to even sampling, keeping both ends.
    return (0 until maxPoints).map { pts[it * (pts.size - 1) / (maxPoints - 1)] }
  }

  private fun rdp(pts: List<Pt>, tol: Double): List<Pt> {
    if (pts.size < 3) return pts
    var maxD = 0.0
    var idx = 0
    for (i in 1 until pts.lastIndex) {
      val d = pointSegDist(pts[i], pts.first(), pts.last())
      if (d > maxD) { maxD = d; idx = i }
    }
    if (maxD <= tol) return listOf(pts.first(), pts.last())
    val left = rdp(pts.subList(0, idx + 1), tol)
    val right = rdp(pts.subList(idx, pts.size), tol)
    return left.dropLast(1) + right
  }

  private fun pointSegDist(p: Pt, a: Pt, b: Pt): Double {
    val dx = b.x - a.x
    val dy = b.y - a.y
    val len2 = dx * dx + dy * dy
    val t = if (len2 > 0) (((p.x - a.x) * dx + (p.y - a.y) * dy) / len2).coerceIn(0.0, 1.0) else 0.0
    return hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy))
  }

  private fun length(pts: List<Pt>): Double {
    var acc = 0.0
    for (i in 1 until pts.size) acc += hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y)
    return acc
  }
}
