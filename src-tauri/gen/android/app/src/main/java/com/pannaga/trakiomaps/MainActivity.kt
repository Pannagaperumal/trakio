package com.pannaga.trakiomaps

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.webkit.GeolocationPermissions
import android.webkit.JavascriptInterface
import android.webkit.WebChromeClient
import android.webkit.WebView
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

class MainActivity : TauriActivity() {
  private val permissionRequestCode = 4101

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    requestMissingPermissions()
  }

  override fun onWebViewCreate(webView: WebView) {
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
      Manifest.permission.READ_CALL_LOG,
    )

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
      requiredPermissions += Manifest.permission.ACCESS_BACKGROUND_LOCATION
    }

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      requiredPermissions += Manifest.permission.POST_NOTIFICATIONS
    }

    // BLE streaming to the ESP32 display — connecting to an already-bonded
    // device only needs BLUETOOTH_CONNECT (we never scan).
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      requiredPermissions += Manifest.permission.BLUETOOTH_CONNECT
    }

    val missing = requiredPermissions.filter {
      ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
    }

    if (missing.isNotEmpty()) {
      ActivityCompat.requestPermissions(this, missing.toTypedArray(), permissionRequestCode)
    }
  }

  private inner class TrakioAndroidBridge {
    @JavascriptInterface
    fun startBackgroundRide(routePayload: String) {
      requestMissingPermissions()
      runCatching {
        val intent = Intent(this@MainActivity, PiNavigationService::class.java).apply {
          action = PiNavigationService.ACTION_START_NAVIGATION
          putExtra(PiNavigationService.EXTRA_ROUTE_PAYLOAD, routePayload)
        }
        ContextCompat.startForegroundService(this@MainActivity, intent)
      }.onFailure { Log.e("TrakioBLE", "startBackgroundRide failed: ${it.message}") }
    }

    @JavascriptInterface
    fun stopBackgroundRide() {
      runCatching {
        val intent = Intent(this@MainActivity, PiNavigationService::class.java).apply {
          action = PiNavigationService.ACTION_STOP_NAVIGATION
        }
        startService(intent)
      }.onFailure { Log.e("TrakioBLE", "stopBackgroundRide failed: ${it.message}") }
    }
  }
}
