package com.pannaga.trakiomaps

import android.Manifest
import android.annotation.SuppressLint
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.provider.ContactsContract
import android.telecom.TelecomManager
import android.telephony.TelephonyManager
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import org.json.JSONArray
import org.json.JSONObject
import java.util.ArrayDeque
import java.util.UUID
import kotlin.math.cos
import kotlin.math.roundToInt

class PiNavigationService : Service(), LocationListener {
  companion object {
    const val ACTION_START_NAVIGATION = "com.pannaga.trakiomaps.action.START_NAVIGATION"
    const val ACTION_STOP_NAVIGATION = "com.pannaga.trakiomaps.action.STOP_NAVIGATION"
    // Idle link: stay connected between rides (park guard, battery, calls).
    const val ACTION_START_LINK = "com.pannaga.trakiomaps.action.START_LINK"
    const val ACTION_STOP_LINK = "com.pannaga.trakiomaps.action.STOP_LINK"
    const val ACTION_CRASH_OK = "com.pannaga.trakiomaps.action.CRASH_OK"
    const val EXTRA_ROUTE_PAYLOAD = "route_payload"

    // Route payloads (with the road corridor) can exceed the ~1 MB Binder
    // limit for Intent extras, so the activity stages them here instead.
    @Volatile private var stagedRoutePayload: String? = null
    fun stageRoutePayload(payload: String) { stagedRoutePayload = payload }

    // Running instance, for settings / corridor updates from the WebView bridge.
    @Volatile var instance: PiNavigationService? = null
      private set

    // logcat tag — filter with: adb logcat -s TrakioBLE
    private const val TAG = "TrakioBLE"

    private const val NOTIFICATION_CHANNEL_ID = "trakio_navigation_tracking"
    private const val NOTIFICATION_ID = 4102
    private const val ALERT_CHANNEL_ID = "trakio_device_alerts"
    private const val CRASH_NOTIFICATION_ID = 4103
    private const val PARK_NOTIFICATION_ID = 4104
    private const val BATTERY_NOTIFICATION_ID = 4105
    private const val PARK_ALERT_DEDUPE_MS = 60_000L
    private const val LOW_BATTERY_PERCENT = 15
    private const val ADVANCE_THRESHOLD_METERS = 35.0
    private const val ARRIVAL_THRESHOLD_METERS = 25.0
    private const val CALL_DEDUPE_MS = 8000L

    // ── Minimal-map tuning (mirrors the old JS computeMiniMap) ──
    private const val LOOK_AHEAD_METERS = 200.0  // route distance ahead to send
    private const val MAX_ROUTE_POINTS = 10      // cap on polyline vertices
    private const val METERS_PER_DEG = 111320.0
    private const val MAX_JUNCTION_ROAD_POINTS = 3

    // ── Nordic UART Service (NUS) — match these UUIDs in the ESP32 firmware.
    // The phone writes JSON frames (newline-delimited) to the RX characteristic.
    private val NUS_SERVICE: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    private val NUS_RX_CHAR: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    // Device → phone notify characteristic (optional: the CrowPanel lacks it).
    private val NUS_TX_CHAR: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
    private val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    // Prefer a bonded device whose name contains this (case-insensitive);
    // otherwise the first bonded device is used. Empty = no name preference.
    private const val TARGET_NAME_SUBSTR = "trakio"

    // ── Smooth frame interpolation between GPS fixes ──
    // Every tick dead-reckons position from the last fix and eases heading;
    // frames go out at an adaptive rate (the device animates in between).
    private const val EMIT_TICK_MS = 250L
    private const val EMIT_MOVING_MS = 250L       // 4 Hz above MOVING_SPEED
    private const val EMIT_SLOW_MS = 1000L        // 1 Hz below it
    private const val HEARTBEAT_MS = 5000L        // when stopped / nothing changed
    private const val MOVING_SPEED = 2.0          // m/s
    private const val STOPPED_SPEED = 0.5         // m/s
    private const val MAX_EXTRAPOLATION_M = 40.0  // cap drift if GPS stalls
    private const val HEADING_EASE = 0.3          // per-tick heading smoothing (0..1)
    private const val POS_EASE = 0.45             // per-tick position correction (0..1)

    // GPS cadence follows speed.
    private const val GPS_RIDING_MS = 1000L
    private const val GPS_STOPPED_MS = 3000L

    // ── Map areas (protocol v2) ──
    private const val AREA_EDGE_MARGIN_M = 250.0  // re-anchor this close to the edge
    private const val AREA_AHEAD_M = 300.0        // new anchor sits ahead of the rider
    private const val THEME_CHECK_MS = 60_000L

    private const val REQUESTED_MTU = 247
    private const val RECONNECT_BASE_DELAY_MS = 2000L
    private const val RECONNECT_MAX_DELAY_MS = 15000L
    private const val MTU_FALLBACK_DELAY_MS = 1500L   // discover services if MTU cb never fires
    private const val WRITE_TIMEOUT_MS = 2000L        // recover if a write callback is lost
  }

  private var locationManager: LocationManager? = null
  private var phoneStateReceiver: BroadcastReceiver? = null
  private var btStateReceiver: BroadcastReceiver? = null
  private val mainHandler = Handler(Looper.getMainLooper())

  private var routeSteps: List<RouteStep> = emptyList()
  private var routeCoords: List<LatLng> = emptyList()
  // Static trip summary frame (destination + totals), resent on every (re)connect.
  @Volatile private var routeStartFrame: String? = null
  private var navStepIdx = 0
  private var navArrived = false
  @Volatile private var navActive = false
  private var lastCallSignature = ""
  private var lastCallAt = 0L
  private var lastPhoneState: String? = null

  // Idle link (between rides) — see ACTION_START_LINK.
  @Volatile private var linkActive = false
  private val linkWanted get() = navActive || linkActive
  private var settings = DeviceSettings()

  // ── Protocol v2 state ──
  private var corridorRoads: List<CorridorRoad> = emptyList()
  private var currentArea: MapAreaBuilder.MapArea? = null
  private var areaSeq = 0
  private var currentStreet = ""
  @Volatile private var themeFrame: String? = null
  private var moving = false
  private var gpsIntervalMs = 0L
  private var lastEmitAt = 0L
  private var lastNavKey = ""
  private var lastParkAlertAt = 0L
  private var lowBatteryWarned = false
  private val deviceReader = DeviceFrames.LineReader()

  // ── Smoothing state: last GPS fix + eased position/heading we emit from ──
  private var fixLat = 0.0
  private var fixLng = 0.0
  private var fixTimeMs = 0L
  private var fixSpeed = 0.0          // m/s along travel
  private var prevFixLat = 0.0
  private var prevFixLng = 0.0
  private var prevFixTimeMs = 0L
  private var targetHeading = 0.0     // latest raw bearing (deg)
  private var smoothHeading = 0.0     // eased heading we render with
  private var emitLat = 0.0           // last emitted (eased) position
  private var emitLng = 0.0
  private var smoothInit = false

  // High-rate emitter: builds an interpolated frame ~every EMIT_INTERVAL_MS so
  // the display sees small, continuous changes between 1 Hz GPS fixes.
  private val emitRunnable = object : Runnable {
    override fun run() {
      if (navActive && !navArrived) emitSmoothFrame()
      if (navActive) mainHandler.postDelayed(this, EMIT_TICK_MS)
    }
  }

  // Sunrise/sunset can flip the theme mid-ride; check once a minute.
  private val themeRunnable = object : Runnable {
    override fun run() {
      if (!linkWanted) return
      refreshTheme()
      mainHandler.postDelayed(this, THEME_CHECK_MS)
    }
  }

  // ── BLE state (touched from both the main thread and GATT callback thread) ──
  @Volatile private var bluetoothGatt: BluetoothGatt? = null
  @Volatile private var rxChar: BluetoothGattCharacteristic? = null
  @Volatile private var discoveredRxChar: BluetoothGattCharacteristic? = null
  @Volatile private var mtu = 23
  @Volatile private var servicesReady = false
  private var reconnectAttempts = 0
  private val writeQueue = ArrayDeque<ByteArray>()
  private var writing = false
  private var writeSeq = 0L
  private val pendingFrames = ArrayDeque<String>()

  override fun onBind(intent: Intent?): IBinder? = null

  override fun onCreate() {
    super.onCreate()
    locationManager = getSystemService(Context.LOCATION_SERVICE) as? LocationManager
    settings = DeviceSettings.load(this)
    instance = this
    ensureNotificationChannel()
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    when (intent?.action) {
      ACTION_START_NAVIGATION -> {
        val payload = intent.getStringExtra(EXTRA_ROUTE_PAYLOAD)
          ?: stagedRoutePayload.also { stagedRoutePayload = null }
        if (payload.isNullOrBlank()) {
          Log.w(TAG, "start ignored: empty route payload")
          stopSelf()
          return START_NOT_STICKY
        }

        // Going foreground can throw on Android 14+ if the location runtime
        // permission isn't granted — never let that crash the app.
        if (!startForegroundSafely(ride = true)) {
          if (!linkActive) stopSelf()
          return START_NOT_STICKY
        }

        navActive = true
        DeviceStatus.mode = "ride"
        navArrived = false
        navStepIdx = 0
        smoothInit = false
        fixTimeMs = 0L
        if (!parseRoutePayload(payload)) {
          Log.w(TAG, "start aborted: could not parse route payload")
          stopNavigation(sendRouteEnd = false)
          return START_NOT_STICKY
        }
        Log.i(TAG, "ride START — ${routeSteps.size} steps, ${routeCoords.size} route points, " +
          "${corridorRoads.size} corridor roads")
        reconnectAttempts = 0
        resetBleRouteState()
        currentArea = null
        lastNavKey = ""
        lastEmitAt = 0L
        // On a re-route the link is already up, so push the refreshed trip
        // summary now (new totals); on first start it flushes after discovery.
        if (servicesReady) routeStartFrame?.let { sendFrame(it) }
        refreshMapArea(fixLat, fixLng)
        // A passive autoConnect left over from the idle link can take a while;
        // swap it for a direct connect now that we're riding.
        if (bluetoothGatt != null && !servicesReady) disconnectBle()
        connectBle()
        registerPhoneStateReceiver()
        registerBtStateReceiver()
        moving = false
        gpsIntervalMs = 0L
        startLocationUpdates()
        refreshTheme()
        mainHandler.removeCallbacks(emitRunnable)
        mainHandler.postDelayed(emitRunnable, EMIT_TICK_MS)
        mainHandler.removeCallbacks(themeRunnable)
        mainHandler.postDelayed(themeRunnable, THEME_CHECK_MS)
      }

      ACTION_STOP_NAVIGATION -> stopNavigation(sendRouteEnd = true)

      ACTION_START_LINK -> startIdleLink()

      ACTION_STOP_LINK -> {
        linkActive = false
        if (!navActive) teardown()
      }

      ACTION_CRASH_OK -> {
        cancelNotification(CRASH_NOTIFICATION_ID)
        sendFrame(DeviceFrames.sosStatus("cancelled"))
        if (!linkWanted) stopSelf()
      }

      else -> {
        // Restarted by the system with no intent — we can't recover ride state.
        Log.w(TAG, "onStartCommand with no actionable intent — stopping")
        stopSelf()
      }
    }

    // Don't auto-restart without an intent: a restarted FGS that never calls
    // startForeground would be killed by the platform anyway.
    return START_NOT_STICKY
  }

  override fun onDestroy() {
    if (instance === this) instance = null
    DeviceStatus.connected = false
    DeviceStatus.mode = "off"
    mainHandler.removeCallbacksAndMessages(null)
    stopLocationUpdates()
    unregisterPhoneStateReceiver()
    unregisterBtStateReceiver()
    disconnectBle()
    super.onDestroy()
  }

  // Rides run as a location FGS; the idle link as a connectedDevice FGS.
  private fun startForegroundSafely(ride: Boolean): Boolean {
    return try {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        val type = if (ride) ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
          else ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        startForeground(NOTIFICATION_ID, buildNotification(ride), type)
      } else {
        startForeground(NOTIFICATION_ID, buildNotification(ride))
      }
      true
    } catch (e: Exception) {
      // e.g. SecurityException (missing location perm) or
      // ForegroundServiceStartNotAllowedException on Android 12+.
      Log.e(TAG, "startForeground failed: ${e.message}")
      false
    }
  }

  override fun onLocationChanged(location: Location) {
    if (!navActive || navArrived || routeSteps.isEmpty()) {
      return
    }

    while (navStepIdx < routeSteps.lastIndex) {
      val distToEnd = distanceTo(location, routeSteps[navStepIdx].endLocation)
      if (distToEnd < ADVANCE_THRESHOLD_METERS) {
        navStepIdx += 1
      } else {
        break
      }
    }

    val step = routeSteps.getOrNull(navStepIdx) ?: return
    val distToEnd = distanceTo(location, step.endLocation)
    val rawHeading = if (location.hasBearing()) location.bearing.toDouble() else targetHeading

    // Record this fix; the high-rate emitter (emitSmoothFrame) interpolates
    // between fixes so the display moves continuously rather than in 1 Hz jumps.
    val now = System.currentTimeMillis()
    prevFixLat = fixLat; prevFixLng = fixLng; prevFixTimeMs = fixTimeMs
    fixLat = location.latitude
    fixLng = location.longitude
    fixTimeMs = now
    fixSpeed = when {
      location.hasSpeed() -> location.speed.toDouble()
      prevFixTimeMs != 0L && now > prevFixTimeMs ->
        BleNavFrameBuilder.haversine(prevFixLat, prevFixLng, fixLat, fixLng) / ((now - prevFixTimeMs) / 1000.0)
      else -> fixSpeed
    }
    targetHeading = rawHeading
    if (!smoothInit) {
      smoothHeading = rawHeading
      emitLat = fixLat
      emitLng = fixLng
      smoothInit = true
    }

    updateMotionState()
    refreshMapArea(fixLat, fixLng)
    currentArea?.let { area ->
      currentStreet = area.streetAt(area.localXY(fixLat, fixLng)) ?: currentStreet
    }

    if (navStepIdx == routeSteps.lastIndex && distToEnd < ARRIVAL_THRESHOLD_METERS) {
      navArrived = true
      sendFrame(
        JSONObject()
          .put("type", "nav")
          .put("heading", smoothHeading.roundToInt())
          .put("distanceToTurn", 0)
          .put("instruction", "ARRIVE")
          .put("route", JSONArray().put(JSONArray().put(0).put(0)))
          .put("remainingDistance", 0)
          .put("remainingTime", 0)
          .put("eta", System.currentTimeMillis())
          .put("arrived", true)
          .toString()
      )
      stopNavigation(sendRouteEnd = false)
    }
  }

  // Emit one interpolated nav frame: dead-reckon position forward from the last
  // fix along the eased heading, and ease both heading and the position
  // correction so motion stays smooth between sparse GPS updates.
  private fun emitSmoothFrame() {
    if (!smoothInit || routeSteps.isEmpty()) return
    val step = routeSteps.getOrNull(navStepIdx) ?: return

    // Ease heading toward the latest bearing (shortest-arc).
    smoothHeading = easeAngle(smoothHeading, targetHeading, HEADING_EASE)

    // Predict where we are now from the last fix using speed + heading.
    val dt = ((System.currentTimeMillis() - fixTimeMs) / 1000.0).coerceAtLeast(0.0)
    val travel = (fixSpeed * dt).coerceAtMost(MAX_EXTRAPOLATION_M)
    val rad = Math.toRadians(smoothHeading)
    val predLat = fixLat + (travel * Math.cos(rad)) / METERS_PER_DEG
    val predLng = fixLng + (travel * Math.sin(rad)) / (METERS_PER_DEG * Math.cos(Math.toRadians(fixLat)))

    // Ease the emitted position toward the prediction so new fixes don't snap.
    emitLat += (predLat - emitLat) * POS_EASE
    emitLng += (predLng - emitLng) * POS_EASE

    // Adaptive rate: 4 Hz riding, 1 Hz slow, heartbeat when stopped, and
    // nothing at all if the frame wouldn't change.
    val now = SystemClock.elapsedRealtime()
    val interval = when {
      fixSpeed > MOVING_SPEED -> EMIT_MOVING_MS
      fixSpeed > STOPPED_SPEED -> EMIT_SLOW_MS
      else -> HEARTBEAT_MS
    }
    if (now - lastEmitAt < interval) return

    val distToEnd = BleNavFrameBuilder.haversine(emitLat, emitLng, step.endLocation.lat, step.endLocation.lng)
    val extras = navExtras(emitLat, emitLng)
    val key = "$navStepIdx|${smoothHeading.roundToInt()}|${distToEnd.roundToInt()}|" +
      "${extras.area}|${extras.ax}|${extras.ay}|${extras.speedKmh}|${extras.street}"
    if (key == lastNavKey && now - lastEmitAt < HEARTBEAT_MS) return
    lastNavKey = key
    lastEmitAt = now
    sendFrame(computeMiniMap(emitLat, emitLng, smoothHeading, step, distToEnd, extras))
  }

  private fun navExtras(lat: Double, lng: Double): BleNavFrameBuilder.NavExtras {
    val area = currentArea
    val p = area?.localXY(lat, lng)
    return BleNavFrameBuilder.NavExtras(
      area = area?.id,
      ax = p?.x?.roundToInt() ?: 0,
      ay = p?.y?.roundToInt() ?: 0,
      street = currentStreet,
      speedKmh = (fixSpeed * 3.6).roundToInt(),
    )
  }

  // Moving/stopped with hysteresis; drives GPS cadence and BLE priority.
  private fun updateMotionState() {
    val nowMoving = if (moving) fixSpeed >= STOPPED_SPEED else fixSpeed > MOVING_SPEED
    if (nowMoving == moving && gpsIntervalMs != 0L) return
    moving = nowMoving
    Log.d(TAG, "motion: ${if (moving) "riding" else "stopped"} (${"%.1f".format(fixSpeed)} m/s)")
    startLocationUpdates()
    applyConnectionPriority()
  }

  // Send a new map_area once the rider nears the edge of the current one.
  private fun refreshMapArea(lat: Double, lng: Double) {
    if (corridorRoads.isEmpty() || (lat == 0.0 && lng == 0.0)) return
    if (!MapAreaBuilder.needsNewArea(currentArea, lat, lng, AREA_EDGE_MARGIN_M)) return
    val anchor = if (currentArea == null) PiNavigationService.LatLng(lat, lng)
      else MapAreaBuilder.anchorAhead(lat, lng, targetHeading, AREA_AHEAD_M)
    areaSeq = areaSeq % 255 + 1
    val area = MapAreaBuilder.build(areaSeq, anchor.lat, anchor.lng, corridorRoads)
    currentArea = area
    Log.i(TAG, "map_area #${area.id}: ${area.roads.size} roads, ${area.frame.length} B")
    // Not buffered while the link is down: the current area is replayed on connect.
    sendFrame(area.frame, bufferIfDown = false)
  }

  // Corridor fetched by the WebView after the ride already started.
  fun updateCorridor(json: String) {
    mainHandler.post {
      if (!navActive) return@post
      corridorRoads = parseCorridorRoads(JSONArray(json))
      Log.i(TAG, "corridor updated: ${corridorRoads.size} roads")
      currentArea = null
      refreshMapArea(fixLat, fixLng)
    }
  }

  internal fun onSettingsChanged(next: DeviceSettings) {
    mainHandler.post {
      settings = next
      refreshTheme()
    }
  }

  private fun refreshTheme() {
    val loc = themeLocation()
    val frame = DeviceFrames.theme(settings, loc?.first, loc?.second, System.currentTimeMillis())
    if (frame == themeFrame) return
    themeFrame = frame
    DeviceStatus.theme = JSONObject(frame).optString("mode")
    sendFrame(frame, bufferIfDown = false)
  }

  @SuppressLint("MissingPermission")
  private fun themeLocation(): Pair<Double, Double>? {
    if (fixTimeMs != 0L && !(fixLat == 0.0 && fixLng == 0.0)) return fixLat to fixLng
    if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
      return null
    }
    val manager = locationManager ?: return null
    return listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER, LocationManager.PASSIVE_PROVIDER)
      .firstNotNullOfOrNull { runCatching { manager.getLastKnownLocation(it) }.getOrNull() }
      ?.let { it.latitude to it.longitude }
  }

  // Ease angle a → b along the shortest arc; alpha in 0..1.
  private fun easeAngle(a: Double, b: Double, alpha: Double): Double {
    var diff = (b - a) % 360.0
    if (diff < -180.0) diff += 360.0
    if (diff > 180.0) diff -= 360.0
    var r = a + diff * alpha
    if (r < 0) r += 360.0
    if (r >= 360.0) r -= 360.0
    return r
  }

  override fun onProviderEnabled(provider: String) = Unit

  @Deprecated("Deprecated in Java")
  override fun onStatusChanged(provider: String?, status: Int, extras: android.os.Bundle?) = Unit

  override fun onProviderDisabled(provider: String) = Unit

  // ── Minimal-map computation (heading-up local frame, metres) ──
  private fun computeMiniMap(
    lat: Double,
    lng: Double,
    heading: Double,
    step: RouteStep,
    distToEnd: Double,
    extras: BleNavFrameBuilder.NavExtras,
  ): String {
    val remainingStepDistances = routeSteps.drop(navStepIdx + 1).map { it.distanceM }
    val remainingStepDurations = routeSteps.drop(navStepIdx + 1).map { it.durationS }
    return BleNavFrameBuilder.buildNavFrame(
      lat = lat,
      lng = lng,
      heading = heading,
      distToEnd = distToEnd,
      step = step,
      navStepIdx = navStepIdx,
      routeCoords = routeCoords,
      remainingStepDistances = remainingStepDistances,
      remainingStepDurations = remainingStepDurations,
      lookAheadMeters = LOOK_AHEAD_METERS,
      maxRoutePoints = MAX_ROUTE_POINTS,
      maxJunctionRoadPoints = MAX_JUNCTION_ROAD_POINTS,
      nowMillis = System.currentTimeMillis(),
      extras = extras,
    )
  }

  private fun parseRoutePayload(payload: String): Boolean {
    return try {
      val json = JSONObject(payload)

      val steps = json.optJSONArray("steps") ?: JSONArray()
      routeSteps = buildList {
        for (idx in 0 until steps.length()) {
          val step = steps.optJSONObject(idx) ?: continue
          val endLocation = step.optJSONObject("end_location") ?: continue
          add(
            RouteStep(
              instructions = step.optString("instructions", "Continue"),
              maneuver = step.optString("maneuver", ""),
              endLocation = LatLng(
                lat = endLocation.optDouble("lat", 0.0),
                lng = endLocation.optDouble("lng", 0.0),
              ),
              distanceM = step.optDouble("distance", 0.0),
              durationS = step.optDouble("duration", 0.0),
              junctionRoads = parseJunctionRoads(step.optJSONObject("junction_roads")),
            )
          )
        }
      }

      // routeCoords is sent from JS as [[lat, lng], ...]
      val coords = json.optJSONArray("routeCoords") ?: JSONArray()
      routeCoords = buildList {
        for (idx in 0 until coords.length()) {
          val pair = coords.optJSONArray(idx) ?: continue
          if (pair.length() < 2) continue
          add(LatLng(lat = pair.optDouble(0, 0.0), lng = pair.optDouble(1, 0.0)))
        }
      }

      // Static trip summary the ESP32 shows at ride start.
      routeStartFrame = JSONObject()
        .put("type", "route_start")
        .put("destination", json.optString("destinationName", "Destination"))
        .put("totalDistance", json.optDouble("totalDistance", 0.0).roundToInt())
        .put("totalDuration", json.optDouble("totalDuration", 0.0).roundToInt())
        .toString()

      corridorRoads = parseCorridorRoads(json.optJSONArray("corridorRoads"))

      seedFromRouteOrigin(json.optJSONObject("origin"))

      routeSteps.isNotEmpty()
    } catch (e: Exception) {
      Log.e(TAG, "parseRoutePayload failed: ${e.message}")
      routeSteps = emptyList()
      routeCoords = emptyList()
      false
    }
  }

  // [{w: 1|2, n: "name", c: [[lat, lng], ...]}, ...] from the JS corridor fetch.
  private fun parseCorridorRoads(arr: JSONArray?): List<CorridorRoad> {
    if (arr == null) return emptyList()
    return buildList {
      for (i in 0 until arr.length()) {
        val road = arr.optJSONObject(i) ?: continue
        val c = road.optJSONArray("c") ?: continue
        val coords = buildList {
          for (j in 0 until c.length()) {
            val pair = c.optJSONArray(j) ?: continue
            if (pair.length() < 2) continue
            add(LatLng(pair.optDouble(0, 0.0), pair.optDouble(1, 0.0)))
          }
        }
        if (coords.size < 2) continue
        add(CorridorRoad(major = road.optInt("w", 1) >= 2, name = road.optString("n", ""), coords = coords))
      }
    }
  }

  private fun startLocationUpdates() {
    if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED &&
      ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
      Log.w(TAG, "startLocationUpdates: location permission not granted")
      return
    }

    val manager = locationManager ?: return
    val interval = if (moving) GPS_RIDING_MS else GPS_STOPPED_MS
    if (interval == gpsIntervalMs) return
    gpsIntervalMs = interval
    runCatching {
      manager.removeUpdates(this)
      if (manager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
        manager.requestLocationUpdates(LocationManager.GPS_PROVIDER, interval, 1f, this, Looper.getMainLooper())
      }
      if (manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
        manager.requestLocationUpdates(LocationManager.NETWORK_PROVIDER, interval + 500L, 1f, this, Looper.getMainLooper())
      }
    }.onFailure { Log.w(TAG, "requestLocationUpdates failed: ${it.message}") }
  }

  private fun parseJunctionRoads(json: JSONObject?): JunctionRoads? {
    if (json == null) return null

    return runCatching {
      val roadsJson = json.optJSONArray("roads") ?: return null
      val roads = buildList {
        for (idx in 0 until roadsJson.length()) {
          val road = roadsJson.optJSONObject(idx) ?: continue
          val coordsJson = road.optJSONArray("coords") ?: continue
          val coords = buildList {
            for (coordIdx in 0 until coordsJson.length()) {
              val pair = coordsJson.optJSONArray(coordIdx) ?: continue
              if (pair.length() < 2) continue
              add(LatLng(lat = pair.optDouble(0, 0.0), lng = pair.optDouble(1, 0.0)))
            }
          }
          if (coords.size < 2) continue

          add(
            JunctionRoad(
              id = road.optString("id", "road-$idx"),
              name = road.optString("name", "Unnamed road"),
              highway = road.optString("highway", ""),
              coords = coords,
            )
          )
        }
      }

      if (roads.isEmpty()) return null
      JunctionRoads(
        highlightedBranchId = json.optString("highlighted_branch_id").ifBlank { null },
        roads = roads,
      )
    }.getOrElse {
      Log.w(TAG, "parseJunctionRoads: ignoring malformed payload")
      null
    }
  }

  private fun stopLocationUpdates() {
    gpsIntervalMs = 0L
    runCatching { locationManager?.removeUpdates(this) }
  }

  private fun seedFromRouteOrigin(origin: JSONObject?) {
    if (origin == null) return

    val lat = origin.optDouble("lat", Double.NaN)
    val lng = origin.optDouble("lng", Double.NaN)
    if (lat.isNaN() || lng.isNaN()) return

    prevFixLat = lat
    prevFixLng = lng
    prevFixTimeMs = 0L
    fixLat = lat
    fixLng = lng
    fixTimeMs = System.currentTimeMillis()
    fixSpeed = 0.0
    emitLat = lat
    emitLng = lng
    smoothInit = true
  }

  private fun resetBleRouteState() {
    synchronized(writeQueue) {
      writeQueue.clear()
      pendingFrames.clear()
      writing = false
      writeSeq += 1
    }
  }

  private fun stopNavigation(sendRouteEnd: Boolean) {
    if (sendRouteEnd) {
      sendFrame(JSONObject().put("type", "route_end").toString())
    }
    navActive = false
    navArrived = false
    navStepIdx = 0
    smoothInit = false
    mainHandler.removeCallbacks(emitRunnable)
    routeSteps = emptyList()
    routeCoords = emptyList()
    routeStartFrame = null
    corridorRoads = emptyList()
    currentArea = null
    currentStreet = ""
    stopLocationUpdates()

    // Park guard keeps the device link up after the ride.
    if (settings.parkGuard || linkActive) {
      startIdleLink()
      return
    }
    // Give the route_end frame a moment to flush over BLE before tearing down.
    mainHandler.postDelayed({ if (!linkWanted) teardown() }, 500L)
  }

  // Low-power link between rides: battery, calls, crash and park-guard events.
  // Reconnects with autoConnect so the radio waits passively for the device
  // to wake from deep sleep instead of polling.
  private fun startIdleLink() {
    if (!hasBtPermission()) {
      Log.w(TAG, "idle link needs BLUETOOTH_CONNECT — not starting")
      if (!navActive) teardown()
      return
    }
    linkActive = true
    if (!navActive) {
      if (!startForegroundSafely(ride = false)) {
        linkActive = false
        teardown()
        return
      }
      DeviceStatus.mode = "idle"
    }
    registerPhoneStateReceiver()
    registerBtStateReceiver()
    connectBle()
    applyConnectionPriority()
    refreshTheme()
    mainHandler.removeCallbacks(themeRunnable)
    mainHandler.postDelayed(themeRunnable, THEME_CHECK_MS)
  }

  private fun teardown() {
    mainHandler.removeCallbacks(themeRunnable)
    unregisterPhoneStateReceiver()
    unregisterBtStateReceiver()
    runCatching {
      disconnectBle()
      DeviceStatus.mode = "off"
      stopForeground(STOP_FOREGROUND_REMOVE)
      stopSelf()
    }
  }

  // ── BLE: connect to the bonded ESP32 and stream JSON frames ──
  private fun hasBtPermission(): Boolean {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true
    return ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
  }

  @SuppressLint("MissingPermission")
  private fun connectBle() {
    if (!linkWanted) return
    if (bluetoothGatt != null) {
      Log.d(TAG, "connectBle: already connecting/connected")
      return
    }
    if (!hasBtPermission()) {
      // Keep retrying: the user may grant "Nearby devices" mid-ride from the
      // permission dialog or Settings, and we want to connect as soon as they do.
      Log.w(TAG, "connectBle: BLUETOOTH_CONNECT (Nearby devices) permission not granted — will retry")
      scheduleReconnect()
      return
    }

    val manager = getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager ?: run {
      Log.w(TAG, "connectBle: no BluetoothManager")
      return
    }
    val adapter: BluetoothAdapter = manager.adapter ?: run {
      Log.w(TAG, "connectBle: no Bluetooth adapter")
      return
    }
    if (!adapter.isEnabled) {
      Log.w(TAG, "connectBle: Bluetooth is OFF — will connect when it's turned on")
      return
    }

    val bonded = try { adapter.bondedDevices ?: emptySet() } catch (e: Exception) { emptySet() }
    Log.d(TAG, "connectBle: bonded devices = ${bonded.joinToString { (it.name ?: "?") + "/" + it.address }}")
    if (bonded.isEmpty()) {
      Log.w(TAG, "connectBle: no bonded devices — pair the ESP32 in Android settings first")
      scheduleReconnect()
      return
    }

    val target: BluetoothDevice =
      bonded.firstOrNull { TARGET_NAME_SUBSTR.isNotEmpty() && (it.name ?: "").contains(TARGET_NAME_SUBSTR, ignoreCase = true) }
        ?: bonded.first()

    // Riding: autoConnect=false → fast direct connect, we retry ourselves.
    // Idle: autoConnect=true → the controller waits for the device to advertise.
    val autoConnect = !navActive
    Log.i(TAG, "connectBle: connecting to ${target.name ?: "?"} (${target.address}), autoConnect=$autoConnect")
    DeviceStatus.deviceName = target.name
    rxChar = null
    discoveredRxChar = null
    servicesReady = false
    bluetoothGatt = try {
      target.connectGatt(this, autoConnect, gattCallback, BluetoothDevice.TRANSPORT_LE)
    } catch (e: Exception) {
      Log.e(TAG, "connectGatt threw: ${e.message}")
      null
    }
    if (bluetoothGatt == null) scheduleReconnect()
  }

  @SuppressLint("MissingPermission")
  private fun disconnectBle() {
    synchronized(writeQueue) {
      writeQueue.clear()
      pendingFrames.clear()
      writing = false
    }
    rxChar = null
    discoveredRxChar = null
    servicesReady = false
    DeviceStatus.connected = false
    val gatt = bluetoothGatt
    bluetoothGatt = null
    if (gatt != null && hasBtPermission()) {
      runCatching { gatt.disconnect() }
      runCatching { gatt.close() }
    }
  }

  private fun scheduleReconnect() {
    if (!linkWanted) return
    reconnectAttempts++
    val delay = (RECONNECT_BASE_DELAY_MS * reconnectAttempts).coerceAtMost(RECONNECT_MAX_DELAY_MS)
    Log.d(TAG, "scheduling reconnect #$reconnectAttempts in ${delay}ms")
    mainHandler.postDelayed({ if (linkWanted && bluetoothGatt == null) connectBle() }, delay)
  }

  // BALANCED while riding, LOW_POWER when stopped or idle.
  @SuppressLint("MissingPermission")
  private fun applyConnectionPriority() {
    val gatt = bluetoothGatt ?: return
    if (!servicesReady || !hasBtPermission()) return
    val priority = if (navActive && moving) BluetoothGatt.CONNECTION_PRIORITY_BALANCED
      else BluetoothGatt.CONNECTION_PRIORITY_LOW_POWER
    runCatching { gatt.requestConnectionPriority(priority) }
  }

  private val gattCallback = object : BluetoothGattCallback() {
    @SuppressLint("MissingPermission")
    override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
      // Ignore callbacks from a stale GATT instance (e.g. after a reconnect).
      if (gatt !== bluetoothGatt) {
        runCatching { gatt.close() }
        return
      }

      if (status != BluetoothGatt.GATT_SUCCESS) {
        // Connection error (133 is the classic Android BLE failure) — recycle and retry.
        Log.w(TAG, "GATT error status=$status, newState=$newState — closing & retrying")
        rxChar = null
        servicesReady = false
        DeviceStatus.connected = false
        runCatching { gatt.close() }
        bluetoothGatt = null
        scheduleReconnect()
        return
      }

      when (newState) {
        BluetoothProfile.STATE_CONNECTED -> {
          reconnectAttempts = 0
          Log.i(TAG, "GATT connected — requesting MTU $REQUESTED_MTU")
          if (hasBtPermission()) gatt.requestMtu(REQUESTED_MTU)
          // Fallback: if onMtuChanged never arrives, discover services anyway.
          mainHandler.postDelayed({
            if (gatt === bluetoothGatt && !servicesReady && hasBtPermission()) {
              Log.w(TAG, "MTU callback timeout — discovering services with default MTU")
              runCatching { gatt.discoverServices() }
            }
          }, MTU_FALLBACK_DELAY_MS)
        }
        BluetoothProfile.STATE_DISCONNECTED -> {
          Log.w(TAG, "GATT disconnected, navActive=$navActive, linkActive=$linkActive")
          rxChar = null
          servicesReady = false
          DeviceStatus.connected = false
          runCatching { gatt.close() }
          bluetoothGatt = null
          scheduleReconnect()
        }
      }
    }

    @SuppressLint("MissingPermission")
    override fun onMtuChanged(gatt: BluetoothGatt, mtuValue: Int, status: Int) {
      if (gatt !== bluetoothGatt) return
      mtu = if (status == BluetoothGatt.GATT_SUCCESS) mtuValue else 23
      Log.i(TAG, "MTU = $mtu (status=$status) — discovering services")
      if (!servicesReady && hasBtPermission()) runCatching { gatt.discoverServices() }
    }

    override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
      if (gatt !== bluetoothGatt) return
      if (status != BluetoothGatt.GATT_SUCCESS) {
        Log.w(TAG, "onServicesDiscovered failed (status=$status)")
        return
      }
      val service = gatt.getService(NUS_SERVICE)
      val rx = service?.getCharacteristic(NUS_RX_CHAR)
      if (rx == null) {
        Log.w(TAG, "NUS service/RX char NOT found (service=${service != null}). " +
          "Check the ESP32 advertises NUS service $NUS_SERVICE with RX char $NUS_RX_CHAR")
        return
      }
      discoveredRxChar = rx

      // Subscribe to device → phone frames first: GATT allows one operation
      // at a time, so streaming starts once the CCCD write completes.
      val tx = service.getCharacteristic(NUS_TX_CHAR)
      if (tx != null && tx.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0 && enableNotifications(gatt, tx)) {
        mainHandler.postDelayed({ onLinkReady(gatt) }, MTU_FALLBACK_DELAY_MS)
      } else {
        Log.i(TAG, "no NUS TX notify characteristic — one-way link")
        onLinkReady(gatt)
      }
    }

    override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
      if (gatt !== bluetoothGatt || descriptor.uuid != CCCD) return
      Log.i(TAG, "TX notify subscription status=$status")
      mainHandler.post { onLinkReady(gatt) }
    }

    override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
      if (gatt !== bluetoothGatt || characteristic.uuid != NUS_TX_CHAR) return
      mainHandler.post { deviceReader.feed(value).forEach { handleDeviceFrame(it) } }
    }

    @Deprecated("Deprecated in Java")
    override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
      // Android 13+ also delivers the overload above; only handle older releases here.
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) return
      @Suppress("DEPRECATION")
      val value = characteristic.value ?: return
      onCharacteristicChanged(gatt, characteristic, value.copyOf())
    }

    override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
      if (gatt !== bluetoothGatt) return
      if (status != BluetoothGatt.GATT_SUCCESS) Log.w(TAG, "onCharacteristicWrite status=$status")
      synchronized(writeQueue) { writing = false }
      processWriteQueue()
    }
  }

  @SuppressLint("MissingPermission")
  private fun enableNotifications(gatt: BluetoothGatt, tx: BluetoothGattCharacteristic): Boolean {
    if (!hasBtPermission()) return false
    val cccd = tx.getDescriptor(CCCD) ?: return false
    return runCatching {
      gatt.setCharacteristicNotification(tx, true)
      val value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        gatt.writeDescriptor(cccd, value) == BluetoothGatt.GATT_SUCCESS
      } else {
        @Suppress("DEPRECATION")
        run {
          cccd.value = value
          gatt.writeDescriptor(cccd)
        }
      }
    }.getOrDefault(false)
  }

  // Link is usable: replay state so the screen recovers in under a second —
  // trip summary, theme, current map area, then the latest buffered nav frames.
  private fun onLinkReady(gatt: BluetoothGatt) {
    if (gatt !== bluetoothGatt || servicesReady) return
    rxChar = discoveredRxChar ?: return
    servicesReady = true
    DeviceStatus.connected = true
    Log.i(TAG, "link ready (mode=${DeviceStatus.mode}) — streaming will begin")
    applyConnectionPriority()
    if (navActive) routeStartFrame?.let { enqueueChunks(it) }
    themeFrame?.let { enqueueChunks(it) }
    if (navActive) currentArea?.let { enqueueChunks(it.frame) }
    flushPendingFrames()
  }

  // ── Device → phone frames (protocol v2) ──
  private fun handleDeviceFrame(msg: JSONObject) {
    Log.d(TAG, "device -> $msg")
    when (msg.optString("type")) {
      "battery" -> {
        val percent = msg.optInt("percent", -1).takeIf { it in 0..100 } ?: return
        DeviceStatus.battery = percent
        DeviceStatus.charging = msg.optBoolean("charging", false)
        DeviceStatus.batteryMv = msg.optInt("mv", 0).takeIf { it > 0 }
        DeviceStatus.batteryAt = System.currentTimeMillis()
        if (DeviceStatus.charging || percent > LOW_BATTERY_PERCENT + 5) {
          lowBatteryWarned = false
        } else if (percent <= LOW_BATTERY_PERCENT && !lowBatteryWarned) {
          lowBatteryWarned = true
          postAlert(BATTERY_NOTIFICATION_ID, getString(R.string.device_low_battery_title),
            getString(R.string.device_low_battery_text, percent), urgent = false)
        }
      }
      "imu_event" -> {
        val peak = msg.optDouble("peakG", 0.0)
        when (msg.optString("event")) {
          "crash" -> if (settings.crashAlerts) showCrashAlert(peak)
          "park_motion" -> if (settings.parkGuard) showParkAlert(peak)
        }
      }
      "call_action" -> handleCallAction(msg.optString("action"))
      "sos_cancel" -> cancelNotification(CRASH_NOTIFICATION_ID)
    }
  }

  @SuppressLint("MissingPermission")
  private fun handleCallAction(action: String) {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O ||
      ContextCompat.checkSelfPermission(this, Manifest.permission.ANSWER_PHONE_CALLS) != PackageManager.PERMISSION_GRANTED) {
      Log.w(TAG, "call_action '$action' ignored: ANSWER_PHONE_CALLS not granted")
      return
    }
    val telecom = getSystemService(Context.TELECOM_SERVICE) as? TelecomManager ?: return
    runCatching {
      when (action) {
        "accept" -> telecom.acceptRingingCall()
        "decline" -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
          @Suppress("DEPRECATION")
          telecom.endCall()
        }
      }
    }.onFailure { Log.w(TAG, "call_action '$action' failed: ${it.message}") }
  }

  private fun showCrashAlert(peakG: Double) {
    val okIntent = PendingIntent.getService(
      this, 0,
      Intent(this, PiNavigationService::class.java).setAction(ACTION_CRASH_OK),
      PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )
    postAlert(
      CRASH_NOTIFICATION_ID,
      getString(R.string.device_crash_title),
      getString(R.string.device_crash_text, peakG),
      urgent = true,
    ) { it.addAction(0, getString(R.string.device_crash_ok), okIntent) }
  }

  private fun showParkAlert(peakG: Double) {
    val now = System.currentTimeMillis()
    if (now - lastParkAlertAt < PARK_ALERT_DEDUPE_MS) return
    lastParkAlertAt = now
    postAlert(PARK_NOTIFICATION_ID, getString(R.string.device_park_title),
      getString(R.string.device_park_text, peakG), urgent = true)
  }

  private fun postAlert(
    id: Int,
    title: String,
    text: String,
    urgent: Boolean,
    extra: (NotificationCompat.Builder) -> Unit = {},
  ) {
    val manager = getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return
    val builder = NotificationCompat.Builder(this, ALERT_CHANNEL_ID)
      .setSmallIcon(R.mipmap.ic_launcher)
      .setContentTitle(title)
      .setContentText(text)
      .setStyle(NotificationCompat.BigTextStyle().bigText(text))
      .setPriority(if (urgent) NotificationCompat.PRIORITY_MAX else NotificationCompat.PRIORITY_DEFAULT)
      .setCategory(if (urgent) NotificationCompat.CATEGORY_ALARM else NotificationCompat.CATEGORY_STATUS)
      .setAutoCancel(true)
      .setContentIntent(launchAppIntent())
    extra(builder)
    runCatching { manager.notify(id, builder.build()) }
  }

  private fun cancelNotification(id: Int) {
    (getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager)?.cancel(id)
  }

  private fun launchAppIntent(): PendingIntent = PendingIntent.getActivity(
    this, 1,
    Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
    PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
  )

  // Queue a JSON frame; chunk it to the negotiated MTU and stream it out.
  // Frames that are replayed on (re)connect skip the pending buffer.
  private fun sendFrame(json: String, bufferIfDown: Boolean = true) {
    if (rxChar == null) {
      if (!bufferIfDown) return
      Log.d(TAG, "frame buffered (link not ready): $json")
      synchronized(writeQueue) {
        // Keep only the most recent few frames if the link isn't ready yet.
        while (pendingFrames.size >= 5) pendingFrames.pollFirst()
        pendingFrames.addLast(json)
      }
      return
    }
    Log.d(TAG, "frame -> $json")
    enqueueChunks(json)
  }

  private fun flushPendingFrames() {
    val frames: List<String>
    synchronized(writeQueue) {
      frames = pendingFrames.toList()
      pendingFrames.clear()
    }
    frames.forEach { enqueueChunks(it) }
  }

  private fun enqueueChunks(json: String) {
    val bytes = (json + "\n").toByteArray(Charsets.UTF_8)
    val chunkSize = (mtu - 3).coerceAtLeast(20)
    synchronized(writeQueue) {
      var offset = 0
      while (offset < bytes.size) {
        val end = (offset + chunkSize).coerceAtMost(bytes.size)
        writeQueue.addLast(bytes.copyOfRange(offset, end))
        offset = end
      }
      // Cap backlog so a stalled link can't grow the queue without bound.
      while (writeQueue.size > 200) writeQueue.pollFirst()
    }
    processWriteQueue()
  }

  @SuppressLint("MissingPermission")
  private fun processWriteQueue() {
    val gatt = bluetoothGatt ?: return
    val characteristic = rxChar ?: return
    if (!hasBtPermission()) return

    val chunk: ByteArray
    val seq: Long
    synchronized(writeQueue) {
      if (writing) return
      chunk = writeQueue.pollFirst() ?: return
      writing = true
      seq = ++writeSeq
    }

    // Match the characteristic's declared capability: prefer no-response (faster)
    // only when the peripheral supports it, else fall back to write-with-response.
    val writeType = if (characteristic.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0) {
      BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
    } else {
      BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
    }
    val ok = try {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        gatt.writeCharacteristic(characteristic, chunk, writeType) == BluetoothGatt.GATT_SUCCESS
      } else {
        @Suppress("DEPRECATION")
        run {
          characteristic.writeType = writeType
          characteristic.value = chunk
          gatt.writeCharacteristic(characteristic)
        }
      }
    } catch (e: Exception) {
      Log.w(TAG, "writeCharacteristic threw: ${e.message}")
      false
    }

    if (!ok) {
      // Write rejected (e.g. busy) — requeue the chunk and retry shortly.
      Log.w(TAG, "writeCharacteristic rejected — requeueing chunk (${chunk.size}B), retrying")
      synchronized(writeQueue) {
        writeQueue.addFirst(chunk)
        writing = false
      }
      mainHandler.postDelayed({ processWriteQueue() }, 50L)
      return
    }

    // Watchdog: if the write callback is lost, don't stall the stream forever.
    mainHandler.postDelayed({
      val stuck = synchronized(writeQueue) { if (writing && writeSeq == seq) { writing = false; true } else false }
      if (stuck) {
        Log.w(TAG, "write callback timeout (seq=$seq) — resuming queue")
        processWriteQueue()
      }
    }, WRITE_TIMEOUT_MS)
  }

  private fun registerPhoneStateReceiver() {
    if (phoneStateReceiver != null) return

    phoneStateReceiver = object : BroadcastReceiver() {
      override fun onReceive(context: Context?, intent: Intent?) {
        if (intent?.action != TelephonyManager.ACTION_PHONE_STATE_CHANGED) return
        val state = intent.getStringExtra(TelephonyManager.EXTRA_STATE)
        val previous = lastPhoneState
        lastPhoneState = state
        when (state) {
          TelephonyManager.EXTRA_STATE_RINGING -> {
            @Suppress("DEPRECATION")
            val number = intent.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER)?.trim().orEmpty()
            val caller = resolveCallerName(number) ?: if (number.isNotBlank()) number else "Incoming call"
            maybeSendIncomingCall(caller, number)
          }
          // The broadcast repeats per state; only forward real transitions.
          TelephonyManager.EXTRA_STATE_OFFHOOK -> if (previous == TelephonyManager.EXTRA_STATE_RINGING) {
            sendFrame(DeviceFrames.callState(active = true))
          }
          TelephonyManager.EXTRA_STATE_IDLE -> if (previous != null && previous != TelephonyManager.EXTRA_STATE_IDLE) {
            sendFrame(DeviceFrames.callState(active = false))
            // v1 frame: the CrowPanel clears its call overlay on this.
            sendFrame(JSONObject().put("type", "call_end").toString())
          }
        }
      }
    }

    val filter = IntentFilter(TelephonyManager.ACTION_PHONE_STATE_CHANGED)
    runCatching {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        registerReceiver(phoneStateReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
      } else {
        registerReceiver(phoneStateReceiver, filter)
      }
    }
  }

  private fun unregisterPhoneStateReceiver() {
    phoneStateReceiver?.let { runCatching { unregisterReceiver(it) } }
    phoneStateReceiver = null
  }

  // Reconnect automatically if the user toggles Bluetooth back on mid-ride.
  private fun registerBtStateReceiver() {
    if (btStateReceiver != null) return
    btStateReceiver = object : BroadcastReceiver() {
      override fun onReceive(context: Context?, intent: Intent?) {
        if (intent?.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
        val state = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)
        if (state == BluetoothAdapter.STATE_ON && linkWanted && bluetoothGatt == null) {
          Log.i(TAG, "Bluetooth turned ON — attempting connect")
          reconnectAttempts = 0
          connectBle()
        }
      }
    }
    runCatching { registerReceiver(btStateReceiver, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)) }
  }

  private fun unregisterBtStateReceiver() {
    btStateReceiver?.let { runCatching { unregisterReceiver(it) } }
    btStateReceiver = null
  }

  private fun maybeSendIncomingCall(caller: String, number: String) {
    val signature = if (number.isNotBlank()) number else caller
    val now = System.currentTimeMillis()
    if (signature == lastCallSignature && now - lastCallAt < CALL_DEDUPE_MS) {
      return
    }
    lastCallSignature = signature
    lastCallAt = now

    sendFrame(
      JSONObject()
        .put("type", "incoming_call")
        .put("caller", caller)
        .put("number", number)
        .put("call_id", signature)
        .toString()
    )
  }

  private fun resolveCallerName(number: String): String? {
    if (number.isBlank()) return null
    if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED) {
      return null
    }

    return runCatching {
      val uri = Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(number))
      contentResolver.query(uri, arrayOf(ContactsContract.PhoneLookup.DISPLAY_NAME), null, null, null)?.use { cursor ->
        if (cursor.moveToFirst()) cursor.getString(0) else null
      }
    }.getOrNull()
  }

  private fun ensureNotificationChannel() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
    val manager = getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return
    val channel = NotificationChannel(
      NOTIFICATION_CHANNEL_ID,
      getString(R.string.tracking_notification_channel_name),
      NotificationManager.IMPORTANCE_LOW,
    )
    manager.createNotificationChannel(channel)
    manager.createNotificationChannel(
      NotificationChannel(ALERT_CHANNEL_ID, getString(R.string.device_alert_channel_name), NotificationManager.IMPORTANCE_HIGH)
    )
  }

  private fun buildNotification(ride: Boolean) = NotificationCompat.Builder(this, NOTIFICATION_CHANNEL_ID)
    .setSmallIcon(R.mipmap.ic_launcher)
    .setContentTitle(getString(if (ride) R.string.tracking_notification_title else R.string.link_notification_title))
    .setContentText(getString(if (ride) R.string.tracking_notification_text else R.string.link_notification_text))
    .setOngoing(true)
    .setSilent(true)
    .build()

  private fun distanceTo(location: Location, end: LatLng): Double {
    return BleNavFrameBuilder.haversine(location.latitude, location.longitude, end.lat, end.lng)
  }

  data class LatLng(val lat: Double, val lng: Double)
  data class JunctionRoad(val id: String, val name: String, val highway: String, val coords: List<LatLng>)
  data class JunctionRoads(val highlightedBranchId: String?, val roads: List<JunctionRoad>)
  data class RouteStep(
    val instructions: String,
    val maneuver: String,
    val endLocation: LatLng,
    val distanceM: Double,
    val durationS: Double,
    val junctionRoads: JunctionRoads?,
  )
}

internal object BleNavFrameBuilder {
  // Protocol v2 additions to `nav`: rider position in the current map_area
  // (north-up metres), the street being ridden, and speed.
  data class NavExtras(val area: Int?, val ax: Int, val ay: Int, val street: String, val speedKmh: Int)

  fun buildNavFrame(
    lat: Double,
    lng: Double,
    heading: Double,
    distToEnd: Double,
    step: PiNavigationService.RouteStep,
    navStepIdx: Int,
    routeCoords: List<PiNavigationService.LatLng>,
    remainingStepDistances: List<Double>,
    remainingStepDurations: List<Double>,
    lookAheadMeters: Double,
    maxRoutePoints: Int,
    maxJunctionRoadPoints: Int,
    nowMillis: Long,
    extras: NavExtras? = null,
  ): String {
    val hRad = Math.toRadians(heading)
    val route = JSONArray().put(JSONArray().put(0).put(0))

    if (routeCoords.isNotEmpty()) {
      var nearestIdx = 0
      var nearestDist = Double.MAX_VALUE
      for (i in routeCoords.indices) {
        val d = haversine(lat, lng, routeCoords[i].lat, routeCoords[i].lng)
        if (d < nearestDist) {
          nearestDist = d
          nearestIdx = i
        }
      }

      var acc = 0.0
      var prevLat = lat
      var prevLng = lng
      var i = nearestIdx + 1
      while (i < routeCoords.size && route.length() < maxRoutePoints) {
        val v = routeCoords[i]
        acc += haversine(prevLat, prevLng, v.lat, v.lng)
        route.put(toLocalXY(v.lat, v.lng, lat, lng, hRad))
        prevLat = v.lat
        prevLng = v.lng
        if (acc >= lookAheadMeters) break
        i++
      }
    }

    var remDist = distToEnd
    for (distance in remainingStepDistances) remDist += distance
    val curFrac = if (step.distanceM > 0) (distToEnd / step.distanceM).coerceIn(0.0, 1.0) else 0.0
    var remTime = step.durationS * curFrac
    for (duration in remainingStepDurations) remTime += duration

    val frame = JSONObject()
      .put("type", "nav")
      .put("heading", heading.roundToInt())
      .put("distanceToTurn", distToEnd.roundToInt())
      .put("instruction", simplifyInstruction(step.maneuver, step.instructions))
      .put("route", route)
      .put("remainingDistance", remDist.roundToInt())
      .put("remainingTime", remTime.roundToInt())
      .put("eta", nowMillis + (remTime * 1000).toLong())
      .put("junctionRoads", buildLocalJunctionRoads(lat, lng, hRad, step, maxJunctionRoadPoints))
      .put("arrived", false)
    if (extras != null) {
      if (extras.area != null) {
        frame.put("area", extras.area).put("ax", extras.ax).put("ay", extras.ay)
      }
      if (extras.street.isNotBlank()) frame.put("street", extras.street)
      frame.put("speed", extras.speedKmh)
    }
    return frame.toString()
  }

  // Pure great-circle distance (m) so the builder runs in JVM unit tests.
  fun haversine(lat1: Double, lng1: Double, lat2: Double, lng2: Double): Double {
    val dLat = Math.toRadians(lat2 - lat1)
    val dLng = Math.toRadians(lng2 - lng1)
    val a = Math.sin(dLat / 2).let { it * it } +
      Math.cos(Math.toRadians(lat1)) * Math.cos(Math.toRadians(lat2)) * Math.sin(dLng / 2).let { it * it }
    return 2 * 6_371_008.8 * Math.asin(Math.sqrt(a.coerceIn(0.0, 1.0)))
  }

  private fun buildLocalJunctionRoads(
    lat: Double,
    lng: Double,
    headingRad: Double,
    step: PiNavigationService.RouteStep,
    maxJunctionRoadPoints: Int,
  ): JSONArray {
    val junctionRoads = step.junctionRoads ?: return JSONArray()
    val roads = JSONArray()

    for (road in junctionRoads.roads) {
      if (road.coords.size < 2) continue

      val coords = JSONArray()
      for (coord in road.coords.take(maxJunctionRoadPoints)) {
        coords.put(toLocalXY(coord.lat, coord.lng, lat, lng, headingRad))
      }
      if (coords.length() < 2) continue

      roads.put(
        JSONObject()
          .put("highlighted", road.id == junctionRoads.highlightedBranchId)
          .put("coords", coords)
      )
    }

    return roads
  }

  // Heading-up (track-up) local frame in metres relative to the rider:
  // x = right, y = forward (up on screen). Points are rotated by the rider's
  // heading so the direction of travel is always up — the display keeps the
  // position arrow fixed pointing up and the map turns on bends.
  private fun toLocalXY(lat: Double, lng: Double, lat0: Double, lng0: Double, headingRad: Double): JSONArray {
    val dNorth = (lat - lat0) * 111320.0
    val dEast = (lng - lng0) * 111320.0 * cos(Math.toRadians(lat0))
    val s = Math.sin(headingRad)
    val c = Math.cos(headingRad)
    val forward = dEast * s + dNorth * c
    val right = dEast * c - dNorth * s
    return JSONArray().put(right.roundToInt()).put(forward.roundToInt())
  }

  private fun simplifyInstruction(maneuver: String?, instructions: String?): String {
    val m = (maneuver ?: "").lowercase()
    return when {
      m.contains("uturn") || m.contains("u-turn") -> "UTURN"
      m.contains("roundabout") || m.contains("rotary") -> "ROUNDABOUT"
      m.contains("arrive") -> "ARRIVE"
      m.contains("left") -> if (m.contains("slight")) "SLIGHT_LEFT" else if (m.contains("sharp")) "SHARP_LEFT" else "LEFT"
      m.contains("right") -> if (m.contains("slight")) "SLIGHT_RIGHT" else if (m.contains("sharp")) "SHARP_RIGHT" else "RIGHT"
      m.contains("straight") || m.contains("continue") || m.contains("depart") -> "STRAIGHT"
      else -> {
        val t = (instructions ?: "").lowercase()
        when {
          t.contains("left") -> "LEFT"
          t.contains("right") -> "RIGHT"
          else -> "STRAIGHT"
        }
      }
    }
  }
}
