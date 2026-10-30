package com.pannaga.trakiomaps

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.ZonedDateTime
import java.time.ZoneId

class DeviceFramesTest {
  private val ist = ZoneId.of("Asia/Kolkata")
  private fun istMs(y: Int, mo: Int, d: Int, h: Int, mi: Int = 0) =
    ZonedDateTime.of(y, mo, d, h, mi, 0, 0, ist).toInstant().toEpochMilli()

  @Test
  fun bengaluruSunriseAndSunsetAreRoughlyRight() {
    // Bengaluru, 21 Jun: sunrise ≈ 05:53, sunset ≈ 18:49 IST.
    val w = SunTimes.window(12.9716, 77.5946, istMs(2026, 6, 21, 12))
    assertNotNull(w)
    val rise = ZonedDateTime.ofInstant(java.time.Instant.ofEpochMilli(w!!.sunriseMs), ist)
    val set = ZonedDateTime.ofInstant(java.time.Instant.ofEpochMilli(w.sunsetMs), ist)
    assertEquals(5 * 60 + 53.0, (rise.hour * 60 + rise.minute).toDouble(), 5.0)
    assertEquals(18 * 60 + 49.0, (set.hour * 60 + set.minute).toDouble(), 5.0)
  }

  @Test
  fun daylightFlipsAtDuskAndDawn() {
    assertTrue(SunTimes.isDaylight(12.9716, 77.5946, istMs(2026, 10, 30, 12)))
    assertFalse(SunTimes.isDaylight(12.9716, 77.5946, istMs(2026, 10, 30, 21)))
    assertFalse(SunTimes.isDaylight(12.9716, 77.5946, istMs(2026, 10, 30, 4)))
  }

  @Test
  fun polarDayAndNight() {
    val juneNoonUtc = ZonedDateTime.of(2026, 6, 21, 12, 0, 0, 0, ZoneId.of("UTC")).toInstant().toEpochMilli()
    assertTrue(SunTimes.isDaylight(78.2, 15.6, juneNoonUtc))   // Svalbard, midnight sun
    assertFalse(SunTimes.isDaylight(-78.2, 15.6, juneNoonUtc)) // Antarctic winter
  }

  @Test
  fun themeFrameHonoursOverrideAndNightBrightness() {
    val noon = istMs(2026, 10, 30, 12)
    val night = JSONObject(DeviceFrames.theme(DeviceSettings(themeMode = "night", nightBrightness = 40), 12.97, 77.59, noon))
    assertEquals("theme", night.getString("type"))
    assertEquals("night", night.getString("mode"))
    assertEquals(40, night.getInt("brightness"))

    val auto = JSONObject(DeviceFrames.theme(DeviceSettings(), 12.97, 77.59, noon))
    assertEquals("day", auto.getString("mode"))
    assertEquals(100, auto.getInt("brightness"))
  }

  @Test
  fun lineReaderReassemblesSplitFrames() {
    val reader = DeviceFrames.LineReader()
    assertTrue(reader.feed("{\"type\":\"batt".toByteArray()).isEmpty())
    val out = reader.feed("ery\",\"percent\":82}\n{\"type\":\"sos_cancel\"}\n".toByteArray())
    assertEquals(listOf("battery", "sos_cancel"), out.map { it.getString("type") })
    assertEquals(82, out[0].getInt("percent"))
  }

  @Test
  fun lineReaderAcceptsUnterminatedSinglePacketFrame() {
    val out = DeviceFrames.LineReader().feed("{\"type\":\"call_action\",\"action\":\"accept\"}".toByteArray())
    assertEquals("accept", out.single().getString("action"))
  }

  @Test
  fun lineReaderSkipsGarbage() {
    val out = DeviceFrames.LineReader().feed("not json\n{\"type\":\"sos_cancel\"}\n".toByteArray())
    assertEquals(1, out.size)
  }
}
