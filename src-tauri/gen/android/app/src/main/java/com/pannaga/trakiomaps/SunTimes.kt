package com.pannaga.trakiomaps

import kotlin.math.acos
import kotlin.math.asin
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.sin

// Offline sunrise/sunset (NOAA "sunrise equation"), accurate to a minute or
// two — plenty for picking the display's day/night theme. No network needed.
internal object SunTimes {
  private const val J2000 = 2451545.0
  private const val DAY_MS = 86_400_000.0
  // Sun's centre 0.833° below the horizon = refraction + solar radius.
  private const val SUN_ALTITUDE_DEG = -0.833

  data class Window(val sunriseMs: Long, val sunsetMs: Long)

  // Sunrise/sunset for the solar day containing nowMs, or null during polar
  // day/night (the caller then treats it as always day / always night).
  fun window(lat: Double, lng: Double, nowMs: Long): Window? {
    val julianDay = nowMs / DAY_MS + 2440587.5
    val n = floor(julianDay - J2000 + 0.0008 - (-lng) / 360.0 + 0.5)
    val meanSolarNoon = n - lng / 360.0
    val m = Math.toRadians((357.5291 + 0.98560028 * meanSolarNoon) % 360.0)
    val c = 1.9148 * sin(m) + 0.02 * sin(2 * m) + 0.0003 * sin(3 * m)
    val lambda = Math.toRadians((Math.toDegrees(m) + c + 180.0 + 102.9372) % 360.0)
    val transit = J2000 + meanSolarNoon + 0.0053 * sin(m) - 0.0069 * sin(2 * lambda)
    val decl = asin(sin(lambda) * sin(Math.toRadians(23.4397)))
    val latRad = Math.toRadians(lat)
    val cosH = (sin(Math.toRadians(SUN_ALTITUDE_DEG)) - sin(latRad) * sin(decl)) / (cos(latRad) * cos(decl))
    if (cosH < -1.0 || cosH > 1.0) return null
    val hDays = Math.toDegrees(acos(cosH)) / 360.0
    return Window(
      sunriseMs = julianToMs(transit - hDays),
      sunsetMs = julianToMs(transit + hDays),
    )
  }

  fun isDaylight(lat: Double, lng: Double, nowMs: Long): Boolean {
    val w = window(lat, lng, nowMs)
      // Polar: declination on the same side as the latitude → midnight sun.
      ?: return polarDay(lat, nowMs)
    return nowMs in w.sunriseMs until w.sunsetMs
  }

  private fun polarDay(lat: Double, nowMs: Long): Boolean {
    val dayOfYear = ((nowMs / DAY_MS) % 365.25)
    val northernSummer = dayOfYear in 79.0..265.0
    return (lat > 0) == northernSummer
  }

  private fun julianToMs(jd: Double): Long = ((jd - 2440587.5) * DAY_MS).toLong()
}
