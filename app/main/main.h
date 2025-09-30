/**
 * @file main.h
 * @brief Main header file for ESP32 configuration and functionality.
 *
 * This file contains function declarations related to WiFi setup, OTA updates,
 * glucose data processing, and error handling.
 */

#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>

extern int16_t glucose_delta;              ///< Change from last reading
extern uint16_t glucoseMeasurement_backup; ///< Previous measurement

/**
 * @enum GlucoseLabelColor
 * @brief Color of Glucose Label
 */
enum GlucoseLabelColor : uint8_t {
    NO_COLOR = 0,
    COLOR_WHITE = 1,
    COLOR_YELLOW = 2,
    COLOR_ORANGE = 3,
    COLOR_RED = 4,
    COLOR_BLUE = 5,
};
/** @} */

/**
 * @brief Displays ESP32 system status information.
 */
void esp_status();

/**
 * @brief Configures WireGuard VPN.
 * 
 * @param enable Set to `true` to enable WireGuard, `false` to disable.
 */
void setup_wg(bool enable);

/**
 * @brief Initializes the WiFi connection.
 */
void setup_wifi();

/**
 * @brief Sets the brightness of the TRGB backlight.
 * 
 * @param value Brightness level (0-255).
 * @return The brightness value that was set.
 */
uint8_t set_trgb_backlight_brightness(uint8_t value);

/**
 * @brief Configures OTA (Over-the-Air) updates.
 * 
 * @param mode Set to `true` to enable OTA updates, `false` to disable.
 */
void setup_OTA(bool mode);

/**
 * @brief Draws the glucose data chart.
 * 
 * @param mode Chart mode (e.g., display type).
 * @param fiveminuteupdate Set to `true` for updates every 5 minutes.
 */
void draw_chart_glucose_data(uint8_t mode, bool fiveminuteupdate);

/**
 * @brief Handles cases where the timestamp is invalid.
 */
void handle_invalid_timestamp();

/**
 * @brief Handles the event when the glucose sensor reconnects.
 */
void handle_sensor_reconnect();

/**
 * @brief Handles errors related to the LibreLinkUp API.
 */
void handle_llu_api_error();

/**
 * @brief Handles internet disconnection events.
 */
void handle_internet_disconnection();

/**
 * @brief Synchronizes the time offset with the server.
 */
void synchronize_time_offset();

/**
 * @brief Updates the trend message based on glucose levels.
 */
void update_trend_message();

/**
 * @brief Updates the five-minute glucose data counter.
 */
void update_five_minute_counter();

/**
 * @brief Updates glucose measurement data.
 */
void update_glucose_data();

/**
 * @brief Logs glucose data to a JSON file.
 */
void update_glucose_json_logging();

/**
 * @brief Calculates and logs glucose statistics.
 */
void glucose_statistics();

void draw_labels(uint8_t mode, uint8_t _glucose_measurement_color, uint16_t _glucose_value, String _trendarrow, String _trendmessage, int16_t delta);

#endif // MAIN_H