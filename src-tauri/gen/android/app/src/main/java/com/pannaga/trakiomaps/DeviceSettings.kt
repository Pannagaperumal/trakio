package com.pannaga.trakiomaps

import android.content.Context
import org.json.JSONObject

// Rider preferences for the Trakio device, edited from the Device tab and read
// by PiNavigationService. Stored natively so the service sees them with the
// WebView closed.
internal data class DeviceSettings(
  val themeMode: String = THEME_AUTO,   // auto | day | night
  val nightBrightness: Int = 35,        // % — day is always 100
  val crashAlerts: Boolean = true,
  val parkGuard: Boolean = false,       // also keeps the link up between rides
) {
  fun toJson(): String = JSONObject()
    .put("themeMode", themeMode)
    .put("nightBrightness", nightBrightness)
    .put("crashAlerts", crashAlerts)
    .put("parkGuard", parkGuard)
    .toString()

  companion object {
    const val THEME_AUTO = "auto"
    const val THEME_DAY = "day"
    const val THEME_NIGHT = "night"
    private const val PREFS = "trakio_device"

    fun load(context: Context): DeviceSettings {
      val p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
      return DeviceSettings(
        themeMode = p.getString("themeMode", THEME_AUTO) ?: THEME_AUTO,
        nightBrightness = p.getInt("nightBrightness", 35),
        crashAlerts = p.getBoolean("crashAlerts", true),
        parkGuard = p.getBoolean("parkGuard", false),
      )
    }

    // Merge a (possibly partial) JSON patch from the WebView and persist it.
    fun update(context: Context, json: String): DeviceSettings {
      val cur = load(context)
      val o = JSONObject(json)
      val next = DeviceSettings(
        themeMode = o.optString("themeMode", cur.themeMode)
          .takeIf { it in setOf(THEME_AUTO, THEME_DAY, THEME_NIGHT) } ?: cur.themeMode,
        nightBrightness = o.optInt("nightBrightness", cur.nightBrightness).coerceIn(10, 100),
        crashAlerts = o.optBoolean("crashAlerts", cur.crashAlerts),
        parkGuard = o.optBoolean("parkGuard", cur.parkGuard),
      )
      context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
        .putString("themeMode", next.themeMode)
        .putInt("nightBrightness", next.nightBrightness)
        .putBoolean("crashAlerts", next.crashAlerts)
        .putBoolean("parkGuard", next.parkGuard)
        .apply()
      return next
    }
  }
}

// Last known device state, written by the service and polled by the Device tab.
internal object DeviceStatus {
  @Volatile var connected = false
  @Volatile var deviceName: String? = null
  @Volatile var mode = "off"            // off | ride | idle
  @Volatile var battery: Int? = null
  @Volatile var charging = false
  @Volatile var batteryMv: Int? = null
  @Volatile var batteryAt = 0L
  @Volatile var theme: String? = null

  fun toJson(): String = JSONObject()
    .put("connected", connected)
    .put("deviceName", deviceName ?: JSONObject.NULL)
    .put("mode", mode)
    .put("battery", battery ?: JSONObject.NULL)
    .put("charging", charging)
    .put("mv", batteryMv ?: JSONObject.NULL)
    .put("batteryAt", batteryAt)
    .put("theme", theme ?: JSONObject.NULL)
    .toString()
}
