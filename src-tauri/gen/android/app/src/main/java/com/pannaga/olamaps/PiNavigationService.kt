package com.pannaga.trakiomaps

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.net.Uri
import android.os.Build
import android.os.IBinder
import android.os.Looper
import android.provider.ContactsContract
import android.telephony.TelephonyManager
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import org.java_websocket.client.WebSocketClient
import org.java_websocket.handshake.ServerHandshake
import org.json.JSONArray
import org.json.JSONObject
import java.net.URI
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.math.roundToInt

class PiNavigationService : Service(), LocationListener {
  companion object {
    const val ACTION_START_NAVIGATION = "com.pannaga.trakiomaps.action.START_NAVIGATION"
    const val ACTION_STOP_NAVIGATION = "com.pannaga.trakiomaps.action.STOP_NAVIGATION"
    const val EXTRA_ROUTE_PAYLOAD = "route_payload"

    private const val NOTIFICATION_CHANNEL_ID = "trakio_navigation_tracking"
    private const val NOTIFICATION_ID = 4102
    private const val ADVANCE_THRESHOLD_METERS = 35.0
    private const val ARRIVAL_THRESHOLD_METERS = 25.0
    private const val CALL_DEDUPE_MS = 8000L
  }

  private val pendingMessages = CopyOnWriteArrayList<String>()
  private var socketClient: WebSocketClient? = null
  private var locationManager: LocationManager? = null
  private var phoneStateReceiver: BroadcastReceiver? = null

  private var displayMode = "map"
  private var routeSteps: List<RouteStep> = emptyList()
  private var navStepIdx = 0
  private var navArrived = false
  private var navActive = false
  private var lastCallSignature = ""
  private var lastCallAt = 0L

  override fun onBind(intent: Intent?): IBinder? = null

  override fun onCreate() {
    super.onCreate()
    locationManager = getSystemService(Context.LOCATION_SERVICE) as LocationManager
    ensureNotificationChannel()
    ensureSocketClient()
  }

  override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
    when (intent?.action) {
      ACTION_START_NAVIGATION -> {
        val payload = intent.getStringExtra(EXTRA_ROUTE_PAYLOAD)
        if (!payload.isNullOrBlank()) {
          startForeground(NOTIFICATION_ID, buildNotification())
          navActive = true
          navArrived = false
          navStepIdx = 0
          parseRoutePayload(payload)
          ensureSocketClient()
          sendMessage(payload)
          registerPhoneStateReceiver()
          startLocationUpdates()
        }
      }

      ACTION_STOP_NAVIGATION -> {
        stopNavigation(sendRouteEnd = true)
      }
    }

    return START_STICKY
  }

  override fun onDestroy() {
    stopLocationUpdates()
    unregisterPhoneStateReceiver()
    socketClient?.close()
    socketClient = null
    super.onDestroy()
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

    sendMessage(
      JSONObject()
        .put("type", "position")
        .put("display_mode", displayMode)
        .put("lat", location.latitude)
        .put("lng", location.longitude)
        .put("heading", heading)
        .put("speed", if (location.hasSpeed()) location.speed.toDouble() else JSONObject.NULL)
        .put("step_idx", navStepIdx)
        .put("step", step.instructions)
        .put("dist_next", formatDistance(distToEnd))
        .put("arrived", false)
        .toString()
    )

    if (navStepIdx == routeSteps.lastIndex && distToEnd < ARRIVAL_THRESHOLD_METERS) {
      navArrived = true
      sendMessage(
        JSONObject()
          .put("type", "position")
          .put("display_mode", displayMode)
          .put("lat", location.latitude)
          .put("lng", location.longitude)
          .put("heading", heading)
          .put("step_idx", navStepIdx)
          .put("step", "Arriving at destination")
          .put("dist_next", "0 m")
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

  private fun parseRoutePayload(payload: String) {
    val json = JSONObject(payload)
    displayMode = json.optString("display_mode", "map")
    val steps = json.optJSONArray("steps") ?: JSONArray()
    routeSteps = buildList {
      for (idx in 0 until steps.length()) {
        val step = steps.getJSONObject(idx)
        val endLocation = step.optJSONObject("end_location") ?: continue
        add(
          RouteStep(
            instructions = step.optString("instructions", "Continue"),
            endLocation = LatLng(
              lat = endLocation.optDouble("lat", 0.0),
              lng = endLocation.optDouble("lng", 0.0),
            ),
          )
        )
      }
    }
  }

  private fun startLocationUpdates() {
    if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED &&
      ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
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
    }
  }

  private fun stopLocationUpdates() {
    runCatching { locationManager?.removeUpdates(this) }
  }

  private fun stopNavigation(sendRouteEnd: Boolean) {
    if (sendRouteEnd) {
      sendMessage(JSONObject().put("type", "route_end").toString())
    }
    navActive = false
    navArrived = false
    navStepIdx = 0
    routeSteps = emptyList()
    stopLocationUpdates()
    unregisterPhoneStateReceiver()
    stopForeground(STOP_FOREGROUND_REMOVE)
    stopSelf()
  }

  private fun ensureSocketClient() {
    val client = socketClient
    if (client?.isOpen == true || client?.isConnecting == true) {
      return
    }

    socketClient = object : WebSocketClient(URI("ws://127.0.0.1:9001")) {
      override fun onOpen(handshakedata: ServerHandshake?) {
        flushPendingMessages()
      }

      override fun onMessage(message: String?) = Unit

      override fun onClose(code: Int, reason: String?, remote: Boolean) = Unit

      override fun onError(ex: Exception?) = Unit
    }.also { it.connect() }
  }

  private fun sendMessage(payload: String) {
    val client = socketClient
    if (client?.isOpen == true) {
      client.send(payload)
      return
    }
    pendingMessages += payload
    ensureSocketClient()
  }

  private fun flushPendingMessages() {
    val client = socketClient ?: return
    if (!client.isOpen) return
    val messages = pendingMessages.toList()
    pendingMessages.clear()
    messages.forEach(client::send)
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
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      registerReceiver(phoneStateReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
    } else {
      registerReceiver(phoneStateReceiver, filter)
    }
  }

  private fun unregisterPhoneStateReceiver() {
    phoneStateReceiver?.let { unregisterReceiver(it) }
    phoneStateReceiver = null
  }

  private fun maybeSendIncomingCall(caller: String, number: String) {
    val signature = if (number.isNotBlank()) number else caller
    val now = System.currentTimeMillis()
    if (signature == lastCallSignature && now - lastCallAt < CALL_DEDUPE_MS) {
      return
    }
    lastCallSignature = signature
    lastCallAt = now

    sendMessage(
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

    val uri = Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(number))
    contentResolver.query(uri, arrayOf(ContactsContract.PhoneLookup.DISPLAY_NAME), null, null, null)?.use { cursor ->
      if (cursor.moveToFirst()) {
        return cursor.getString(0)
      }
    }
    return null
  }

  private fun ensureNotificationChannel() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
    val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
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

  private fun formatDistance(meters: Double): String {
    return if (meters >= 1000) {
      String.format("%.1f km", meters / 1000.0)
    } else {
      "${meters.roundToInt()} m"
    }
  }

  data class LatLng(val lat: Double, val lng: Double)
  data class RouteStep(val instructions: String, val endLocation: LatLng)
}