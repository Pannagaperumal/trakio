package com.pannaga.trakiomaps

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.provider.ContactsContract
import android.telephony.TelephonyManager
import android.webkit.GeolocationPermissions
import android.webkit.JavascriptInterface
import android.webkit.WebChromeClient
import android.webkit.WebView
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import org.json.JSONObject

class MainActivity : TauriActivity() {
  private val permissionRequestCode = 4101

  private var appWebView: WebView? = null
  private var phoneStateReceiver: BroadcastReceiver? = null
  private var lastIncomingSignature = ""
  private var lastIncomingAt = 0L

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    requestMissingPermissions()
    registerPhoneStateReceiver()
  }

  override fun onDestroy() {
    phoneStateReceiver?.let { unregisterReceiver(it) }
    phoneStateReceiver = null
    super.onDestroy()
  }

  override fun onWebViewCreate(webView: WebView) {
    appWebView = webView
    webView.addJavascriptInterface(TrakioAndroidBridge(), "TrakioAndroidBridge")
    webView.webChromeClient = object : WebChromeClient() {
      override fun onGeolocationPermissionsShowPrompt(
        origin: String,
        callback: GeolocationPermissions.Callback
      ) {
        requestMissingPermissions()
        callback.invoke(origin, true, false)
      }
    }
  }

  private fun requestMissingPermissions() {
    val requiredPermissions = mutableListOf(
      Manifest.permission.ACCESS_FINE_LOCATION,
      Manifest.permission.ACCESS_COARSE_LOCATION,
      Manifest.permission.READ_PHONE_STATE,
      Manifest.permission.READ_CONTACTS,
    )

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      requiredPermissions += Manifest.permission.ACCESS_BACKGROUND_LOCATION
    }

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      requiredPermissions += Manifest.permission.POST_NOTIFICATIONS
    }

    val missing = requiredPermissions.filter {
      ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
    }

    if (missing.isNotEmpty()) {
      ActivityCompat.requestPermissions(this, missing.toTypedArray(), permissionRequestCode)
    }
  }

  private fun registerPhoneStateReceiver() {
    if (phoneStateReceiver != null) {
      return
    }

    phoneStateReceiver = object : BroadcastReceiver() {
      override fun onReceive(context: Context?, intent: Intent?) {
        if (intent?.action != TelephonyManager.ACTION_PHONE_STATE_CHANGED) {
          return
        }

        val state = intent.getStringExtra(TelephonyManager.EXTRA_STATE)
        if (state != TelephonyManager.EXTRA_STATE_RINGING) {
          return
        }

        val number = intent.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER)?.trim().orEmpty()
        val callerName = resolveCallerName(number)
        maybeEmitIncomingCall(callerName, number)
      }
    }

    val filter = IntentFilter(TelephonyManager.ACTION_PHONE_STATE_CHANGED)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      registerReceiver(phoneStateReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
    } else {
      registerReceiver(phoneStateReceiver, filter)
    }
  }

  private fun resolveCallerName(number: String): String? {
    if (number.isBlank()) {
      return null
    }

    if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED) {
      return null
    }

    val uri = Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(number))
    contentResolver.query(
      uri,
      arrayOf(ContactsContract.PhoneLookup.DISPLAY_NAME),
      null,
      null,
      null,
    )?.use { cursor ->
      if (cursor.moveToFirst()) {
        return cursor.getString(0)
      }
    }

    return null
  }

  private fun maybeEmitIncomingCall(callerName: String?, number: String) {
    val caller = callerName ?: if (number.isNotBlank()) number else "Incoming call"
    val signature = if (number.isNotBlank()) number else caller
    val now = SystemClock.elapsedRealtime()
    if (signature == lastIncomingSignature && now - lastIncomingAt < 8000) {
      return
    }

    lastIncomingSignature = signature
    lastIncomingAt = now
    emitIncomingCallToWebView(caller, number)
  }

  private fun emitIncomingCallToWebView(caller: String, number: String) {
    val webView = appWebView ?: return
    val callerJson = JSONObject.quote(caller)
    val numberJson = JSONObject.quote(number)
    val callIdJson = JSONObject.quote(if (number.isNotBlank()) number else caller)
    val script = "window.__trakioIncomingCall && window.__trakioIncomingCall({caller:$callerJson, number:$numberJson, call_id:$callIdJson});"
    webView.post {
      webView.evaluateJavascript(script, null)
    }
  }

  private inner class TrakioAndroidBridge {
    @JavascriptInterface
    fun startBackgroundRide(routePayload: String) {
      requestMissingPermissions()
      val intent = Intent(this@MainActivity, PiNavigationService::class.java).apply {
        action = PiNavigationService.ACTION_START_NAVIGATION
        putExtra(PiNavigationService.EXTRA_ROUTE_PAYLOAD, routePayload)
      }
      ContextCompat.startForegroundService(this@MainActivity, intent)
    }

    @JavascriptInterface
    fun stopBackgroundRide() {
      val intent = Intent(this@MainActivity, PiNavigationService::class.java).apply {
        action = PiNavigationService.ACTION_STOP_NAVIGATION
      }
      startService(intent)
    }
  }
}
