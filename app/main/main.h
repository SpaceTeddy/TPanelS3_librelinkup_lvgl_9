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

#define RECONNECT_WIFI_TIMEOUT_MS (1000)    ///< WiFi reconnect timeout in milliseconds

extern int16_t glucose_delta;               ///< Change from last reading
extern uint16_t glucoseMeasurement_backup;  ///< Previous measurement
extern String g_h2_fw_version;              ///< Last known H2 firmware version
extern String g_h2_fw_build;                ///< Last known H2 firmware build string
extern String g_h2_chip_model;              ///< Last known H2 chip model
extern String g_h2_chip_rev;                ///< Last known H2 chip revision
extern String g_h2_chip_mac;                ///< Last known H2 MAC/EUI
extern String g_h2_chip_cores;              ///< Last known H2 core count
extern String g_h2_chip_cpu_mhz;            ///< Last known H2 CPU clock
extern String g_h2_chip_xtal_mhz;           ///< Last known H2 XTAL clock
extern String g_h2_chip_features;           ///< Last known H2 feature string
extern String g_h2_last_type;               ///< Last H2 message type
extern String g_h2_last_json;               ///< Last raw H2 JSON message
extern uint32_t g_h2_last_seen_ms;          ///< millis() of last H2 message

/// Feed the loop() hang-detector heartbeat from within a long blocking step
/// (e.g. FW_INSTALLING) that won't return to loop()'s top for a while.
void feed_loop_heartbeat();


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

/**
 * @brief enable UART IPC communication
 */
void setup_UART_IPC(void);
void h2_send(const char *cmd);

/** @} */

/**
 * @brief Sets up mDNS for the ESP32.
 */
void setup_mdns(void);


/**
 * @brief Displays ESP32 system status information.
 */
void esp_status();

/**
 * @brief Configures WireGuard VPN.
 * 
 * @param enable Set to `true` to enable WireGuard, `false` to disable.
 */
bool setup_wg(bool enable, bool force_reinit = false);
bool wg_is_busy();

/**
 * @brief Initializes the WiFi connection.
 */
void setup_wifi();

/**
 * @brief Configures OTA (Over-the-Air) updates.
 * 
 * @param mode Set to `true` to enable OTA updates, `false` to disable.
 */
void setup_OTA(bool mode);

/**
 * @brief Initializes MQTT communication.
 */
bool setup_mqtt(void);


/**
 * @brief Sets the brightness of the TRGB backlight.
 * 
 * @param value Brightness level (0-255).
 * @return The brightness value that was set.
 */
uint8_t set_trgb_backlight_brightness(uint8_t value);


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

/**
 * @brief Checks the internet connection status.
 * 
 * @return An integer representing the internet status (e.g., 0 for connected, 1 for disconnected).
 */
int app_check_internet_status();

/**
 * @brief Performs recovery actions when the device is offline.
 */
void app_recover_offline();

#endif // MAIN_H
