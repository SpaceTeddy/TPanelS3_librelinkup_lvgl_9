#pragma once
/**
 * @file tpanels3.h
 * @brief T-Panel S3 hardware abstraction: touch, display (Arduino_GFX) and LVGL glue.
 *
 * This module encapsulates:
 *  - Touch controller setup (CST3240 via TouchLib, interrupt-driven)
 *  - Display bring-up (XL9535 I²C expander + ST7701 over RGB panel)
 *  - LVGL integration (display flush + input driver registration)
 *
 * @note Requires external pin/timing definitions (e.g., from pin_config.h) and
 *       the TouchLib configuration macro for your touch IC.
 * @see TouchLib: define TOUCH_MODULES_CST_MUTUAL for CST3240 before including TouchLib.h
 */

#include <stdint.h>
#include <lvgl.h>  // Needed for exact callback signatures

// Forward declaration to avoid heavy Arduino_GFX includes in user code.
class Arduino_RGB_Display;

/**
 * @class TPanelS3
 * @brief Static façade to initialize and access the T-Panel S3 hardware.
 *
 * Typical usage:
 * @code
 *   #include "tpanels3.h"
 *   void setup() {
 *     TPanelS3::initAll();
 *     TPanelS3::setRotation(1);
 *   }
 * @endcode
 */
class TPanelS3 {
public:
  /**
   * @brief Bring up touch, display and LVGL.
   *
   * Initializes:
   *  - I²C and TouchLib (CST3240)
   *  - Arduino_GFX bus/panel/display
   *  - LVGL display flush + input drivers (buffers allocated from PSRAM)
   *  - Touch interrupt line
   *
   * @warning Call exactly once during startup (e.g., from setup()).
   */
  static void initTPanelS3();

  /**
   * @brief Set display rotation.
   * @param r Rotation: 0=0°, 1=90°, 2=180°, 3=270°.
   */
  static void setRotation(uint8_t r);

  /**
   * @brief LVGL input read callback (exact LVGL signature).
   * @details Registered internally by initAll(). Exposed for testing/advanced use.
   * @param indev LVGL input device instance.
   * @param data  Output touch state/coordinates.
   */
  static void lvglTouchRead(::lv_indev_t* indev, ::lv_indev_data_t* data);

  /**
   * @brief Touch interrupt service routine (ISR), toggled on FALLING edge.
   * @note Attached by initAll(); marked IRAM_ATTR for ESP32.
   */
  static void IRAM_ATTR onTouchISR();

  /**
   * @brief Access the global display object.
   * @return Pointer to Arduino_RGB_Display or nullptr if not initialized yet.
   */
  static Arduino_RGB_Display* display();

  /**
   * @brief Set the backlight brightness.
   * 
   * @param value Brightness level (0-255).
   * @return The set brightness level (0-255).
   */
  static uint8_t set_backlight_brightness(uint8_t value);
};
