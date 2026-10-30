#include "ui.h"
#include <lvgl.h>
#include <math.h>
#include <time.h>
#include "board.h"
#include "state.h"

LV_FONT_DECLARE(font_num104);
LV_FONT_DECLARE(font_num56);
LV_FONT_DECLARE(font_bold44);
LV_FONT_DECLARE(font_bold34);
LV_FONT_DECLARE(font_semi23);
LV_FONT_DECLARE(font_semi18);

#define SCR   466
#define MAP_RX 233          // rider arrow position on the Nav page
#define MAP_RY 252
#define MAP_BOTTOM 330      // the map fades out above the turn readout

// ── palette ──────────────────────────────────────────────
static lv_color_t C_MINT, C_CASING, C_TEXT, C_DIM, C_CALL, C_CALL2, C_WARN, C_SURF, C_BLACK, C_GREEN;
struct MapPalette {
  lv_color_t minor, major, casing, route, rider;
  uint8_t minorW, majorW, casingW, routeW;
  lv_color_t dim;
};
static MapPalette PAL[2];
static const MapPalette *pal = &PAL[0];

// ── objects ──────────────────────────────────────────────
static lv_obj_t *tv, *tileHome, *tileNav, *tileRide;
static lv_obj_t *statusRow, *lblBle, *lblBatt, *dots[3];
static lv_obj_t *homeRing, *lblBrand, *lblClock, *lblDate, *lblHint;
static lv_obj_t *mapObj, *turnIcon, *lblDist, *lblUnit, *lblStreet, *lblEta, *progArc, *lblIdle, *navLive;
static lv_obj_t *lblSpeed, *lblSpeedUnit, *speedArc, *lblLean, *lblTrip, *lblTime;
static lv_obj_t *callOv, *callRing[2], *lblCallInit, *lblCallName, *lblCallNum, *callPill, *lblCallPill;
static lv_obj_t *crashOv, *crashArc, *lblCrashTitle, *lblCrashNum, *lblCrashSub, *btnCrashOk, *lblCrashOk;
static lv_obj_t *doneOv, *lblDoneDest, *lblDoneStats;

static uint32_t doneUntilMs = 0, wakeMs = 0;
static bool linkUp = false;
static float tripKm = 0;
static uint32_t lastUpdateMs = 0;

// ── helpers ──────────────────────────────────────────────
static lv_obj_t *mkBox(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, int y, int w = 300) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(l, w);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  lv_label_set_text(l, "");
  return l;
}

static void setText(lv_obj_t *l, const char *t) {   // skip redraws when nothing changed
  if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static lv_obj_t *mkCircle(lv_obj_t *parent, int d, lv_color_t bg) {
  lv_obj_t *o = mkBox(parent);
  lv_obj_set_size(o, d, d);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

static lv_obj_t *mkArc(lv_obj_t *parent, int d, int w, int a0, int a1, lv_color_t bg, lv_color_t fg) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_remove_style_all(a);
  lv_obj_set_size(a, d, d);
  lv_obj_center(a);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_arc_set_bg_angles(a, a0, a1);
  lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, bg, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, fg, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
  return a;
}

static void fmtDist(long m, char *num, char *unit) {
  if (m >= 1000) { snprintf(num, 12, "%.1f", m / 1000.0); strcpy(unit, "km"); }
  else { snprintf(num, 12, "%ld", m >= 100 ? (m + 5) / 10 * 10 : max(0L, (m + 2) / 5 * 5)); strcpy(unit, "m"); }
}

static bool localTime(struct tm &out) {
  int64_t ms = clock_epoch_ms();
  if (ms <= 0) return false;
  time_t s = (time_t)(ms / 1000) + settings.tzOffsetMin * 60;
  gmtime_r(&s, &out);
  return true;
}

// ── turn arrow (drawn, scales cleanly) ───────────────────
static void lineSeg(lv_draw_ctx_t *ctx, lv_draw_line_dsc_t *d, int x1, int y1, int x2, int y2) {
  lv_point_t a = {(lv_coord_t)x1, (lv_coord_t)y1}, b = {(lv_coord_t)x2, (lv_coord_t)y2};
  lv_draw_line(ctx, d, &a, &b);
}

static void tri(lv_draw_ctx_t *ctx, lv_draw_rect_dsc_t *d, float x1, float y1, float x2, float y2, float x3, float y3) {
  lv_point_t p[3] = {{(lv_coord_t)lroundf(x1), (lv_coord_t)lroundf(y1)},
                     {(lv_coord_t)lroundf(x2), (lv_coord_t)lroundf(y2)},
                     {(lv_coord_t)lroundf(x3), (lv_coord_t)lroundf(y3)}};
  lv_draw_polygon(ctx, d, p, 3);
}

static int turnAngle(const char *ins) {
  if (!strcmp(ins, "LEFT")) return -90;
  if (!strcmp(ins, "RIGHT")) return 90;
  if (!strcmp(ins, "SLIGHT_LEFT")) return -45;
  if (!strcmp(ins, "SLIGHT_RIGHT")) return 45;
  if (!strcmp(ins, "SHARP_LEFT")) return -135;
  if (!strcmp(ins, "SHARP_RIGHT")) return 135;
  return 0;
}

static void turnIconDraw(lv_event_t *e) {
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_obj_t *o = lv_event_get_target(e);
  const float k = lv_obj_get_width(o) / 48.0f;          // geometry is on a 48-unit grid
  const int ox = o->coords.x1, oy = o->coords.y1;
  auto X = [&](float v) { return (int)lroundf(ox + v * k); };
  auto Y = [&](float v) { return (int)lroundf(oy + v * k); };

  lv_draw_line_dsc_t ld;
  lv_draw_line_dsc_init(&ld);
  ld.color = C_MINT;
  ld.width = (lv_coord_t)lroundf(4.6f * k);
  ld.round_start = ld.round_end = 1;
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.bg_color = C_MINT;
  rd.bg_opa = LV_OPA_COVER;

  const char *ins = nav.instruction;
  if (!strcmp(ins, "ARRIVE")) {
    lineSeg(ctx, &ld, X(24), Y(44), X(24), Y(8));
    tri(ctx, &rd, X(24), Y(7), X(40), Y(13), X(24), Y(20));
    return;
  }
  if (!strcmp(ins, "UTURN")) {
    lineSeg(ctx, &ld, X(32), Y(44), X(32), Y(20));
    for (int i = 0; i < 6; i++) {                       // half circle over the top
      float a0 = (float)M_PI * i / 6, a1 = (float)M_PI * (i + 1) / 6;
      lineSeg(ctx, &ld, X(24 + 8 * cosf(a0)), Y(20 - 8 * sinf(a0)), X(24 + 8 * cosf(a1)), Y(20 - 8 * sinf(a1)));
    }
    lineSeg(ctx, &ld, X(16), Y(20), X(16), Y(30));
    tri(ctx, &rd, X(7), Y(29), X(25), Y(29), X(16), Y(41));
    return;
  }
  if (!strcmp(ins, "ROUNDABOUT")) {
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = C_MINT;
    ad.width = ld.width;
    lv_point_t c = {(lv_coord_t)X(24), (lv_coord_t)Y(22)};
    lv_draw_arc(ctx, &ad, &c, (uint16_t)lroundf(9 * k), 0, 360);
    lineSeg(ctx, &ld, X(24), Y(44), X(24), Y(31));
    lineSeg(ctx, &ld, X(30), Y(16), X(37), Y(9));
    tri(ctx, &rd, X(42), Y(4), X(32), Y(6), X(40), Y(14));
    return;
  }
  float a = turnAngle(ins) * (float)M_PI / 180.0f;
  bool straight = a == 0;
  float jy = straight ? 10 : 24, len = straight ? 0 : 16;
  float ex = 24 + len * sinf(a), ey = jy - len * cosf(a);
  lineSeg(ctx, &ld, X(24), Y(44), X(24), Y(jy));
  if (!straight) lineSeg(ctx, &ld, X(24), Y(jy), X(ex), Y(ey));
  float hx = sinf(a), hy = -cosf(a), px = -hy, py = hx;
  tri(ctx, &rd, X(ex + hx * 9), Y(ey + hy * 9), X(ex + px * 8.5f), Y(ey + py * 8.5f), X(ex - px * 8.5f), Y(ey - py * 8.5f));
}

// ── map projection (fixed point — the C6 has no FPU) ─────
struct Pose { float x, y, h; };
static Pose pose = {0, 0, 0};
static bool poseInit = false;
static float zoom = 0.8f;                   // px per metre
static int32_t pc, ps, pk, px16, py16;      // cos/sin·4096, zoom·1024, pose·16

static int16_t aSx[MAX_AREA_ROADS][MAX_ROAD_PTS], aSy[MAX_AREA_ROADS][MAX_ROAD_PTS];
static lv_area_t aBox[MAX_AREA_ROADS];
static bool aVis[MAX_AREA_ROADS];
static int16_t jSx[MAX_JROADS][MAX_ROAD_PTS], jSy[MAX_JROADS][MAX_ROAD_PTS];
static bool jVis[MAX_JROADS];
static int16_t rSx[MAX_ROUTE_PTS + 1], rSy[MAX_ROUTE_PTS + 1];
static int rN = 0;
static lv_point_t headPts[3];
static bool headOk = false;

static inline void project(int32_t mx16, int32_t my16, int16_t &sx, int16_t &sy) {
  int32_t dx = mx16 - px16, dy = my16 - py16;            // 1/16 m
  int32_t lx = (dx * pc - dy * ps) >> 12;
  int32_t ly = (dx * ps + dy * pc) >> 12;
  int32_t X = MAP_RX + ((lx * pk) >> 14);
  int32_t Y = MAP_RY - ((ly * pk) >> 14);
  sx = (int16_t)constrain(X, -2000, 2000);
  sy = (int16_t)constrain(Y, -2000, 2000);
}

static void projectRoad(const Road &r, int16_t *sx, int16_t *sy, lv_area_t &box, bool &vis) {
  box = {32000, 32000, -32000, -32000};
  for (int i = 0; i < r.n; i++) {
    project((int32_t)r.x[i] * 16, (int32_t)r.y[i] * 16, sx[i], sy[i]);
    box.x1 = min<lv_coord_t>(box.x1, sx[i]);
    box.y1 = min<lv_coord_t>(box.y1, sy[i]);
    box.x2 = max<lv_coord_t>(box.x2, sx[i]);
    box.y2 = max<lv_coord_t>(box.y2, sy[i]);
  }
  vis = box.x2 >= -20 && box.x1 <= SCR + 20 && box.y2 >= -20 && box.y1 <= MAP_BOTTOM + 20;
}

static void projectAll() {
  float h = pose.h * (float)M_PI / 180.0f;
  pc = (int32_t)lroundf(cosf(h) * 4096);
  ps = (int32_t)lroundf(sinf(h) * 4096);
  pk = (int32_t)lroundf(zoom * 1024);
  px16 = (int32_t)lroundf(pose.x * 16);
  py16 = (int32_t)lroundf(pose.y * 16);
  if (nav.v2) {
    for (int i = 0; i < area.n; i++) projectRoad(area.roads[i], aSx[i], aSy[i], aBox[i], aVis[i]);
  }
  for (int i = 0; i < jroads.n; i++) {
    lv_area_t b;
    projectRoad(jroads.roads[i], jSx[i], jSy[i], b, jVis[i]);
  }
  // route ahead, starting at the rider
  rN = 0;
  rSx[rN] = MAP_RX; rSy[rN] = MAP_RY; rN++;
  for (int i = 1; i < nav.routeN && rN <= MAX_ROUTE_PTS; i++, rN++)
    project((int32_t)lroundf(nav.routeX[i] * 16), (int32_t)lroundf(nav.routeY[i] * 16), rSx[rN], rSy[rN]);
  headOk = false;
  if (rN >= 2) {
    float dx = rSx[rN - 1] - rSx[rN - 2], dy = rSy[rN - 1] - rSy[rN - 2];
    float l = sqrtf(dx * dx + dy * dy);
    if (l > 0.5f) {
      float ux = dx / l, uy = dy / l, bx = rSx[rN - 1], by = rSy[rN - 1];
      headPts[0] = {(lv_coord_t)lroundf(bx + ux * 18), (lv_coord_t)lroundf(by + uy * 18)};
      headPts[1] = {(lv_coord_t)lroundf(bx - ux * 9 - uy * 15), (lv_coord_t)lroundf(by - uy * 9 + ux * 15)};
      headPts[2] = {(lv_coord_t)lroundf(bx - ux * 9 + uy * 15), (lv_coord_t)lroundf(by - uy * 9 - ux * 15)};
      headOk = true;
    }
  }
}

static bool segHits(const lv_area_t *clip, int ox, int oy, int x1, int y1, int x2, int y2, int pad) {
  return !(max(x1, x2) + ox + pad < clip->x1 || min(x1, x2) + ox - pad > clip->x2 ||
           max(y1, y2) + oy + pad < clip->y1 || min(y1, y2) + oy - pad > clip->y2);
}

static void drawPolyline(lv_draw_ctx_t *ctx, lv_draw_line_dsc_t *d, const int16_t *sx, const int16_t *sy, int n,
                         int ox, int oy) {
  for (int i = 1; i < n; i++) {
    if (!segHits(ctx->clip_area, ox, oy, sx[i - 1], sy[i - 1], sx[i], sy[i], d->width)) continue;
    lv_point_t a = {(lv_coord_t)(sx[i - 1] + ox), (lv_coord_t)(sy[i - 1] + oy)};
    lv_point_t b = {(lv_coord_t)(sx[i] + ox), (lv_coord_t)(sy[i] + oy)};
    lv_draw_line(ctx, d, &a, &b);
  }
}

static void drawMapLayer(lv_draw_ctx_t *ctx, int ox, int oy, lv_opa_t opa) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.round_start = d.round_end = 1;
  d.opa = opa;
  for (int w = 1; w <= 2; w++) {
    d.color = w == 1 ? pal->minor : pal->major;
    d.width = w == 1 ? pal->minorW : pal->majorW;
    if (nav.v2) {
      for (int i = 0; i < area.n; i++) {
        if (!aVis[i] || area.roads[i].w != w) continue;
        lv_area_t b = aBox[i];
        lv_area_move(&b, ox, oy);
        lv_area_increase(&b, d.width, d.width);
        if (!_lv_area_is_on(&b, ctx->clip_area)) continue;
        drawPolyline(ctx, &d, aSx[i], aSy[i], area.roads[i].n, ox, oy);
      }
    }
    if (w == 1) for (int i = 0; i < jroads.n; i++) if (jVis[i]) drawPolyline(ctx, &d, jSx[i], jSy[i], jroads.roads[i].n, ox, oy);
  }
  if (rN >= 2) {
    d.color = pal->casing; d.width = pal->casingW;
    drawPolyline(ctx, &d, rSx, rSy, rN, ox, oy);
    d.color = pal->route; d.width = pal->routeW;
    drawPolyline(ctx, &d, rSx, rSy, rN, ox, oy);
    if (headOk) {
      lv_draw_rect_dsc_t r;
      lv_draw_rect_dsc_init(&r);
      r.bg_color = pal->route;
      r.bg_opa = opa;
      lv_point_t p[3];
      for (int i = 0; i < 3; i++) p[i] = {(lv_coord_t)(headPts[i].x + ox), (lv_coord_t)(headPts[i].y + oy)};
      lv_draw_polygon(ctx, &r, p, 3);
    }
  }
}

static void mapDraw(lv_event_t *e) {
  if (!nav.active || nav.routeN == 0) return;
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_obj_t *o = lv_event_get_target(e);
  const int ox = o->coords.x1, oy = o->coords.y1;
  const lv_area_t *orig = ctx->clip_area;
  // Fade the map out above the turn readout with a few opacity bands (cheap, no masks).
  static const struct { int16_t y1, y2; lv_opa_t opa; } bands[] = {
      {0, 269, LV_OPA_COVER}, {270, 287, 175}, {288, 307, 100}, {308, MAP_BOTTOM, 40}};
  for (auto &b : bands) {
    lv_area_t clip = {(lv_coord_t)ox, (lv_coord_t)(oy + b.y1), (lv_coord_t)(ox + SCR - 1), (lv_coord_t)(oy + b.y2)};
    if (!_lv_area_intersect(&clip, &clip, orig)) continue;
    ctx->clip_area = &clip;
    drawMapLayer(ctx, ox, oy, b.opa);
  }
  ctx->clip_area = orig;

  // rider: two triangles (LVGL polygons must be convex), black outline first
  lv_draw_rect_dsc_t r;
  lv_draw_rect_dsc_init(&r);
  r.bg_opa = LV_OPA_COVER;
  const float cx = ox + MAP_RX, cy = oy + MAP_RY;
  for (int pass = 0; pass < 2; pass++) {
    float s = pass == 0 ? 1.3f : 1.0f;
    r.bg_color = pass == 0 ? C_BLACK : pal->rider;
    tri(ctx, &r, cx, cy - 17 * s, cx, cy + 4 * s, cx - 13 * s, cy + 11 * s);
    tri(ctx, &r, cx, cy - 17 * s, cx + 13 * s, cy + 11 * s, cx, cy + 4 * s);
  }
}

// Ease the pose toward the latest frame; only redraw while something moves.
static void mapAnimate(float dt) {
  if (!nav.active || nav.routeN == 0) return;
  float e = min(1.0f, dt * 7.0f);
  if (!poseInit) { pose = {nav.riderX, nav.riderY, nav.heading}; poseInit = true; e = 1; }
  float zt = constrain(210.0f / (nav.distanceToTurn + 45.0f), 0.55f, 1.8f);
  float dh = fmodf(nav.heading - pose.h + 540.0f, 360.0f) - 180.0f;
  float dx = nav.riderX - pose.x, dy = nav.riderY - pose.y, dz = zt - zoom;
  bool moving = fabsf(dh) > 0.3f || fabsf(dx) > 0.2f || fabsf(dy) > 0.2f || fabsf(dz) > 0.005f;
  static uint32_t seenSeq = 0;
  if (!moving && seenSeq == nav.seq) return;
  seenSeq = nav.seq;
  if (nav.v2) { pose.x += dx * e; pose.y += dy * e; }
  else { pose.x = nav.riderX; pose.y = nav.riderY; }    // v1: every frame re-centres on the rider
  pose.h = fmodf(pose.h + dh * e + 360.0f, 360.0f);
  zoom += dz * min(1.0f, dt * 2.5f);
  projectAll();
  lv_area_t a = {0, 0, SCR - 1, MAP_BOTTOM};
  lv_area_move(&a, mapObj->coords.x1, mapObj->coords.y1);
  lv_obj_invalidate_area(mapObj, &a);
}

// ── pages ────────────────────────────────────────────────
static void buildHome() {
  homeRing = mkBox(tileHome);
  lv_obj_set_size(homeRing, 440, 440);   // hugs the bezel so the clock gets the full width
  lv_obj_center(homeRing);
  lv_obj_set_style_radius(homeRing, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(homeRing, 3, 0);
  lv_obj_set_style_border_color(homeRing, C_MINT, 0);
  lv_obj_set_style_border_opa(homeRing, LV_OPA_40, 0);

  lblBrand = mkLabel(tileHome, &font_semi18, C_MINT, 80);
  lv_obj_set_style_text_letter_space(lblBrand, 6, 0);
  lv_label_set_text(lblBrand, "TRAKIO");
  lblClock = mkLabel(tileHome, &font_num104, C_TEXT, 134, 400);
  lblDate = mkLabel(tileHome, &font_semi23, C_DIM, 262, 340);
  lblHint = mkLabel(tileHome, &font_semi18, C_DIM, 336, 300);
}

static void buildNav() {
  mapObj = mkBox(tileNav);
  lv_obj_set_size(mapObj, SCR, SCR);
  lv_obj_add_event_cb(mapObj, mapDraw, LV_EVENT_DRAW_MAIN, nullptr);

  navLive = mkBox(tileNav);
  lv_obj_set_size(navLive, SCR, SCR);

  progArc = mkArc(navLive, 440, 7, 35, 145, C_SURF, C_TEXT);
  lv_arc_set_mode(progArc, LV_ARC_MODE_REVERSE);
  lv_arc_set_range(progArc, 0, 1000);

  lv_obj_t *row = mkBox(navLive);
  lv_obj_set_size(row, LV_SIZE_CONTENT, 76);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 284);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(row, 10, 0);
  turnIcon = mkBox(row);
  lv_obj_set_size(turnIcon, 64, 64);
  lv_obj_set_style_translate_y(turnIcon, -10, 0);   // sit on the numerals' baseline
  lv_obj_add_event_cb(turnIcon, turnIconDraw, LV_EVENT_DRAW_MAIN, nullptr);
  lblDist = lv_label_create(row);
  lv_obj_set_style_text_font(lblDist, &font_num56, 0);
  lv_obj_set_style_text_color(lblDist, C_TEXT, 0);
  lblUnit = lv_label_create(row);
  lv_obj_set_style_text_font(lblUnit, &font_semi23, 0);
  lv_obj_set_style_text_color(lblUnit, C_DIM, 0);
  lv_obj_set_style_pad_bottom(lblUnit, 9, 0);

  lblStreet = mkLabel(navLive, &font_semi23, C_TEXT, 362, 330);
  lblEta = mkLabel(navLive, &font_semi23, C_DIM, 390, 290);

  lblIdle = mkLabel(tileNav, &font_semi23, C_DIM, 180, 340);
  lv_label_set_long_mode(lblIdle, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lblIdle, "No route yet.\nStart a ride on your phone.");
}

static void buildRide() {
  speedArc = mkArc(tileRide, 448, 14, 135, 45, lv_color_hex(0x11182A), C_MINT);   // at the bezel, clear of the status row
  lv_arc_set_range(speedArc, 0, 60);
  lblSpeed = mkLabel(tileRide, &font_num104, C_TEXT, 102, 340);
  lblSpeedUnit = mkLabel(tileRide, &font_semi23, C_DIM, 220);
  lv_label_set_text(lblSpeedUnit, "km/h");

  const char *caps[3] = {"LEAN", "KM", "TIME"};
  lv_obj_t **vals[3] = {&lblLean, &lblTrip, &lblTime};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *col = mkBox(tileRide);
    lv_obj_set_size(col, 110, 80);
    lv_obj_align(col, LV_ALIGN_TOP_MID, (i - 1) * 118, 282);   // clear of the speed arc's ends
    *vals[i] = mkLabel(col, &font_bold34, C_TEXT, 0, 110);
    lv_obj_t *c = mkLabel(col, &font_semi18, C_DIM, 46, 110);
    lv_obj_set_style_text_letter_space(c, 2, 0);
    lv_label_set_text(c, caps[i]);
  }
}

// ── overlays ─────────────────────────────────────────────
static void ringAnim(void *o, int32_t v) {
  lv_obj_set_style_transform_width((lv_obj_t *)o, v, 0);
  lv_obj_set_style_transform_height((lv_obj_t *)o, v, 0);
  lv_obj_set_style_border_opa((lv_obj_t *)o, (lv_opa_t)map(v, -40, 30, 230, 0), 0);
}

static void onCallBtn(lv_event_t *e) { app_call_button((bool)(intptr_t)lv_event_get_user_data(e)); }
static void onCrashOk(lv_event_t *) { app_crash_ok(); }

static lv_obj_t *mkRoundBtn(lv_obj_t *parent, lv_color_t bg, const char *sym, lv_color_t fg, int x) {
  lv_obj_t *b = mkCircle(parent, 80, bg);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(b, 12);
  lv_obj_align(b, LV_ALIGN_TOP_MID, x, 348);
  lv_obj_set_style_transform_width(b, -3, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(b, -3, LV_STATE_PRESSED);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_36, 0);
  lv_obj_set_style_text_color(l, fg, 0);
  lv_label_set_text(l, sym);
  lv_obj_center(l);
  return b;
}

static void buildOverlays() {
  lv_obj_t *top = lv_layer_top();

  // incoming call — full screen, made for the circle
  callOv = mkBox(top);
  lv_obj_set_size(callOv, SCR, SCR);
  lv_obj_add_flag(callOv, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(callOv, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(callOv, lv_color_hex(0x3A0A12), 0);
  lv_obj_set_style_bg_grad_color(callOv, C_BLACK, 0);
  lv_obj_set_style_bg_grad_dir(callOv, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_main_stop(callOv, 40, 0);
  lv_obj_set_style_bg_grad_stop(callOv, 200, 0);
  lv_obj_t *lab = mkLabel(callOv, &font_semi18, C_CALL, 56);
  lv_obj_set_style_text_letter_space(lab, 4, 0);
  lv_label_set_text(lab, "INCOMING CALL");
  for (int i = 0; i < 2; i++) {
    callRing[i] = mkBox(callOv);
    lv_obj_set_size(callRing[i], 184, 184);
    lv_obj_align(callRing[i], LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_radius(callRing[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(callRing[i], 3, 0);
    lv_obj_set_style_border_color(callRing[i], C_CALL, 0);
  }
  lv_obj_t *av = mkCircle(callOv, 140, C_CALL);
  lv_obj_set_style_bg_grad_color(av, C_CALL2, 0);
  lv_obj_set_style_bg_grad_dir(av, LV_GRAD_DIR_VER, 0);
  lv_obj_align(av, LV_ALIGN_TOP_MID, 0, 110);
  lblCallInit = lv_label_create(av);
  lv_obj_set_style_text_font(lblCallInit, &font_bold44, 0);
  lv_obj_set_style_text_color(lblCallInit, C_TEXT, 0);
  lv_obj_center(lblCallInit);
  lblCallName = mkLabel(callOv, &font_bold34, C_TEXT, 268, 360);
  lblCallNum = mkLabel(callOv, &font_semi23, C_DIM, 310, 320);
  lv_obj_t *no = mkRoundBtn(callOv, C_CALL, LV_SYMBOL_CLOSE, C_TEXT, -64);
  lv_obj_add_event_cb(no, onCallBtn, LV_EVENT_CLICKED, (void *)0);
  lv_obj_t *yes = mkRoundBtn(callOv, C_MINT, LV_SYMBOL_OK, C_BLACK, 64);
  lv_obj_add_event_cb(yes, onCallBtn, LV_EVENT_CLICKED, (void *)1);
  lv_obj_add_flag(callOv, LV_OBJ_FLAG_HIDDEN);

  // live call pill
  callPill = mkBox(top);
  lv_obj_set_size(callPill, LV_SIZE_CONTENT, 46);
  lv_obj_align(callPill, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_set_style_radius(callPill, 23, 0);
  lv_obj_set_style_bg_opa(callPill, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(callPill, lv_color_hex(0x1FBF6F), 0);
  lv_obj_set_style_bg_grad_color(callPill, lv_color_hex(0x0E7A45), 0);
  lv_obj_set_style_bg_grad_dir(callPill, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_pad_hor(callPill, 18, 0);
  lv_obj_set_flex_flow(callPill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(callPill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(callPill, 8, 0);
  lv_obj_t *ic = lv_label_create(callPill);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(ic, C_TEXT, 0);
  lv_label_set_text(ic, LV_SYMBOL_CALL);
  lblCallPill = lv_label_create(callPill);
  lv_obj_set_style_text_font(lblCallPill, &font_semi18, 0);
  lv_obj_set_style_text_color(lblCallPill, C_TEXT, 0);
  lv_obj_add_flag(callPill, LV_OBJ_FLAG_HIDDEN);

  // crash countdown
  crashOv = mkBox(top);
  lv_obj_set_size(crashOv, SCR, SCR);
  lv_obj_add_flag(crashOv, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(crashOv, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(crashOv, C_BLACK, 0);
  crashArc = mkArc(crashOv, 440, 14, 270, 269, lv_color_hex(0x2A0D12), C_CALL);
  lv_arc_set_rotation(crashArc, 0);
  lv_arc_set_bg_angles(crashArc, 0, 360);
  lv_arc_set_rotation(crashArc, 270);
  lv_arc_set_range(crashArc, 0, 10);
  lblCrashTitle = mkLabel(crashOv, &font_semi23, C_CALL, 84);
  lv_obj_set_style_text_letter_space(lblCrashTitle, 2, 0);
  lblCrashNum = mkLabel(crashOv, &font_num104, C_TEXT, 122, 300);
  lblCrashSub = mkLabel(crashOv, &font_semi18, C_DIM, 258, 320);
  btnCrashOk = mkBox(crashOv);
  lv_obj_add_flag(btnCrashOk, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(btnCrashOk, 16);
  lv_obj_set_size(btnCrashOk, 220, 70);
  lv_obj_align(btnCrashOk, LV_ALIGN_TOP_MID, 0, 304);
  lv_obj_set_style_radius(btnCrashOk, 35, 0);
  lv_obj_set_style_bg_opa(btnCrashOk, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(btnCrashOk, C_TEXT, 0);
  lv_obj_add_event_cb(btnCrashOk, onCrashOk, LV_EVENT_CLICKED, nullptr);
  lblCrashOk = lv_label_create(btnCrashOk);
  lv_obj_set_style_text_font(lblCrashOk, &font_semi23, 0);
  lv_obj_set_style_text_color(lblCrashOk, C_BLACK, 0);
  lv_obj_center(lblCrashOk);
  lv_obj_add_flag(crashOv, LV_OBJ_FLAG_HIDDEN);

  // arrival
  doneOv = mkBox(top);
  lv_obj_set_size(doneOv, SCR, SCR);
  lv_obj_set_style_bg_opa(doneOv, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(doneOv, lv_color_hex(0x0A2A22), 0);
  lv_obj_set_style_bg_grad_color(doneOv, C_BLACK, 0);
  lv_obj_set_style_bg_grad_dir(doneOv, LV_GRAD_DIR_VER, 0);
  lv_obj_t *tick = mkCircle(doneOv, 128, C_MINT);
  lv_obj_align(tick, LV_ALIGN_TOP_MID, 0, 74);
  lv_obj_t *tl = lv_label_create(tick);
  lv_obj_set_style_text_font(tl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(tl, lv_color_hex(0x06231A), 0);
  lv_label_set_text(tl, LV_SYMBOL_OK);
  lv_obj_center(tl);
  lv_obj_t *hd = mkLabel(doneOv, &font_bold44, C_TEXT, 216);
  lv_label_set_text(hd, "Arrived");
  lblDoneDest = mkLabel(doneOv, &font_semi23, C_MINT, 284, 340);
  lblDoneStats = mkLabel(doneOv, &font_semi18, C_DIM, 324, 300);
  lv_obj_add_flag(doneOv, LV_OBJ_FLAG_HIDDEN);
}

// ── status + dots ────────────────────────────────────────
static void onTileChanged(lv_event_t *) {
  lv_obj_t *t = lv_tileview_get_tile_act(tv);
  int idx = t == tileHome ? 0 : t == tileNav ? 1 : 2;
  for (int i = 0; i < 3; i++) {
    lv_obj_set_width(dots[i], i == idx ? 18 : 7);
    lv_obj_set_style_bg_color(dots[i], i == idx ? C_MINT : lv_color_hex(0x2A3145), 0);
  }
}

static int activeTile() {
  lv_obj_t *t = lv_tileview_get_tile_act(tv);
  return t == tileHome ? 0 : t == tileNav ? 1 : 2;
}

static void showTile(int idx) {
  lv_obj_set_tile_id(tv, idx, 0, LV_ANIM_ON);
  onTileChanged(nullptr);
}

static void applyTheme() {
  pal = &PAL[settings.theme == THEME_DAY ? 1 : 0];
  C_DIM = pal->dim;
  lv_obj_t *dimLabels[] = {lblDate, lblHint, lblUnit, lblEta, lblIdle, lblSpeedUnit, lblBatt, lblCallNum, lblCrashSub, lblDoneStats};
  for (lv_obj_t *l : dimLabels) lv_obj_set_style_text_color(l, C_DIM, 0);
  lv_obj_set_style_arc_color(progArc, settings.theme == THEME_DAY ? lv_color_hex(0x2B3858) : C_SURF, LV_PART_MAIN);
  if (poseInit) projectAll();
  lv_obj_invalidate(lv_scr_act());
}

// ── public ───────────────────────────────────────────────
void ui_init() {
  C_MINT = lv_color_hex(0x12E29C);
  C_CASING = lv_color_hex(0x0C3D31);
  C_TEXT = lv_color_hex(0xF5F7FA);
  C_DIM = lv_color_hex(0x7A869A);
  C_CALL = lv_color_hex(0xFF4D5E);
  C_CALL2 = lv_color_hex(0xB91D33);
  C_WARN = lv_color_hex(0xFFB020);
  C_SURF = lv_color_hex(0x1B2336);
  C_BLACK = lv_color_hex(0x000000);
  C_GREEN = lv_color_hex(0x1FBF6F);
  PAL[0] = {lv_color_hex(0x2E3750), lv_color_hex(0x4B5674), C_CASING, C_MINT, C_TEXT, 4, 7, 22, 14, lv_color_hex(0x7A869A)};
  PAL[1] = {lv_color_hex(0x7D879F), lv_color_hex(0xC4CAD8), lv_color_hex(0xFFFFFF), C_MINT, lv_color_hex(0xFFFFFF), 5, 8, 22, 15, lv_color_hex(0xC9D1DE)};

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C_BLACK, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  tv = lv_tileview_create(scr);
  lv_obj_set_size(tv, SCR, SCR);
  lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
  tileHome = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
  tileNav = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);
  tileRide = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);
  lv_obj_add_event_cb(tv, onTileChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  buildHome();
  buildNav();
  buildRide();

  statusRow = mkBox(scr);
  lv_obj_set_size(statusRow, LV_SIZE_CONTENT, 28);
  lv_obj_align(statusRow, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_set_flex_flow(statusRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(statusRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(statusRow, 8, 0);
  lblBle = lv_label_create(statusRow);
  lv_obj_set_style_text_font(lblBle, &lv_font_montserrat_20, 0);
  lv_label_set_text(lblBle, LV_SYMBOL_BLUETOOTH);
  lblBatt = lv_label_create(statusRow);
  lv_obj_set_style_text_font(lblBatt, &font_semi18, 0);
  lv_label_set_text(lblBatt, "");

  lv_obj_t *dotRow = mkBox(scr);
  lv_obj_set_size(dotRow, LV_SIZE_CONTENT, 8);
  lv_obj_align(dotRow, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_set_flex_flow(dotRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(dotRow, 7, 0);
  for (int i = 0; i < 3; i++) {
    dots[i] = mkBox(dotRow);
    lv_obj_set_size(dots[i], 7, 7);
    lv_obj_set_style_radius(dots[i], 4, 0);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
  }

  buildOverlays();
  applyTheme();
  ui_set_link(false);
  onTileChanged(nullptr);
  ui_on_events(EV_NAV);
  wakeMs = millis();
}

void ui_set_link(bool up) {
  linkUp = up;
  lv_obj_set_style_text_color(lblBle, up ? C_MINT : lv_color_hex(0x3A4258), 0);
}

void ui_set_battery(int pct, bool usb) {
  char b[16];
  snprintf(b, sizeof b, usb ? "USB" : "%d%%", pct);
  setText(lblBatt, b);
  lv_obj_set_style_text_color(lblBatt, !usb && pct <= 15 ? C_WARN : C_DIM, 0);
}

void ui_set_lean(float deg) {
  char b[16];
  int d = (int)lroundf(deg);
  snprintf(b, sizeof b, d == 0 ? "0°" : "%d°%s", abs(d), d > 0 ? "L" : "R");
  setText(lblLean, b);
}

void ui_note_wake() { wakeMs = millis(); }

bool ui_overlay_active() {
  return !lv_obj_has_flag(callOv, LV_OBJ_FLAG_HIDDEN) || !lv_obj_has_flag(crashOv, LV_OBJ_FLAG_HIDDEN) ||
         !lv_obj_has_flag(doneOv, LV_OBJ_FLAG_HIDDEN);
}

static void showCall() {
  bool ringing = call.phase == CALL_RINGING;
  if (ringing) {
    char init[2] = {call.caller[0] ? call.caller[0] : '?', 0};
    lv_label_set_text(lblCallInit, init);
    setText(lblCallName, call.caller);
    setText(lblCallNum, call.number);
    if (lv_obj_has_flag(callOv, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_clear_flag(callOv, LV_OBJ_FLAG_HIDDEN);
      for (int i = 0; i < 2; i++) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, callRing[i]);
        lv_anim_set_exec_cb(&a, ringAnim);
        lv_anim_set_values(&a, -40, 30);
        lv_anim_set_time(&a, 1600);
        lv_anim_set_delay(&a, i * 800);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
      }
    }
  } else if (!lv_obj_has_flag(callOv, LV_OBJ_FLAG_HIDDEN)) {
    for (int i = 0; i < 2; i++) lv_anim_del(callRing[i], ringAnim);
    lv_obj_add_flag(callOv, LV_OBJ_FLAG_HIDDEN);
  }
  if (call.phase == CALL_ACTIVE) lv_obj_clear_flag(callPill, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(callPill, LV_OBJ_FLAG_HIDDEN);
}

void ui_on_events(uint32_t ev) {
  if (ev & EV_THEME) applyTheme();
  if (ev & EV_ROUTE_START) {
    lv_obj_add_flag(doneOv, LV_OBJ_FLAG_HIDDEN);
    poseInit = false;
    tripKm = 0;
    showTile(1);
  }
  if (ev & EV_ARRIVED) {
    char stats[48];
    uint32_t sec = (millis() - nav.startedMs) / 1000;
    char n[12], u[4];
    fmtDist(nav.totalDist, n, u);
    snprintf(stats, sizeof stats, "%s %s · %lu:%02lu ride", n, u, (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    setText(lblDoneDest, nav.dest);
    setText(lblDoneStats, stats);
    lv_obj_clear_flag(doneOv, LV_OBJ_FLAG_HIDDEN);
    lv_obj_fade_in(doneOv, 300, 0);
    doneUntilMs = millis() + 7000;
  }
  if (ev & EV_ROUTE_END) {
    if (lv_obj_has_flag(doneOv, LV_OBJ_FLAG_HIDDEN)) showTile(0);
    lv_obj_invalidate(mapObj);
  }
  if (ev & (EV_NAV | EV_ROUTE_START | EV_ROUTE_END | EV_AREA)) {
    bool live = nav.active && nav.routeN > 0;
    if (live) lv_obj_clear_flag(navLive, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(navLive, LV_OBJ_FLAG_HIDDEN);
    if (live) lv_obj_add_flag(lblIdle, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(lblIdle, LV_OBJ_FLAG_HIDDEN);
    if (live) {
      char n[12], u[4], eta[48], buf[64];
      fmtDist(nav.distanceToTurn, n, u);
      setText(lblDist, n);
      setText(lblUnit, u);
      lv_obj_invalidate(turnIcon);
      setText(lblStreet, !strcmp(nav.instruction, "ARRIVE") ? nav.dest : nav.street);
      struct tm t;
      fmtDist(nav.remainingDist, n, u);
      if (nav.eta > 0 && clock_epoch_ms() > 0) {
        time_t s = (time_t)(nav.eta / 1000) + settings.tzOffsetMin * 60;
        gmtime_r(&s, &t);
        snprintf(eta, sizeof eta, "ETA %02d:%02d · %s %s", t.tm_hour, t.tm_min, n, u);
      } else {
        snprintf(eta, sizeof eta, "%ld min · %s %s", (nav.remainingTime + 59) / 60, n, u);
      }
      setText(lblEta, eta);
      long done = nav.totalDist > 0 ? 1000 - nav.remainingDist * 1000 / nav.totalDist : 0;
      lv_arc_set_value(progArc, (int16_t)constrain(done, 0L, 1000L));
      snprintf(buf, sizeof buf, "Riding to %s", nav.dest);
      setText(lblHint, buf);
    } else {
      setText(lblHint, linkUp ? "Ready to ride" : "Waiting for your phone");
    }
  }
  if (ev & EV_CALL) showCall();
}

void ui_crash_show(int left) {
  if (left < 0) {
    lv_obj_add_flag(crashOv, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  char b[8];
  snprintf(b, sizeof b, "%d", left);
  setText(lblCrashTitle, "CRASH DETECTED");
  setText(lblCrashNum, b);
  setText(lblCrashSub, "Sending SOS to your contacts");
  setText(lblCrashOk, "I'M OK");
  lv_arc_set_value(crashArc, left);
  lv_obj_clear_flag(crashOv, LV_OBJ_FLAG_HIDDEN);
}

void ui_crash_sos_sent() {
  setText(lblCrashTitle, "SOS SENT");
  setText(lblCrashNum, "");
  setText(lblCrashSub, "Your live location was shared");
  setText(lblCrashOk, "DISMISS");
  lv_arc_set_value(crashArc, 0);
}

void ui_update() {
  uint32_t now = millis();
  float dt = lastUpdateMs ? (now - lastUpdateMs) / 1000.0f : 0.03f;
  lastUpdateMs = now;

  if (activeTile() == 1) mapAnimate(dt);
  lv_obj_set_style_opa(statusRow, activeTile() == 1 && nav.active && nav.routeN ? LV_OPA_TRANSP : LV_OPA_COVER, 0);

  // once a second: clock, ride stats, call timer
  static uint32_t lastSec = 0;
  if (now - lastSec >= 1000) {
    float secs = lastSec ? (now - lastSec) / 1000.0f : 0;
    lastSec = now;
    struct tm t;
    char b[48];
    if (localTime(t)) {
      static const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
      static const char *mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min);
      setText(lblClock, b);
      snprintf(b, sizeof b, "%s %d %s", wd[t.tm_wday], t.tm_mday, mo[t.tm_mon]);
      setText(lblDate, b);
    } else {
      setText(lblClock, "--:--");
      setText(lblDate, "");
    }

    int kmh = nav.active && nav.speedKmh >= 0 && now - nav.lastFrameMs < 5000 ? nav.speedKmh : -1;
    if (kmh > 0) tripKm += kmh * secs / 3600.0f;
    snprintf(b, sizeof b, kmh >= 0 ? "%d" : "--", kmh);
    setText(lblSpeed, b);
    lv_arc_set_value(speedArc, kmh > 0 ? min(kmh, 60) : 0);
    lv_obj_set_style_arc_color(speedArc, kmh > 45 ? C_WARN : C_MINT, LV_PART_INDICATOR);
    snprintf(b, sizeof b, "%.1f", tripKm);
    setText(lblTrip, b);
    uint32_t ride = nav.active ? (now - nav.startedMs) / 1000 : 0;
    snprintf(b, sizeof b, "%lu:%02lu", (unsigned long)(ride / 60), (unsigned long)(ride % 60));
    setText(lblTime, b);

    if (call.phase == CALL_ACTIVE) {
      uint32_t s = (now - call.sinceMs) / 1000;
      snprintf(b, sizeof b, "%s · %lu:%02lu", call.caller, (unsigned long)(s / 60), (unsigned long)(s % 60));
      setText(lblCallPill, b);
    }
    // The current app never says when a ringing call ends — give up after 30 s.
    if (call.phase == CALL_RINGING && now - call.sinceMs > 30000) {
      call.phase = CALL_NONE;
      showCall();
    }
  }

  if (doneUntilMs && now > doneUntilMs) {
    doneUntilMs = 0;
    lv_obj_add_flag(doneOv, LV_OBJ_FLAG_HIDDEN);
    showTile(0);
  }

  // Home ring breathes for a few seconds after waking, then rests (saves redraws).
  if (activeTile() == 0) {
    static uint32_t lastRing = 0;
    if (now - wakeMs < 8000 && now - lastRing > 80) {
      lastRing = now;
      float ph = (now - wakeMs) / 3600.0f * 2 * (float)M_PI;
      lv_obj_set_style_border_opa(homeRing, (lv_opa_t)(70 + 60 * (0.5f - 0.5f * cosf(ph))), 0);
    }
  }
}
