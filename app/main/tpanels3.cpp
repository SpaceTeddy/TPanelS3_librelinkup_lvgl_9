/**
 * @file tpanels3.cpp
 * @brief Implementation of the T-Panel S3 hardware abstraction.
 *
 * Responsibilities:
 *  - Configure and own the global display/touch/LVGL state
 *  - Provide LVGL display flush + input callbacks
 *  - Hide Arduino_GFX internals from application code
 *
 * Build assumptions:
 *  - PSRAM is available (LVGL buffers are allocated from PSRAM)
 *  - TouchLib is configured for CST3240: #define TOUCH_MODULES_CST_MUTUAL
 */

#include "tpanels3.h"

#include <Arduino.h>
#include <Wire.h>

// Arduino_GFX (panel + bus)
#include "Arduino_GFX_Library.h"

// Touch controller
// Ensure the correct touch model is defined before TouchLib.h
#ifndef TOUCH_MODULES_CST_MUTUAL
#define TOUCH_MODULES_CST_MUTUAL
#endif
#include <TouchLib.h>

// Pins/timings for your board (must define LCD_* and XL95X5_* symbols, etc.)
#include "pin_config.h"

// -------------------- Static state (translation-unit scope) --------------------

/// Latched by ISR to indicate a pending touch read.
static volatile bool s_touchIntFlag = false;

/// Touch controller instance (CST3240 on I²C)
static TouchLib s_touch(Wire, TOUCH_SDA, TOUCH_SCL, CST3240_ADDRESS);

/// Display bus & panel
static Arduino_DataBus*        s_bus   = nullptr;
static Arduino_ESP32RGBPanel*  s_rgb   = nullptr;
static Arduino_RGB_Display*    s_gfx   = nullptr;

/// LVGL device handles and buffers
#if LVGL_VERSION_MAJOR >= 9
  static lv_display_t* s_lvDisplay = nullptr;
#else
  static lv_disp_draw_buf_t s_lvDrawBuf;
  static lv_color_t*        s_buf1 = nullptr;
  static lv_color_t*        s_buf2 = nullptr;
  static lv_disp_drv_t      s_lvDispDrv;
#endif

static lv_indev_t* s_lvIndev = nullptr;

// -------------------- Private helpers --------------------

Arduino_RGB_Display* TPanelS3::display() {
  return s_gfx;
}

void TPanelS3::setRotation(uint8_t r) {
  if (s_gfx) s_gfx->setRotation(r);
}

void IRAM_ATTR TPanelS3::onTouchISR() {
  s_touchIntFlag = true;
}

#if LVGL_VERSION_MAJOR >= 9
/**
 * @brief LVGL v9 display flush callback.
 * @param disp   LVGL display handle
 * @param area   dirty rectangle to update
 * @param px_map pointer to 16-bit RGB565 pixel buffer
 */
static void lvglFlush(::lv_display_t* disp, const ::lv_area_t* area, uint8_t* px_map) {
  const uint32_t w = (area->x2 - area->x1 + 1);
  const uint32_t h = (area->y2 - area->y1 + 1);
#if LV_COLOR_16_SWAP
  s_gfx->draw16bitBeRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t*>(px_map), w, h);
#else
  s_gfx->draw16bitRGBBitmap  (area->x1, area->y1, reinterpret_cast<uint16_t*>(px_map), w, h);
#endif
  lv_display_flush_ready(disp);
}
#else
/**
 * @brief LVGL v8 display flush callback.
 * @param drv     LVGL display driver
 * @param area    dirty rectangle to update
 * @param color_p pointer to 16-bit RGB565 pixel buffer
 */
static void lvglFlush(::lv_disp_drv_t* drv, const ::lv_area_t* area, ::lv_color_t* color_p) {
  const uint32_t w = (area->x2 - area->x1 + 1);
  const uint32_t h = (area->y2 - area->y1 + 1);
#if LV_COLOR_16_SWAP
  s_gfx->draw16bitBeRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t*>(color_p), w, h);
#else
  s_gfx->draw16bitRGBBitmap  (area->x1, area->y1, reinterpret_cast<uint16_t*>(color_p), w, h);
#endif
  lv_disp_flush_ready(drv);
}
#endif

// Map LVGL press/release enum across v8/v9
#if LVGL_VERSION_MAJOR >= 9
  #define LV_PRESSED  LV_INDEV_STATE_PRESSED
  #define LV_RELEASED LV_INDEV_STATE_RELEASED
#else
  #define LV_PRESSED  LV_INDEV_STATE_PR
  #define LV_RELEASED LV_INDEV_STATE_REL
#endif

// -------------------- LVGL input read callback (public) --------------------

/**
 * @brief LVGL input read callback for the CST3240 via TouchLib.
 *
 * Reads one-touch data when the ISR flagged a pending event and translates it
 * to LVGL input state + coordinates. Releases state when no touch is present.
 *
 * @param indev LVGL input device (unused)
 * @param data  Out: LVGL touch state and point
 */
void TPanelS3::lvglTouchRead(::lv_indev_t* /*indev*/, ::lv_indev_data_t* data) {
  if (s_touchIntFlag) {
    s_touch.read();
    TP_Point t = s_touch.getPoint(0);

    if (s_touch.getPointNum() == 1 && t.pressure > 0 && t.state != 0) {
      data->state   = LV_PRESSED;
      data->point.x = t.x;
      data->point.y = t.y;
    } else {
      data->state = LV_RELEASED;
    }
    s_touchIntFlag = false;
  } else {
    data->state = LV_RELEASED;
  }
}

// -------------------- Public init --------------------

/**
 * @brief Initialize touch (I²C + ISR), display (bus/panel) and LVGL (display+input).
 *
 * Steps:
 *  1. Start I²C and construct TouchLib for CST3240
 *  2. Create Arduino_GFX bus/panel/display objects
 *  3. Reset touch via XL9535 (if supported)
 *  4. Initialize LVGL core, allocate PSRAM buffers, register flush callback
 *  5. Create LVGL input device and wire read callback
 *  6. Attach touch interrupt and bring up the panel
 */
void TPanelS3::initTPanelS3() {
  // Initialize backlight PWM pin
  pinMode(LCD_BL, OUTPUT);
  
  // --- I²C for touch
  Wire.begin(IIC_SDA, IIC_SCL);

  // --- Prepare XL9535-backed SW SPI bus for panel control
  s_bus = new Arduino_XL9535SWSPI(
      IIC_SDA, IIC_SCL, -1,             // I²C SDA/SCL, PWD (unused)
      XL95X5_CS, XL95X5_SCLK, XL95X5_MOSI);

  // --- RGB parallel panel timings
  s_rgb = new Arduino_ESP32RGBPanel(
      -1 /*DE*/, LCD_VSYNC, LCD_HSYNC, LCD_PCLK,
      LCD_B0, LCD_B1, LCD_B2, LCD_B3, LCD_B4,
      LCD_G0, LCD_G1, LCD_G2, LCD_G3, LCD_G4, LCD_G5,
      LCD_R0, LCD_R1, LCD_R2, LCD_R3, LCD_R4,
      /* HSYNC */ 1, 20, 2, 0,
      /* VSYNC */ 1, 30, 8, 1,
      /* PCLK  */ 1, 6000000L,
      /* BE    */ false,
      /* DE idle high */ 0,
      /* PCLK idle high */ 0);

  // --- Main display object
  s_gfx = new Arduino_RGB_Display(
      LCD_WIDTH, LCD_HEIGHT, s_rgb, 0 /*rotation*/, true /*auto_flush*/,
      s_bus, -1 /*RST*/, st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

  // --- Touch reset via expander (depends on Arduino_GFX build)
#ifdef XL95X5_TOUCH_RST
  // Some Arduino_GFX builds forward expander GPIO helpers via the display object.
  // If not available, toggle the expander pin via your own XL95X5 helper.
  if (s_gfx) {
    s_gfx->XL_digitalWrite(XL95X5_TOUCH_RST, LOW);
    delay(200);
    s_gfx->XL_digitalWrite(XL95X5_TOUCH_RST, HIGH);
    delay(200);
  }
#endif

  // --- Touch init
  s_touch.init();

  // --- LVGL core + tick
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

#if LVGL_VERSION_MAJOR >= 9
  // Allocate PSRAM buffers for full-screen double buffering
  lv_color_t* buf1 = (lv_color_t*)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_color_t* buf2 = (lv_color_t*)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  assert(buf1 && buf2);

  s_lvDisplay = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  lv_display_set_buffers(s_lvDisplay, buf1, buf2, LCD_WIDTH * LCD_HEIGHT, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(s_lvDisplay, lvglFlush);
#else
  // v8: draw buffer + driver registration
  s_buf1 = (lv_color_t*)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  s_buf2 = (lv_color_t*)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  assert(s_buf1 && s_buf2);

  lv_disp_draw_buf_init(&s_lvDrawBuf, s_buf1, s_buf2, LCD_WIDTH * LCD_HEIGHT);

  lv_disp_drv_init(&s_lvDispDrv);
  s_lvDispDrv.hor_res  = LCD_WIDTH;
  s_lvDispDrv.ver_res  = LCD_HEIGHT;
  s_lvDispDrv.flush_cb = lvglFlush;
  s_lvDispDrv.draw_buf = &s_lvDrawBuf;
  lv_disp_drv_register(&s_lvDispDrv);
#endif

  // --- LVGL input device
  s_lvIndev = lv_indev_create();
  lv_indev_set_type(s_lvIndev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_lvIndev, TPanelS3::lvglTouchRead);

  // --- Touch interrupt line
  pinMode(TOUCH_INT, INPUT_PULLUP);
  attachInterrupt(TOUCH_INT, TPanelS3::onTouchISR, FALLING);

  // --- Panel bring-up
  s_gfx->begin();
  s_gfx->fillScreen(BLACK);
  setRotation(0);
}

/**
 * @brief Sets TPanel backlight brightness using PWM
 * 
 * The TPanel backlight chip has 16 adjustment levels (0-15).
 * Uses LEDC PWM with 8-bit resolution for smooth dimming.
 * 
 * @param[in] value Brightness level (0-255, 0=off, 255=max)
 * @return The brightness value that was set
 */
uint8_t TPanelS3::set_backlight_brightness(uint8_t value)
{
    ledcSetup(0, 15000, 8);      // Channel 0, 15kHz, 8-bit resolution
    ledcAttachPin(LCD_BL, 0);    // Attach backlight pin to channel 0
    ledcWrite(0, value);         // Set brightness value
    
    return value;
}
