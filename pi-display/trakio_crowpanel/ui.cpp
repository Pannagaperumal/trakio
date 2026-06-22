// Premium LVGL UI for the Trakio Navigator — 320x240 rectangular display.
//
// Three screens with smooth fade transitions, gradient backgrounds and
// breathing animations:
//   • HOME  — branded standby with a breathing ring (shown when idle).
//   • NAV   — left turn panel (arrow + distance + instruction), a heading-up
//             MAP on the right that draws the route polyline in the brand
//             colour with a direction arrowhead at its leading end — that is
//             the path to take — over faint junction context roads. A top
//             progress bar, a pulsing position marker, and a destination +
//             ETA footer complete it.
//   • DONE  — full-screen arrival celebration (big tick, destination, trip
//             totals) shown when the route is complete.
//
// Incoming calls appear as a TOAST that floats on the top layer over whatever
// screen is showing, so navigation keeps running underneath.
//
// LVGL v8/v9 (common API). On v9, if lv_scr_act() is rejected use
// lv_screen_active(). Enable in lv_conf.h: LV_FONT_MONTSERRAT_14/22/28/40
// and LV_USE_BAR, LV_USE_LINE, LV_USE_ANIMATION.

#include <lvgl.h>
#include <math.h>
#include "nav_state.h"
#include "ui.h"

#define SCR_W 320
#define SCR_H 240
#define CX    160
#define CY    120

// Left turn-info panel occupies x[0..PANEL_W]; the map fills the rest.
#define PANEL_W    112

// Map region (a framed panel right of the turn panel). The route + junction
// are scaled to fit and CENTRED in this rectangle so there is no dead space.
#define MAP_X0     116
#define MAP_X1     314
#define MAP_Y0     12
#define MAP_Y1     200
#define MAP_RCX    ((MAP_X0 + MAP_X1) / 2)   // region centre x
#define MAP_RCY    ((MAP_Y0 + MAP_Y1) / 2)   // region centre y
#define MAP_PAD    16    // padding inside the panel, pixels
#define MAP_MAXSCALE 4.0f  // px per metre cap (don't over-zoom tiny content)
#define RIDER_Y    168   // rider's fixed screen y inside the map panel (drive-view)

#define CASING_W   16   // road casing thickness (under the route)
#define ROUTE_W    9    // route line thickness
#define GHOST_W    13   // faint context road thickness

// ── Palette ──────────────────────────────────────────────
#define C_BG     lv_color_hex(0x0B0F1A)
#define C_BG2    lv_color_hex(0x131C30)
#define C_SURF   lv_color_hex(0x1B2336)
#define C_ACCENT lv_color_hex(0x12E29C)   // mint — brand colour
#define C_ROUTE  lv_color_hex(0x12E29C)   // route line = brand colour (the path to take)
#define C_CASING lv_color_hex(0x0C3D31)   // route casing (dim brand)
#define C_GHOST  lv_color_hex(0x2B3858)   // context road surface
#define C_GHOSTC lv_color_hex(0x141C30)   // context road casing (depth)
#define C_MAPBG  lv_color_hex(0x0E1626)   // map panel fill
#define C_MAPBG2 lv_color_hex(0x0A111E)   // map panel fill (gradient end)
#define C_MAPBRD lv_color_hex(0x1E2942)   // map panel border
#define C_TEXT   lv_color_hex(0xF5F7FA)
#define C_DIM    lv_color_hex(0x7A869A)
#define C_CALL   lv_color_hex(0xFF4D5E)
#define C_CALL2  lv_color_hex(0xB91D33)

enum { SC_HOME, SC_NAV, SC_DONE };
static int curScreen = -1;

static lv_obj_t *scrHome, *scrNav, *scrDone;

// HOME
static lv_obj_t *homeStatus;

// NAV
static lv_obj_t *progressBar;
static lv_obj_t *lblArrow, *lblTurnDist, *lblInstr;
static lv_obj_t *mapPanel;
static lv_obj_t *roadCasing, *routeFill, *riderHalo, *riderArrow, *riderArrowEdge;
static lv_obj_t *lblDest, *lblTrip;

// Route + junction geometry (persistent point arrays for lv_line)
static lv_point_t routePts[MAX_ROUTE_POINTS];
static int        routeN = 0;
static lv_point_t jPts[MAX_JUNCTION_ROADS][MAX_JUNCTION_ROAD_POINTS];

// Position marker: a heading-up navigation arrow (Google-Maps style) drawn as
// a kite. Points are fixed (the map is heading-up, so it always points up).
static lv_point_t riderArrowPts[5];

// Junction roads are drawn as faint context only (no branch is emphasised —
// the route polyline itself is the highlighted path). Two layers per road
// (casing + surface) give them depth like real map roads.
static lv_obj_t  *ghostCasing[MAX_JUNCTION_ROADS];
static lv_obj_t  *ghostLine[MAX_JUNCTION_ROADS];

static lv_style_t styleCasing, styleRoute, styleGhost, styleGhostCasing,
                  styleRiderArrow, styleRiderEdge;

// DONE (arrival)
static lv_obj_t *lblDoneDest, *lblDoneStats;

// CALL TOAST (floats on the top layer over any screen)
static lv_obj_t *toast, *lblToastCaller, *lblToastNumber;

// ── Helpers ──────────────────────────────────────────────
static String fmtDist(long m) {
  if (m >= 1000) return String(m / 1000.0, 1) + " km";
  return String(m) + " m";
}
static String fmtTime(long s) {
  if (s >= 3600) return String(s / 3600) + "h " + String((s % 3600) / 60) + "m";
  long mins = (s + 30) / 60;
  return String(mins < 1 ? 1 : mins) + " min";
}
static const char *arrowFor(const String &i) {
  if (i.indexOf("LEFT") >= 0)  return LV_SYMBOL_LEFT;
  if (i.indexOf("RIGHT") >= 0) return LV_SYMBOL_RIGHT;
  if (i == "UTURN" || i == "ROUNDABOUT") return LV_SYMBOL_REFRESH;
  if (i == "ARRIVE") return LV_SYMBOL_OK;
  return LV_SYMBOL_UP;
}
static void gradientBg(lv_obj_t *o) {
  lv_obj_set_style_bg_color(o, C_BG, 0);
  lv_obj_set_style_bg_grad_color(o, C_BG2, 0);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// Breathing ring: grows + fades, centred on (cx,cy).
struct Pulse { lv_obj_t *obj; int cx; int cy; int rMin; };
static void pulse_cb(void *var, int32_t r) {
  Pulse *p = (Pulse *)var;
  lv_obj_set_size(p->obj, r * 2, r * 2);
  lv_obj_set_pos(p->obj, p->cx - r, p->cy - r);
  lv_opa_t opa = (lv_opa_t)(LV_OPA_COVER - ((r - p->rMin) * (LV_OPA_COVER - 40)) / 30);
  lv_obj_set_style_bg_opa(p->obj, opa, 0);
}
static Pulse homePulse, riderPulse;
static void startPulse(Pulse *p, int rMin, int rMax, uint32_t period) {
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, p);
  lv_anim_set_exec_cb(&a, pulse_cb);
  lv_anim_set_values(&a, rMin, rMax);
  lv_anim_set_time(&a, period);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}
static lv_obj_t *circle(lv_obj_t *parent, lv_color_t col) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, col, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

// ── HOME ─────────────────────────────────────────────────
static void buildHome(void) {
  scrHome = lv_obj_create(NULL);
  gradientBg(scrHome);

  lv_obj_t *halo = circle(scrHome, C_ACCENT);
  homePulse = { halo, CX, CY - 6, 26 };
  startPulse(&homePulse, 26, 56, 1800);

  lv_obj_t *glyph = lv_label_create(scrHome);
  lv_obj_set_style_text_font(glyph, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(glyph, C_BG, 0);
  lv_label_set_text(glyph, LV_SYMBOL_GPS);
  lv_obj_align(glyph, LV_ALIGN_CENTER, 0, -6);

  lv_obj_t *brand = lv_label_create(scrHome);
  lv_obj_set_style_text_font(brand, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(brand, C_TEXT, 0);
  lv_obj_set_style_text_letter_space(brand, 4, 0);
  lv_label_set_text(brand, "TRAKIO");
  lv_obj_align(brand, LV_ALIGN_CENTER, 0, 58);

  homeStatus = lv_label_create(scrHome);
  lv_obj_set_style_text_font(homeStatus, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(homeStatus, C_DIM, 0);
  lv_label_set_text(homeStatus, "Ready to ride");
  lv_obj_align(homeStatus, LV_ALIGN_CENTER, 0, 84);
}

// ── NAV ──────────────────────────────────────────────────
// Make a line object on scrNav carrying the given style.
static lv_obj_t *makeLine(lv_style_t *st) {
  lv_obj_t *l = lv_line_create(scrNav);
  lv_obj_add_style(l, st, 0);
  lv_obj_set_pos(l, 0, 0);
  lv_obj_set_size(l, SCR_W, SCR_H);
  lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
  return l;
}

static void buildNav(void) {
  scrNav = lv_obj_create(NULL);
  gradientBg(scrNav);

  // ── Map layer (created first so it sits behind the text panels) ──
  // 0. framed map panel — gives the map a defined area (no floating lines).
  mapPanel = lv_obj_create(scrNav);
  lv_obj_set_pos(mapPanel, MAP_X0, MAP_Y0);
  lv_obj_set_size(mapPanel, MAP_X1 - MAP_X0, MAP_Y1 - MAP_Y0);
  lv_obj_set_style_radius(mapPanel, 14, 0);
  lv_obj_set_style_bg_color(mapPanel, C_MAPBG, 0);
  lv_obj_set_style_bg_grad_color(mapPanel, C_MAPBG2, 0);
  lv_obj_set_style_bg_grad_dir(mapPanel, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_color(mapPanel, C_MAPBRD, 0);
  lv_obj_set_style_border_width(mapPanel, 1, 0);
  lv_obj_clear_flag(mapPanel, LV_OBJ_FLAG_SCROLLABLE);

  // 1. faint junction context roads (casing + surface for depth)
  lv_style_init(&styleGhostCasing);
  lv_style_set_line_width(&styleGhostCasing, GHOST_W + 4);
  lv_style_set_line_color(&styleGhostCasing, C_GHOSTC);
  lv_style_set_line_rounded(&styleGhostCasing, true);
  lv_style_init(&styleGhost);
  lv_style_set_line_width(&styleGhost, GHOST_W);
  lv_style_set_line_color(&styleGhost, C_GHOST);
  lv_style_set_line_rounded(&styleGhost, true);
  for (int i = 0; i < MAX_JUNCTION_ROADS; i++) ghostCasing[i] = makeLine(&styleGhostCasing);
  for (int i = 0; i < MAX_JUNCTION_ROADS; i++) ghostLine[i]   = makeLine(&styleGhost);

  // 2. route casing + route fill (brand colour — the path to take)
  lv_style_init(&styleCasing);
  lv_style_set_line_width(&styleCasing, CASING_W);
  lv_style_set_line_color(&styleCasing, C_CASING);
  lv_style_set_line_rounded(&styleCasing, true);
  roadCasing = makeLine(&styleCasing);

  lv_style_init(&styleRoute);
  lv_style_set_line_width(&styleRoute, ROUTE_W);
  lv_style_set_line_color(&styleRoute, C_ROUTE);
  lv_style_set_line_rounded(&styleRoute, true);
  routeFill = makeLine(&styleRoute);

  // 3. position marker: a heading-up navigation arrow (Google-Maps style) —
  //    soft halo + white edge under a brand-green kite. Points are set in
  //    rebuildMap() once the arrow's screen position is known.
  riderHalo = circle(scrNav, C_ACCENT);
  riderPulse = { riderHalo, MAP_RCX, RIDER_Y, 10 };
  startPulse(&riderPulse, 10, 24, 1500);

  lv_style_init(&styleRiderEdge);
  lv_style_set_line_width(&styleRiderEdge, 12);
  lv_style_set_line_color(&styleRiderEdge, C_TEXT);
  lv_style_set_line_rounded(&styleRiderEdge, true);
  riderArrowEdge = makeLine(&styleRiderEdge);

  lv_style_init(&styleRiderArrow);
  lv_style_set_line_width(&styleRiderArrow, 8);
  lv_style_set_line_color(&styleRiderArrow, C_ACCENT);
  lv_style_set_line_rounded(&styleRiderArrow, true);
  riderArrow = makeLine(&styleRiderArrow);

  // ── Top progress bar ──
  progressBar = lv_bar_create(scrNav);
  lv_obj_set_size(progressBar, SCR_W, 6);
  lv_obj_align(progressBar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(progressBar, C_SURF, LV_PART_MAIN);
  lv_obj_set_style_radius(progressBar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(progressBar, C_ACCENT, LV_PART_INDICATOR);
  lv_obj_set_style_radius(progressBar, 0, LV_PART_INDICATOR);
  lv_bar_set_range(progressBar, 0, 100);
  lv_bar_set_value(progressBar, 0, LV_ANIM_OFF);

  // ── Left turn-info panel ──
  lblArrow = lv_label_create(scrNav);
  lv_obj_set_style_text_font(lblArrow, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(lblArrow, C_ACCENT, 0);
  lv_label_set_text(lblArrow, LV_SYMBOL_UP);
  lv_obj_align(lblArrow, LV_ALIGN_TOP_LEFT, 36, 26);

  lblTurnDist = lv_label_create(scrNav);
  lv_obj_set_style_text_font(lblTurnDist, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(lblTurnDist, C_TEXT, 0);
  lv_label_set_text(lblTurnDist, "-- m");
  lv_obj_align(lblTurnDist, LV_ALIGN_TOP_LEFT, 14, 86);

  lblInstr = lv_label_create(scrNav);
  lv_obj_set_style_text_font(lblInstr, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblInstr, C_DIM, 0);
  lv_label_set_long_mode(lblInstr, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lblInstr, PANEL_W - 16);
  lv_label_set_text(lblInstr, "");
  lv_obj_align(lblInstr, LV_ALIGN_TOP_LEFT, 14, 124);

  // ── Footer (full width) ──
  lblDest = lv_label_create(scrNav);
  lv_obj_set_style_text_font(lblDest, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblDest, C_TEXT, 0);
  lv_label_set_long_mode(lblDest, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblDest, SCR_W - 24);
  lv_obj_set_style_text_align(lblDest, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lblDest, "");
  lv_obj_align(lblDest, LV_ALIGN_BOTTOM_MID, 0, -34);

  lblTrip = lv_label_create(scrNav);
  lv_obj_set_style_text_font(lblTrip, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(lblTrip, C_ACCENT, 0);
  lv_label_set_text(lblTrip, "");
  lv_obj_align(lblTrip, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ── DONE (arrival, full screen) ──────────────────────────
static void buildDone(void) {
  scrDone = lv_obj_create(NULL);
  // Accent-tinted gradient so arrival feels distinct + celebratory.
  lv_obj_set_style_bg_color(scrDone, lv_color_hex(0x0A2A22), 0);
  lv_obj_set_style_bg_grad_color(scrDone, C_BG, 0);
  lv_obj_set_style_bg_grad_dir(scrDone, LV_GRAD_DIR_VER, 0);
  lv_obj_clear_flag(scrDone, LV_OBJ_FLAG_SCROLLABLE);

  // Big tick inside an accent disc.
  lv_obj_t *disc = circle(scrDone, C_ACCENT);
  lv_obj_set_size(disc, 92, 92);
  lv_obj_align(disc, LV_ALIGN_TOP_MID, 0, 28);

  lv_obj_t *tick = lv_label_create(scrDone);
  lv_obj_set_style_text_font(tick, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(tick, lv_color_hex(0x06231A), 0);
  lv_label_set_text(tick, LV_SYMBOL_OK);
  lv_obj_align(tick, LV_ALIGN_TOP_MID, 0, 54);

  lv_obj_t *headline = lv_label_create(scrDone);
  lv_obj_set_style_text_font(headline, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(headline, C_ACCENT, 0);
  lv_obj_set_style_text_letter_space(headline, 2, 0);
  lv_label_set_text(headline, "Arrived");
  lv_obj_align(headline, LV_ALIGN_TOP_MID, 0, 134);

  lblDoneDest = lv_label_create(scrDone);
  lv_obj_set_style_text_font(lblDoneDest, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(lblDoneDest, C_TEXT, 0);
  lv_label_set_long_mode(lblDoneDest, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblDoneDest, SCR_W - 32);
  lv_obj_set_style_text_align(lblDoneDest, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lblDoneDest, "");
  lv_obj_align(lblDoneDest, LV_ALIGN_TOP_MID, 0, 172);

  lblDoneStats = lv_label_create(scrDone);
  lv_obj_set_style_text_font(lblDoneStats, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblDoneStats, C_DIM, 0);
  lv_label_set_text(lblDoneStats, "");
  lv_obj_align(lblDoneStats, LV_ALIGN_BOTTOM_MID, 0, -14);
}

// ── CALL TOAST (top layer, floats over any screen) ───────
static void buildToast(void) {
  toast = lv_obj_create(lv_layer_top());
  lv_obj_set_size(toast, SCR_W - 24, 56);
  lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_radius(toast, 14, 0);
  lv_obj_set_style_bg_color(toast, C_CALL, 0);
  lv_obj_set_style_bg_grad_color(toast, C_CALL2, 0);
  lv_obj_set_style_bg_grad_dir(toast, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_border_width(toast, 0, 0);
  lv_obj_set_style_shadow_width(toast, 18, 0);
  lv_obj_set_style_shadow_opa(toast, LV_OPA_50, 0);
  lv_obj_set_style_shadow_color(toast, lv_color_hex(0x000000), 0);
  lv_obj_set_style_pad_all(toast, 10, 0);
  lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *icon = lv_label_create(toast);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(icon, C_TEXT, 0);
  lv_label_set_text(icon, LV_SYMBOL_CALL);
  lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);

  lblToastCaller = lv_label_create(toast);
  lv_obj_set_style_text_font(lblToastCaller, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(lblToastCaller, C_TEXT, 0);
  lv_label_set_long_mode(lblToastCaller, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblToastCaller, SCR_W - 100);
  lv_label_set_text(lblToastCaller, "");
  lv_obj_align(lblToastCaller, LV_ALIGN_LEFT_MID, 44, -8);

  lblToastNumber = lv_label_create(toast);
  lv_obj_set_style_text_font(lblToastNumber, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblToastNumber, lv_color_hex(0xFFD7DC), 0);
  lv_label_set_long_mode(lblToastNumber, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblToastNumber, SCR_W - 100);
  lv_label_set_text(lblToastNumber, "");
  lv_obj_align(lblToastNumber, LV_ALIGN_LEFT_MID, 44, 12);

  lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
}

void ui_init(void) {
  buildHome();
  buildNav();
  buildDone();
  buildToast();
  curScreen = SC_HOME;
  lv_scr_load(scrHome);
}

static void showScreen(int s) {
  if (s == curScreen) return;
  curScreen = s;
  lv_obj_t *t = s == SC_HOME ? scrHome : (s == SC_NAV ? scrNav : scrDone);
  lv_scr_load_anim(t, LV_SCR_LOAD_ANIM_FADE_ON, 350, 0, false);
}

// Show/hide the floating call toast independently of the active screen.
// Auto-dismiss after a timeout so a missed "call_end" frame can't leave the
// toast covering the map forever.
#define CALL_TOAST_TIMEOUT_MS 25000
static void updateToast(void) {
  static uint32_t callShownAt = 0;
  static bool     wasActive   = false;

  if (callActive && !wasActive) callShownAt = lv_tick_get();   // rising edge
  if (callActive && lv_tick_elaps(callShownAt) > CALL_TOAST_TIMEOUT_MS)
    callActive = false;
  wasActive = callActive;

  if (callActive) {
    lv_label_set_text(lblToastCaller, callCaller.c_str());
    lv_label_set_text(lblToastNumber, callNumber.c_str());
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast);   // keep it above the active screen
  } else {
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
  }
}

// ── Drive-view transform (Google-Maps "start ride" feel) ─────────────────
// The rider is pinned near the BOTTOM of the map panel; the road ahead fills
// the space above. The traveled road is not in the packet, so as you advance
// it scrolls down and off the bottom — disappearing behind you.
// (RIDER_Y is defined with the map-geometry constants near the top of the file
//  because buildNav() references it before this point.)
#define SCALE_EASE  0.14f      // per-render zoom easing toward target (0..1)
static float vScale = 1.0f;    // current px per metre (eased)
static bool  scaleInit = false;

// Compute the target zoom that fills the height with the road ahead, then ease
// the live scale toward it so the zoom never pops when the extent changes.
static void computeFit(void) {
  float maxFwd = 1.0f, maxAbsX = 1.0f;  // forward (up) extent + lateral spread
  for (int i = 0; i < routeLen; i++) {
    if (routeY[i] > maxFwd) maxFwd = routeY[i];
    float ax = routeX[i] < 0 ? -routeX[i] : routeX[i];
    if (ax > maxAbsX) maxAbsX = ax;
  }
  for (int r = 0; r < junctionRoadCount; r++) {
    for (int j = 0; j < junctionRoadLen[r]; j++) {
      if (junctionY[r][j] > maxFwd) maxFwd = junctionY[r][j];
      float ax = junctionX[r][j] < 0 ? -junctionX[r][j] : junctionX[r][j];
      if (ax > maxAbsX) maxAbsX = ax;
    }
  }
  float vAvail = (float)(RIDER_Y - (MAP_Y0 + MAP_PAD));      // room above rider
  float hAvail = (float)((MAP_X1 - MAP_X0) / 2 - MAP_PAD);   // half panel width
  float target = vAvail / maxFwd;           // fill the height with the road ahead
  float sx = hAvail / maxAbsX;
  if (sx < target) target = sx;             // but don't let width overflow
  if (target > MAP_MAXSCALE) target = MAP_MAXSCALE;

  if (!scaleInit) { vScale = target; scaleInit = true; }   // snap on first frame
  else vScale += (target - vScale) * SCALE_EASE;           // then ease
}

// Rider origin [0,0] maps to (MAP_RCX, RIDER_Y); +forward = up, +x = right.
// Every point is clamped to stay INSIDE the map panel (inset by the line
// half-thickness) so nothing — including line width — spills out of the box.
// Clamping points keeps whole segments inside because the panel is a rectangle.
#define MAP_INSET 10   // >= half the thickest line (casing) so width stays inside
static inline lv_coord_t clampi(lv_coord_t v, lv_coord_t lo, lv_coord_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline lv_coord_t fitX(int m) {
  return clampi((lv_coord_t)(MAP_RCX + m * vScale), MAP_X0 + MAP_INSET, MAP_X1 - MAP_INSET);
}
static inline lv_coord_t fitY(int m) {
  return clampi((lv_coord_t)(RIDER_Y - m * vScale), MAP_Y0 + MAP_INSET, MAP_Y1 - MAP_INSET);
}

// Build a heading-up navigation-arrow kite centred at (cx,cy), pointing up.
static void buildRiderArrow(lv_coord_t cx, lv_coord_t cy) {
  riderArrowPts[0] = { (lv_coord_t)(cx),      (lv_coord_t)(cy - 13) };  // tip
  riderArrowPts[1] = { (lv_coord_t)(cx + 11), (lv_coord_t)(cy + 10) };  // right
  riderArrowPts[2] = { (lv_coord_t)(cx),      (lv_coord_t)(cy + 4)  };  // notch
  riderArrowPts[3] = { (lv_coord_t)(cx - 11), (lv_coord_t)(cy + 10) };  // left
  riderArrowPts[4] = { (lv_coord_t)(cx),      (lv_coord_t)(cy - 13) };  // close
}

// Build the heading-up map: junction context roads (casing + surface) first,
// then the brand-colour route polyline, then the green navigation arrow.
static void rebuildMap(void) {
  // Hide everything, then re-show what we draw.
  lv_obj_add_flag(roadCasing, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(routeFill, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < MAX_JUNCTION_ROADS; i++) {
    lv_obj_add_flag(ghostCasing[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ghostLine[i], LV_OBJ_FLAG_HIDDEN);
  }

  if (routeLen < 2 && junctionRoadCount == 0) {
    lv_obj_add_flag(riderHalo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(riderArrowEdge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(riderArrow, LV_OBJ_FLAG_HIDDEN);
    scaleInit = false;   // snap the zoom fresh when a new route appears
    return;
  }

  computeFit();

  // Junction roads — casing + surface, faint context only.
  for (int r = 0; r < junctionRoadCount; r++) {
    int n = junctionRoadLen[r];
    if (n < 2) continue;
    for (int j = 0; j < n; j++) {
      jPts[r][j].x = fitX(junctionX[r][j]);
      jPts[r][j].y = fitY(junctionY[r][j]);
    }
    lv_line_set_points(ghostCasing[r], jPts[r], n);
    lv_line_set_points(ghostLine[r], jPts[r], n);
    lv_obj_clear_flag(ghostCasing[r], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ghostLine[r], LV_OBJ_FLAG_HIDDEN);
  }

  // Route polyline (dim-brand casing under, brand route over).
  if (routeLen >= 2) {
    routeN = routeLen;
    for (int i = 0; i < routeN; i++) {
      routePts[i].x = fitX(routeX[i]);
      routePts[i].y = fitY(routeY[i]);
    }
    lv_line_set_points(roadCasing, routePts, routeN);
    lv_line_set_points(routeFill, routePts, routeN);
    lv_obj_clear_flag(roadCasing, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(routeFill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(roadCasing);
    lv_obj_move_foreground(routeFill);
  }

  // Navigation arrow at the rider origin [0,0], kept on top.
  lv_coord_t rx = fitX(0), ry = fitY(0);
  riderPulse.cx = rx; riderPulse.cy = ry;
  buildRiderArrow(rx, ry);
  lv_line_set_points(riderArrowEdge, riderArrowPts, 5);
  lv_line_set_points(riderArrow, riderArrowPts, 5);
  lv_obj_clear_flag(riderHalo, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(riderArrowEdge, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(riderArrow, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(riderHalo);
  lv_obj_move_foreground(riderArrowEdge);
  lv_obj_move_foreground(riderArrow);
}

void ui_update(void) {
  // The call toast floats on the top layer over whatever screen is showing,
  // so navigation keeps running underneath it.
  updateToast();

  if (navActive && navArrived) {
    lv_label_set_text(lblDoneDest, tripDestination.c_str());
    String stats = fmtDist(tripTotalDist) + "  -  " + fmtTime(tripTotalDur);
    lv_label_set_text(lblDoneStats, stats.c_str());
    showScreen(SC_DONE);
    return;
  }

  if (!navActive) {
    lv_label_set_text(homeStatus, "Ready to ride");
    routeN = 0;
    showScreen(SC_HOME);
    return;
  }

  showScreen(SC_NAV);

  lv_label_set_text(lblArrow, arrowFor(navInstruction));
  lv_label_set_text(lblTurnDist, fmtDist(navDistanceToTurn).c_str());
  lv_label_set_text(lblInstr, navInstruction.c_str());

  int pct = 0;
  if (tripTotalDist > 0) {
    pct = (int)(100.0 * (tripTotalDist - navRemainingDist) / tripTotalDist);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  }
  lv_bar_set_value(progressBar, pct, LV_ANIM_ON);

  rebuildMap();

  lv_label_set_text(lblDest, tripDestination.c_str());
  String trip = fmtDist(navRemainingDist) + "  -  " + fmtTime(navRemainingTime);
  lv_label_set_text(lblTrip, trip.c_str());
}
