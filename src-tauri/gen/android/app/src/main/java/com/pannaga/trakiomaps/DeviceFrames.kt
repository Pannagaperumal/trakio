package com.pannaga.trakiomaps

import org.json.JSONObject

// Protocol v2 frames that aren't nav/map_area. Pure so they're unit-testable.
internal object DeviceFrames {
  // Day = full brightness in sunlight; night = rider-chosen dim level.
  fun theme(settings: DeviceSettings, lat: Double?, lng: Double?, nowMs: Long): String {
    val day = when (settings.themeMode) {
      DeviceSettings.THEME_DAY -> true
      DeviceSettings.THEME_NIGHT -> false
      // No fix yet: fall back to a 06:00–18:00 local guess.
      else -> if (lat != null && lng != null) SunTimes.isDaylight(lat, lng, nowMs) else localHourIsDay(nowMs)
    }
    return JSONObject()
      .put("type", "theme")
      .put("mode", if (day) "day" else "night")
      .put("brightness", if (day) 100 else settings.nightBrightness)
      .toString()
  }

  // Device clock: UTC epoch plus the display offset. Trakio shows Indian
  // Standard Time (UTC+5:30, no DST) regardless of the phone's zone.
  const val IST_OFFSET_MIN = 330

  fun time(nowMs: Long): String =
    JSONObject().put("type", "time").put("epoch", nowMs).put("tz", IST_OFFSET_MIN).toString()

  fun callState(active: Boolean): String =
    JSONObject().put("type", "call_state").put("state", if (active) "active" else "ended").toString()

  // "cancelled" extends the plan's sos_status: the rider tapped "I'm OK" on the phone.
  fun sosStatus(state: String): String =
    JSONObject().put("type", "sos_status").put("state", state).toString()

  // Reassembles newline-delimited JSON from notify packets. The firmware may
  // also send a short frame without a trailing newline in a single packet.
  class LineReader(private val maxBytes: Int = 2048) {
    private val buf = StringBuilder()

    fun feed(bytes: ByteArray): List<JSONObject> {
      buf.append(String(bytes, Charsets.UTF_8))
      val out = mutableListOf<JSONObject>()
      while (true) {
        val nl = buf.indexOf("\n")
        if (nl < 0) break
        parse(buf.substring(0, nl))?.let { out += it }
        buf.delete(0, nl + 1)
      }
      val rest = buf.toString().trim()
      if (rest.startsWith("{") && rest.endsWith("}")) {
        parse(rest)?.let { out += it; buf.setLength(0) }
      }
      if (buf.length > maxBytes) buf.setLength(0)
      return out
    }

    private fun parse(line: String): JSONObject? =
      line.trim().takeIf { it.isNotEmpty() }?.let { runCatching { JSONObject(it) }.getOrNull() }
  }

  private fun localHourIsDay(nowMs: Long): Boolean {
    val hour = java.util.Calendar.getInstance().apply { timeInMillis = nowMs }.get(java.util.Calendar.HOUR_OF_DAY)
    return hour in 6 until 18
  }
}
