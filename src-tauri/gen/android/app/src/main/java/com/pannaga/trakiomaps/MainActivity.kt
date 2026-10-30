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
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

class MainActivity : TauriActivity() {
  private val permissionRequestCode = 4101

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    enterFullScreen()
    requestMissingPermissions()
    // Park guard keeps the device linked between rides — resume it on launch.
    if (DeviceSettings.load(this).parkGuard) startIdleLink()
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

  // Immersive full screen: status and navigation bars hidden; a swipe from
  // the edge shows them briefly. Re-applied on focus since dialogs (e.g.
  // permission prompts) bring the bars back.
  private fun enterFullScreen() {
    WindowCompat.setDecorFitsSystemWindows(window, false)
    WindowInsetsControllerCompat(window, window.decorView).apply {
      hide(WindowInsetsCompat.Type.systemBars())
      systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    }
  }

  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    if (hasFocus) enterFullScreen()
  }

  private fun requestMissingPermissions() {
    val requiredPermissions = mutableListOf(
      Manifest.permission.ACCESS_FINE_LOCATION,
      Manifest.permission.ACCESS_COARSE_LOCATION,
      Manifest.permission.READ_PHONE_STATE,
      Manifest.permission.READ_CONTACTS,
      Manifest.permission.READ_CALL_LOG,
    )

    // Accept / decline from the device's call screen.
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      requiredPermissions += Manifest.permission.ANSWER_PHONE_CALLS
    }

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

  private fun hasBtConnectPermission(): Boolean =
    Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
      ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

  // Returns false when Nearby devices permission is missing: a connectedDevice
  // FGS can't start without it.
  private fun startIdleLink(): Boolean {
    if (!hasBtConnectPermission()) return false
    return runCatching {
      val intent = Intent(this, PiNavigationService::class.java).setAction(PiNavigationService.ACTION_START_LINK)
      ContextCompat.startForegroundService(this, intent)
    }.onFailure { Log.e("TrakioBLE", "startIdleLink failed: ${it.message}") }.isSuccess
  }

  private fun stopIdleLink() {
    if (PiNavigationService.instance == null) return
    runCatching {
      startService(Intent(this, PiNavigationService::class.java).setAction(PiNavigationService.ACTION_STOP_LINK))
    }
  }

  private inner class TrakioAndroidBridge {
    @JavascriptInterface
    fun startBackgroundRide(routePayload: String) {
      requestMissingPermissions()
      runCatching {
        // Staged in memory rather than as an extra: with the road corridor the
        // payload can exceed the Binder transaction limit.
        PiNavigationService.stageRoutePayload(routePayload)
        val intent = Intent(this@MainActivity, PiNavigationService::class.java).apply {
          action = PiNavigationService.ACTION_START_NAVIGATION
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

    // Road corridor that finished loading after the ride started.
    @JavascriptInterface
    fun setRouteCorridor(roadsJson: String) {
      PiNavigationService.instance?.updateCorridor(roadsJson)
    }

    @JavascriptInterface
    fun getDeviceStatus(): String = DeviceStatus.toJson()

    @JavascriptInterface
    fun getDeviceSettings(): String = DeviceSettings.load(this@MainActivity).toJson()

    @JavascriptInterface
    fun setDeviceSettings(patchJson: String): String {
      val before = DeviceSettings.load(this@MainActivity)
      val next = runCatching { DeviceSettings.update(this@MainActivity, patchJson) }
        .onFailure { Log.w("TrakioBLE", "setDeviceSettings: bad JSON: ${it.message}") }
        .getOrDefault(before)
      PiNavigationService.instance?.onSettingsChanged(next)
      if (next.parkGuard && !before.parkGuard) runOnUiThread { startIdleLink() }
      if (!next.parkGuard && before.parkGuard) runOnUiThread { stopIdleLink() }
      return next.toJson()
    }

    // Connect now without starting a ride (battery, theme preview, calls).
    @JavascriptInterface
    fun connectDevice(): Boolean {
      if (!hasBtConnectPermission()) {
        runOnUiThread { requestMissingPermissions() }
        return false
      }
      runOnUiThread { startIdleLink() }
      return true
    }

    @JavascriptInterface
    fun disconnectDevice() {
      runOnUiThread { stopIdleLink() }
    }
  }
}
