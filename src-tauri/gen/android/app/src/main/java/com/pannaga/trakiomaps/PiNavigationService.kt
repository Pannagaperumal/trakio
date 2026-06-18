package com.pannaga.trakiomaps

import android.Manifest
import android.annotation.SuppressLint
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
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
import android.provider.ContactsContract
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
    const val EXTRA_ROUTE_PAYLOAD = "route_payload"

    // logcat tag — filter with: adb logcat -s TrakioBLE
    private const val TAG = "TrakioBLE"

    private const val NOTIFICATION_CHANNEL_ID = "trakio_navigation_tracking"
    private const val NOTIFICATION_ID = 4102
    private const val ADVANCE_THRESHOLD_METERS = 35.0
    private const val ARRIVAL_THRESHOLD_METERS = 25.0
    private const val CALL_DEDUPE_MS = 8000L

    // ── Minimal-map tuning (mirrors the old JS computeMiniMap) ──
    private const val LOOK_AHEAD_METERS = 200.0  // route distance ahead to send
    private const val MAX_ROUTE_POINTS = 10      // cap on polyline vertices
    private const val METERS_PER_DEG = 111320.0

    // ── Nordic UART Service (NUS) — match these UUIDs in the ESP32 firmware.
    // The phone writes JSON frames (newline-delimited) to the RX characteristic.
    private val NUS_SERVICE: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    private val NUS_RX_CHAR: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")

    // Prefer a bonded device whose name contains this (case-insensitive);
    // otherwise the first bonded device is used. Empty = no name preference.
    private const val TARGET_NAME_SUBSTR = "trakio"

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

  // ── BLE state (touched from both the main thread and GATT callback thread) ──
  @Volatile private var bluetoothGatt: BluetoothGatt? = null
  @Volatile private var rxChar: BluetoothGattCharacteristic? = null
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
    ensureNotificationChannel()
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    when (intent?.action) {
      ACTION_START_NAVIGATION -> {
        val payload = intent.getStringExtra(EXTRA_ROUTE_PAYLOAD)
        if (payload.isNullOrBlank()) {
          Log.w(TAG, "start ignored: empty route payload")
          stopSelf()
          return START_NOT_STICKY
        }

        // Going foreground can throw on Android 14+ if the location runtime
        // permission isn't granted — never let that crash the app.
        if (!startForegroundSafely()) {
          stopSelf()
          return START_NOT_STICKY
        }

        navActive = true
        navArrived = false
        navStepIdx = 0
        if (!parseRoutePayload(payload)) {
          Log.w(TAG, "start aborted: could not parse route payload")
          stopNavigation(sendRouteEnd = false)
          return START_NOT_STICKY
        }
        Log.i(TAG, "ride START — ${routeSteps.size} steps, ${routeCoords.size} route points")
        reconnectAttempts = 0
        connectBle()
        registerPhoneStateReceiver()
        registerBtStateReceiver()
        startLocationUpdates()
      }

      ACTION_STOP_NAVIGATION -> stopNavigation(sendRouteEnd = true)

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
    mainHandler.removeCallbacksAndMessages(null)
    stopLocationUpdates()
    unregisterPhoneStateReceiver()
    unregisterBtStateReceiver()
    disconnectBle()
    super.onDestroy()
  }

  private fun startForegroundSafely(): Boolean {
    return try {
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
        startForeground(NOTIFICATION_ID, buildNotification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION)
      } else {
        startForeground(NOTIFICATION_ID, buildNotification())
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
    val heading = if (location.hasBearing()) location.bearing.toDouble() else 0.0

    sendFrame(computeMiniMap(location.latitude, location.longitude, heading, step, distToEnd))

    if (navStepIdx == routeSteps.lastIndex && distToEnd < ARRIVAL_THRESHOLD_METERS) {
      navArrived = true
      sendFrame(
        JSONObject()
          .put("type", "nav")
          .put("heading", heading.roundToInt())
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
  ): String {
    val hRad = Math.toRadians(heading)
    val route = JSONArray().put(JSONArray().put(0).put(0)) // origin = current position

    if (routeCoords.isNotEmpty()) {
      // Nearest route vertex to where we actually are.
      var nearestIdx = 0
      var nearestDist = Double.MAX_VALUE
      for (i in routeCoords.indices) {
        val d = haversine(lat, lng, routeCoords[i].lat, routeCoords[i].lng)
        if (d < nearestDist) { nearestDist = d; nearestIdx = i }
      }

      // Walk forward, projecting vertices until the look-ahead window is full.
      var acc = 0.0
      var prevLat = lat
      var prevLng = lng
      var i = nearestIdx + 1
      while (i < routeCoords.size && route.length() < MAX_ROUTE_POINTS) {
        val v = routeCoords[i]
        acc += haversine(prevLat, prevLng, v.lat, v.lng)
        route.put(toLocalXY(v.lat, v.lng, lat, lng, hRad))
        prevLat = v.lat; prevLng = v.lng
        if (acc >= LOOK_AHEAD_METERS) break
        i++
      }
    }

    // Remaining distance = to current step end + all later steps.
    var remDist = distToEnd
    for (i in navStepIdx + 1 until routeSteps.size) remDist += routeSteps[i].distanceM
    // Remaining time = unfinished fraction of current step + all later steps.
    val curFrac = if (step.distanceM > 0) (distToEnd / step.distanceM).coerceIn(0.0, 1.0) else 0.0
    var remTime = step.durationS * curFrac
    for (i in navStepIdx + 1 until routeSteps.size) remTime += routeSteps[i].durationS

    return JSONObject()
      .put("type", "nav")
      .put("heading", heading.roundToInt())
      .put("distanceToTurn", distToEnd.roundToInt())
      .put("instruction", simplifyInstruction(step.maneuver, step.instructions))
      .put("route", route)
      .put("remainingDistance", remDist.roundToInt())     // metres to destination
      .put("remainingTime", remTime.roundToInt())         // seconds to destination
      .put("eta", System.currentTimeMillis() + (remTime * 1000).toLong()) // epoch ms
      .put("arrived", false)
      .toString()
  }

  // Project (lat,lng) into the local heading-up metric frame centred on
  // (lat0,lng0): +y = forward (travel direction), +x = right. Returns [x, y].
  private fun toLocalXY(lat: Double, lng: Double, lat0: Double, lng0: Double, headingRad: Double): JSONArray {
    val dNorth = (lat - lat0) * METERS_PER_DEG
    val dEast = (lng - lng0) * METERS_PER_DEG * cos(Math.toRadians(lat0))
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

  private fun haversine(lat1: Double, lng1: Double, lat2: Double, lng2: Double): Double {
    val results = FloatArray(1)
    Location.distanceBetween(lat1, lng1, lat2, lng2, results)
    return results[0].toDouble()
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

      routeSteps.isNotEmpty()
    } catch (e: Exception) {
      Log.e(TAG, "parseRoutePayload failed: ${e.message}")
      routeSteps = emptyList()
      routeCoords = emptyList()
      false
    }
  }

  private fun startLocationUpdates() {
    if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED &&
      ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
      Log.w(TAG, "startLocationUpdates: location permission not granted")
      return
    }

    val manager = locationManager ?: return
    runCatching {
      if (manager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
        manager.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 1f, this, Looper.getMainLooper())
      }
      if (manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
        manager.requestLocationUpdates(LocationManager.NETWORK_PROVIDER, 1500L, 1f, this, Looper.getMainLooper())
      }
    }.onFailure { Log.w(TAG, "requestLocationUpdates failed: ${it.message}") }
  }

  private fun stopLocationUpdates() {
    runCatching { locationManager?.removeUpdates(this) }
  }

  private fun stopNavigation(sendRouteEnd: Boolean) {
    if (sendRouteEnd) {
      sendFrame(JSONObject().put("type", "route_end").toString())
    }
    navActive = false
    navArrived = false
    navStepIdx = 0
    routeSteps = emptyList()
    routeCoords = emptyList()
    routeStartFrame = null
    stopLocationUpdates()
    unregisterPhoneStateReceiver()
    unregisterBtStateReceiver()
    // Give the route_end frame a moment to flush over BLE before tearing down.
    mainHandler.postDelayed({
      runCatching {
        disconnectBle()
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
      }
    }, 500L)
  }

  // ── BLE: connect to the bonded ESP32 and stream JSON frames ──
  private fun hasBtPermission(): Boolean {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true
    return ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
  }

  @SuppressLint("MissingPermission")
  private fun connectBle() {
    if (!navActive) return
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

    Log.i(TAG, "connectBle: connecting to ${target.name ?: "?"} (${target.address})")
    rxChar = null
    servicesReady = false
    // autoConnect=false → fast direct connect; we handle reconnection ourselves.
    bluetoothGatt = try {
      target.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
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
    servicesReady = false
    val gatt = bluetoothGatt
    bluetoothGatt = null
    if (gatt != null && hasBtPermission()) {
      runCatching { gatt.disconnect() }
      runCatching { gatt.close() }
    }
  }

  private fun scheduleReconnect() {
    if (!navActive) return
    reconnectAttempts++
    val delay = (RECONNECT_BASE_DELAY_MS * reconnectAttempts).coerceAtMost(RECONNECT_MAX_DELAY_MS)
    Log.d(TAG, "scheduling reconnect #$reconnectAttempts in ${delay}ms")
    mainHandler.postDelayed({ if (navActive && bluetoothGatt == null) connectBle() }, delay)
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
          Log.w(TAG, "GATT disconnected, navActive=$navActive")
          rxChar = null
          servicesReady = false
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
      rxChar = service?.getCharacteristic(NUS_RX_CHAR)
      if (rxChar != null) {
        servicesReady = true
        Log.i(TAG, "NUS RX characteristic found — link ready, streaming will begin")
        // Always (re)send the trip summary first so the ESP32 UI has it.
        routeStartFrame?.let { enqueueChunks(it) }
        flushPendingFrames()
      } else {
        Log.w(TAG, "NUS service/RX char NOT found (service=${service != null}). " +
          "Check the ESP32 advertises NUS service $NUS_SERVICE with RX char $NUS_RX_CHAR")
      }
    }

    override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
      if (gatt !== bluetoothGatt) return
      if (status != BluetoothGatt.GATT_SUCCESS) Log.w(TAG, "onCharacteristicWrite status=$status")
      synchronized(writeQueue) { writing = false }
      processWriteQueue()
    }
  }

  // Queue a JSON frame; chunk it to the negotiated MTU and stream it out.
  private fun sendFrame(json: String) {
    if (rxChar == null) {
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
        if (state != TelephonyManager.EXTRA_STATE_RINGING) return

        val number = intent.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER)?.trim().orEmpty()
        val caller = resolveCallerName(number) ?: if (number.isNotBlank()) number else "Incoming call"
        maybeSendIncomingCall(caller, number)
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
        if (state == BluetoothAdapter.STATE_ON && navActive && bluetoothGatt == null) {
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
  }

  private fun buildNotification() = NotificationCompat.Builder(this, NOTIFICATION_CHANNEL_ID)
    .setSmallIcon(R.mipmap.ic_launcher)
    .setContentTitle(getString(R.string.tracking_notification_title))
    .setContentText(getString(R.string.tracking_notification_text))
    .setOngoing(true)
    .setSilent(true)
    .build()

  private fun distanceTo(location: Location, end: LatLng): Double {
    val results = FloatArray(1)
    Location.distanceBetween(location.latitude, location.longitude, end.lat, end.lng, results)
    return results[0].toDouble()
  }

  data class LatLng(val lat: Double, val lng: Double)
  data class RouteStep(
    val instructions: String,
    val maneuver: String,
    val endLocation: LatLng,
    val distanceM: Double,
    val durationS: Double,
  )
}
