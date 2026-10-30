#include "display.h"
#include <Wire.h>
#include <lvgl.h>
#include "board.h"
#include "hal.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "drivers/esp_lcd_sh8601.h"

#define LCD_HOST     SPI2_HOST
#define BUF_LINES    40      // 2 × 466×40 RGB565 DMA buffers ≈ 75 KB (no PSRAM on the C6)

static esp_lcd_panel_io_handle_t panelIo = nullptr;
static esp_lcd_panel_handle_t    panel   = nullptr;
static lv_disp_drv_t dispDrv;
static lv_disp_draw_buf_t drawBuf;
static bool asleep = false;
static uint8_t brightnessPct = 80;
static uint32_t lastTouchMs = 0;

static const sh8601_lcd_init_cmd_t initCmds[] = {
  {0x11, (uint8_t[]){0x00}, 0, 80},   // sleep out
  {0xC4, (uint8_t[]){0x80}, 1, 0},
  {0x53, (uint8_t[]){0x20}, 1, 1},    // brightness control on
  {0x63, (uint8_t[]){0xFF}, 1, 1},
  {0x51, (uint8_t[]){0x00}, 1, 1},    // start dark, fade in after the first frame
  {0x29, (uint8_t[]){0x00}, 0, 10},   // display on
};

static esp_err_t panelCmd(uint8_t cmd, const uint8_t *data, size_t len) {
  return esp_lcd_panel_io_tx_param(panelIo, (0x02 << 24) | (cmd << 8), data, len);
}

static bool IRAM_ATTR onFlushDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *ctx) {
  lv_disp_flush_ready((lv_disp_drv_t *)ctx);
  return false;
}

static void flushCb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
  esp_lcd_panel_draw_bitmap(panel, a->x1 + LCD_X_OFFSET, a->y1, a->x2 + 1 + LCD_X_OFFSET, a->y2 + 1, px);
}

// The controller needs even-aligned windows.
static void rounderCb(lv_disp_drv_t *, lv_area_t *a) {
  a->x1 &= ~1;
  a->y1 &= ~1;
  a->x2 |= 1;
  a->y2 |= 1;
}

// The FT6146 drops into monitor mode a few seconds after the last touch and
// stops ACKing I2C until a finger lands, so polling it blindly fails ~30×/s.
// Only talk to it while a touch is in progress or its INT line (TCA9554 input
// 0, reported through the expander's #INT on PIN_EXIO_INT) says one started.
static bool touchDown = false;
static uint32_t touchRetryUntilMs = 0;   // the first read after waking can NACK

static bool touchPending() {
  if (touchDown || millis() < touchRetryUntilMs) return true;
  if (digitalRead(PIN_EXIO_INT) == HIGH) return false;   // no expander input changed
  uint8_t in = exio_read_inputs();                        // also clears #INT
  if (in == 0xFF || (in & (1 << EXIO_TP_INT))) return false;   // TP_INT is active low
  touchRetryUntilMs = millis() + 150;
  return true;
}

static bool readTouch(uint16_t &x, uint16_t &y) {
  if (!touchPending()) return false;
  touchDown = false;
  Wire.beginTransmission(I2C_ADDR_TOUCH);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)I2C_ADDR_TOUCH, (uint8_t)5) != 5) return false;
  uint8_t b[5];
  for (int i = 0; i < 5; i++) b[i] = Wire.read();
  if ((b[0] & 0x0F) == 0) return false;
  touchDown = true;
  x = min<uint16_t>(((b[1] & 0x0F) << 8) | b[2], LCD_RES - 1);
  y = min<uint16_t>(((b[3] & 0x0F) << 8) | b[4], LCD_RES - 1);
  return true;
}

static void touchCb(lv_indev_drv_t *, lv_indev_data_t *data) {
  uint16_t x, y;
  if (readTouch(x, y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
    lastTouchMs = millis();
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

bool touch_poll_pressed() {
  uint16_t x, y;
  bool p = readTouch(x, y);
  if (p) lastTouchMs = millis();
  return p;
}

uint32_t touch_last_ms() { return lastTouchMs; }

void display_init() {
  spi_bus_config_t bus = {};
  bus.data0_io_num = PIN_LCD_D0;
  bus.data1_io_num = PIN_LCD_D1;
  bus.sclk_io_num = PIN_LCD_CLK;
  bus.data2_io_num = PIN_LCD_D2;
  bus.data3_io_num = PIN_LCD_D3;
  bus.max_transfer_sz = LCD_RES * BUF_LINES * 2 + 16;
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = PIN_LCD_CS;
  io.dc_gpio_num = -1;
  io.spi_mode = 0;
  io.pclk_hz = 40 * 1000 * 1000;
  io.trans_queue_depth = 10;
  io.on_color_trans_done = onFlushDone;
  io.user_ctx = &dispDrv;
  io.lcd_cmd_bits = 32;
  io.lcd_param_bits = 8;
  io.flags.quad_mode = true;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &panelIo));

  sh8601_vendor_config_t vendor = {};
  vendor.flags.use_qspi_interface = 1;
  vendor.init_cmds = initCmds;
  vendor.init_cmds_size = sizeof(initCmds) / sizeof(initCmds[0]);
  esp_lcd_panel_dev_config_t cfg = {};
  cfg.reset_gpio_num = PIN_LCD_RST;
  cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  cfg.bits_per_pixel = 16;
  cfg.vendor_config = &vendor;
  ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(panelIo, &cfg, &panel));
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);
  esp_lcd_panel_disp_on_off(panel, true);

  lv_init();
  auto *b1 = (lv_color_t *)heap_caps_malloc(LCD_RES * BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA);
  auto *b2 = (lv_color_t *)heap_caps_malloc(LCD_RES * BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA);
  assert(b1 && b2);
  lv_disp_draw_buf_init(&drawBuf, b1, b2, LCD_RES * BUF_LINES);
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = LCD_RES;
  dispDrv.ver_res = LCD_RES;
  dispDrv.flush_cb = flushCb;
  dispDrv.rounder_cb = rounderCb;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);

  static lv_indev_drv_t indev;
  lv_indev_drv_init(&indev);
  indev.type = LV_INDEV_TYPE_POINTER;
  indev.read_cb = touchCb;
  lv_indev_drv_register(&indev);
}

void display_set_brightness(uint8_t pct) {
  brightnessPct = min<uint8_t>(pct, 100);
  if (asleep) return;
  uint8_t v = (uint8_t)(brightnessPct * 255 / 100);
  panelCmd(0x51, &v, 1);
}

void display_sleep() {
  if (asleep) return;
  uint8_t zero = 0;
  panelCmd(0x51, &zero, 1);
  panelCmd(0x28, nullptr, 0);   // display off
  panelCmd(0x10, nullptr, 0);   // sleep in
  asleep = true;
}

void display_wake() {
  if (!asleep) return;
  panelCmd(0x11, nullptr, 0);   // sleep out
  delay(80);
  panelCmd(0x29, nullptr, 0);
  asleep = false;
  lv_obj_invalidate(lv_scr_act());
  display_set_brightness(brightnessPct);
}

bool display_is_asleep() { return asleep; }
