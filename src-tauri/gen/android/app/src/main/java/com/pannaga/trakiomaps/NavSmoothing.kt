package com.pannaga.trakiomaps

// Cleans up raw location data before it reaches the display. Pure logic so it
// runs in JVM tests.
internal object NavSmoothing {
  const val MAX_ACCURACY_M = 35f        // worse fixes are ignored once we have a good one
  const val GPS_FRESH_MS = 10_000L      // network fixes are ignored while GPS is this fresh
  const val MAX_PLAUSIBLE_MPS = 45.0    // ~160 km/h: anything faster is a position jump
  const val STILL_MPS = 0.8             // GPS speed noise when standing still

  // Network (cell/Wi-Fi) fixes can be hundreds of metres off; only use them
  // when GPS has gone quiet (tunnels, indoors). Poor GPS fixes are dropped
  // unless we have nothing yet.
  fun acceptFix(isGps: Boolean, accuracyM: Float?, msSinceLastGps: Long, haveFix: Boolean): Boolean {
    if (!isGps) return msSinceLastGps >= GPS_FRESH_MS
    if (haveFix && accuracyM != null && accuracyM > MAX_ACCURACY_M) return false
    return true
  }

  class SpeedFilter(private val alpha: Double = 0.35) {
    var value = 0.0
      private set

    // reported: the receiver's Doppler speed (reliable), else null.
    // derived: distance / time between the last two accepted fixes.
    fun update(reported: Double?, derived: Double?): Double {
      val raw = reported ?: derived ?: return value
      if (raw.isNaN() || raw < 0 || raw > MAX_PLAUSIBLE_MPS) return value   // a jump, not motion
      val clean = if (raw < STILL_MPS) 0.0 else raw
      value = if (clean == 0.0 && value < STILL_MPS * 2) 0.0 else value + (clean - value) * alpha
      return value
    }

    fun reset() { value = 0.0 }
  }

  // Distance to the next turn shown to the rider: follows the rider down
  // smoothly, ignores small upward wobble from GPS noise, and only goes back
  // up for a real increase (e.g. riding away from the turn).
  class TurnDistanceFilter(private val riseThresholdM: Double = 20.0, private val alpha: Double = 0.5) {
    private var step = -1
    var value = 0.0
      private set

    fun update(stepIdx: Int, raw: Double): Double {
      if (stepIdx != step || value == 0.0) {
        step = stepIdx
        value = raw
      } else if (raw < value) {
        value += (raw - value) * alpha
      } else if (raw > value + riseThresholdM) {
        value = raw
      }
      return value
    }

    fun reset() { step = -1; value = 0.0 }
  }
}
