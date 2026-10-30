package com.pannaga.trakiomaps

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NavSmoothingTest {
  @Test
  fun networkFixesIgnoredWhileGpsIsFresh() {
    assertFalse(NavSmoothing.acceptFix(isGps = false, accuracyM = 20f, msSinceLastGps = 2_000, haveFix = true))
    assertTrue(NavSmoothing.acceptFix(isGps = false, accuracyM = 300f, msSinceLastGps = 30_000, haveFix = true))
    assertFalse(NavSmoothing.acceptFix(isGps = true, accuracyM = 80f, msSinceLastGps = 0, haveFix = true))
    assertTrue(NavSmoothing.acceptFix(isGps = true, accuracyM = 80f, msSinceLastGps = 0, haveFix = false))
  }

  @Test
  fun positionJumpDoesNotSpikeSpeed() {
    val f = NavSmoothing.SpeedFilter()
    f.update(reported = null, derived = 0.2)
    // A 300 m network jump in ~5 s reads as ~64 m/s (230 km/h) — must be ignored.
    assertEquals(0.0, f.update(reported = null, derived = 64.0), 0.001)
  }

  @Test
  fun standingStillReadsZero() {
    val f = NavSmoothing.SpeedFilter()
    repeat(5) { f.update(reported = 0.5, derived = null) }
    assertEquals(0.0, f.value, 0.001)
  }

  @Test
  fun speedFollowsRealMotionSmoothly() {
    val f = NavSmoothing.SpeedFilter()
    repeat(20) { f.update(reported = 10.0, derived = null) }
    assertEquals(10.0, f.value, 0.1)
    val next = f.update(reported = 12.0, derived = null)
    assertTrue(next > 10.0 && next < 12.0)
  }

  @Test
  fun turnDistanceIgnoresSmallUpwardWobble() {
    val f = NavSmoothing.TurnDistanceFilter()
    f.update(0, 55.0)
    val down = f.update(0, 45.0)
    assertTrue(down < 55.0 && down >= 45.0)
    // GPS noise pushes it back up by 10 m: the shown value must not jump back.
    assertEquals(down, f.update(0, 55.0), 0.001)
  }

  @Test
  fun turnDistanceRisesForRealAndResetsOnNewStep() {
    val f = NavSmoothing.TurnDistanceFilter()
    f.update(0, 100.0)
    assertEquals(150.0, f.update(0, 150.0), 0.001)   // rode away from the turn
    assertEquals(400.0, f.update(1, 400.0), 0.001)   // next instruction
  }
}
