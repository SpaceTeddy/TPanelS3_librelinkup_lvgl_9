/**
 * @file main.cpp
 * @brief Main application file for ESP32 LibreLinkUp Client
 * @version 1.0
 * @date 2025
 *
 * This is the main entry point for the ESP32-based LibreLinkUp glucose monitoring
 * client. It handles:
 * - LVGL UI initialization and management
 * - WiFi connectivity and network management
 * - LibreLinkUp API communication
 * - MQTT client for IoT integration
 * - WireGuard VPN support
 * - OTA (Over-The-Air) firmware updates
 * - Touch screen interaction
 * - Data logging to LittleFS
 *
 * @author Chris
 * @license GPL 3.0
 *
 * Hardware:
 * - ESP32-S3 with TPanel RGB display (480x480)
 * - CST3240 capacitive touch controller
 * - 16MB PSRAM
 *
 * @note Requires Arduino framework and PlatformIO
 */

#include <Arduino.h>
#include <string.h>
#include "main.h"
#include "app_fsm.h"
#include "lvgl.h"
#include "Arduino_GFX_Library.h"
#include "pin_config.h"

///////////////////// HELPER UTILITIES ////////////////////

#include "helper.h"
HELPER helper; ///< Helper class instance for time and utility functions

///////////////////// UART IPC COMMUNICATION ////////////////////

#include <HardwareSerial.h>

/// UART2 instance for IPC communication between ESP32-H2 and ESP32-S3
HardwareSerial SerialPort(2);
char UART_IPC_DATA1 = 0; ///< Buffer for received UART data

///////////////////// CONFIGURATION ////////////////////

#include "settings.h"

SETTINGS settings; ///< Settings manager instance

bool flag_mqtt_master_rx = true; // for first run
bool flag_debug_screen = true;   // Debug flag to trigger debug screen on first loop iteration

static AppFsm g_fsm; ///< Application state machine (polled from loop())

/// @name System Status Counters
/// @{
uint8_t esp_status_counter_wifi_restart = 0; ///< WiFi reconnection counter
uint8_t esp_status_counter_llu_reauth = 0;   ///< LibreLinkUp re-authentication counter
uint8_t esp_status_counter_llu_retou = 0;    ///< LibreLinkUp terms of use acceptance counter
/// @}

///////////////////// DEBUG MACROS ////////////////////

/// Debug print macro with timestamp and function name
#define DBGprint Serial.printf("[%09lu ms][%s][%s] ", (unsigned long)millis(), __FILE__, __func__)

///////////////////// TPanelS3 SELECTION ////////////////////

#include "tpanels3.h"
TPanelS3 tpanels3; ///< TPanelS3 hardware interface instance

///////////////////// BACKLIGHT CONTROL ////////////////////

/// Standard backlight brightness level (0-255)
#define TRGB_STD_BACKLIGHT_BRIGHTNESS (50)

///////////////////// FREERTOS TASK HANDLES ////////////////////

TaskHandle_t wifiScanHandle = NULL; ///< WiFi network scan background task
TaskHandle_t LoopTaskhandle = NULL; ///< Main loop task handle
TaskHandle_t LvglTaskHandle = NULL; ///< LVGL tick task handle

///////////////////// JSON PARSER ////////////////////

#include <ArduinoJson.h>
#include <StreamUtils.h>

///////////////////// FILESYSTEM ////////////////////

#include <LittleFS.h>

///////////////////// MDNS SERVICE ////////////////////

#include <ESPmDNS.h>

String hostname_base = "librelinkup_"; ///< Base hostname for mDNS
String hostname = "";                  ///< Full hostname with unique ID

///////////////////// CONSOLE COMMANDS ////////////////////

#include "commands.h"

using uuid::flash_string_vector;
using uuid::read_flash_string;
using uuid::console::Commands;
using uuid::console::Shell;
using LogFacility = ::uuid::log::Facility;

/// Console command handler instance
static std::shared_ptr<uuid::console::Commands> commands = std::make_shared<uuid::console::Commands>();

/// Telnet service for remote console access
static uuid::telnet::TelnetService telnet{commands};

/// Logger instance for this module
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

///////////////////// WIREGUARD VPN ////////////////////

#include <WireGuard-ESP32.h>
#include "wireguardif.h"

static WireGuard wg; ///< WireGuard VPN client instance

/**
 * @brief WireGuard VPN configuration
 *
 * Configuration loaded from settings.config at runtime:
 * - local_ip: Virtual IP address inside VPN tunnel
 * - private_key: Client private key
 * - public_key: Server public key
 * - preshared_key: Pre-shared key for additional security
 * - endpoint_address: VPN server hostname/IP
 * - endpoint_port: VPN server port
 */
IPAddress local_ip(192, 168, 0, 103); ///< VPN tunnel IP address

///////////////////// HBA1C CALCULATION ////////////////////

#include "hba1c.h"

HBA1C hba1c; ///< HbA1c calculation engine

///////////////////// LIBRELINKUP API CLIENT ////////////////////

#include <librelinkup.h>

LIBRELINKUP librelinkup; ///< LibreLinkUp API client instance

int16_t glucose_delta = 0;              ///< Change from last reading (mg/dL)
uint16_t glucoseMeasurement_backup = 0; ///< Previous glucose measurement

///////////////////// UI COMPONENTS ////////////////////

#include "ui.h"

/**
 * @brief Updates OTA progress display
 *
 * @param[in] progress Update progress percentage (0-100)
 * @return Always returns 1
 */
uint8_t update_ota_progress_screen(int progress)
{
    char progress_text[10];
    snprintf(progress_text, sizeof(progress_text), "%d%%", progress);
    lv_label_set_text(ui_Label_FWUpdateProgress_percent, progress_text);
    lv_timer_handler();
    delay(5);
    return 1;
}

///////////////////// TIME ZONE CONFIGURATION ////////////////////

/// Time zone string for Central European Time (CET/CEST)
/// Format: std offset dst [offset],start[/time],end[/time]
const char *tz = "CET-1CEST,M3.5.0/2,M10.5.0/3";

///////////////////// OTA UPDATE ////////////////////

#include <ElegantOTA.h>

AsyncWebServer server(80); ///< Async web server for OTA and config

uint32_t ota_progress_millis = 0; ///< Last OTA progress update timestamp
bool ota_in_progress = 0;         ///< OTA update in progress flag

///////////////////// WIFI BACKGROUND SCAN ////////////////////

TaskHandle_t scanTaskHandle; ///< WiFi scan task handle
String availableNetworks;    ///< JSON string of available networks

/**
 * @brief Background WiFi network scanner task
 *
 * Periodically scans for available WiFi networks and stores results
 * as JSON string for web interface display. Pauses during OTA updates.
 *
 * @param[in] parameter Task parameter (unused)
 */
// globals:
static volatile bool g_scan_in_progress = false;

void scanWiFiTask(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        if (ota_in_progress)
        {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        g_scan_in_progress = true;
        int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);

        // Build JSON with less fragmentation
        String json;
        json.reserve(256);
        json = "[";

        bool first = true;
        for (int i = 0; i < n; ++i)
        {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0)
                continue;

            if (!first)
                json += ",";
            first = false;

            json += "{\"ssid\":\"";
            json += ssid;
            json += "\",\"rssi\":";
            json += String(WiFi.RSSI(i));
            json += "}";
        }
        json += "]";

        availableNetworks = json;
        WiFi.scanDelete();
        g_scan_in_progress = false;

        vTaskDelay(pdMS_TO_TICKS(36000)); // 36s
    }
}

///////////////////// WEB INTERFACE ////////////////////

#include "webpage.h"

///////////////////// OTA CALLBACKS ////////////////////

/**
 * @brief OTA update start callback
 *
 * Called when OTA update begins. Pauses background tasks and
 * sets OTA progress flag.
 */
void onOTAStart()
{
    Serial.println("OTA update started!");
    logger.notice("OTA Update Progress has started");

    // wait for scan to finish (max ~2s), no suspend
    uint32_t t0 = millis();
    while (g_scan_in_progress && (millis() - t0) < 2000)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ota_in_progress = 1;

    if (lv_screen_active() != ui_FWUpdate_screen)
    {
        lv_disp_load_scr(ui_FWUpdate_screen);
        lv_label_set_text(ui_Label_FWUpdateInfo, "Firmware Update in progress...");
        lv_timer_handler();
    }
}

/**
 * @brief OTA update progress callback
 *
 * Called periodically during OTA update with progress information.
 * Updates progress display every 1000ms.
 *
 * @param[in] current Bytes received so far
 * @param[in] final   Total bytes to receive
 */
void onOTAProgress(size_t current, size_t final)
{
    // Log every 1000 milliseconds
    if (millis() - ota_progress_millis > 1000)
    {
        ota_progress_millis = millis();

        // Calculate progress percentage
        float progress = ((float)current / (float) final) * 100.0;

        if (ota_in_progress == 1)
        {
            update_ota_progress_screen(progress);
            logger.notice("FWUpdate Progress: %.2f%% (%d / %d Bytes)", progress, current, final);
        }
    }
}

/**
 * @brief OTA update completion callback
 *
 * Called when OTA update finishes (success or failure).
 * On success, displays completion message and triggers reboot.
 * On failure, returns to main screen.
 *
 * @param[in] success True if update succeeded, false otherwise
 */
void onOTAEnd(bool success)
{
    vTaskResume(LvglTaskHandle);
    if (success == 0)
    {
        // Update failed - return to main screen
        lv_disp_load_scr(ui_Main_screen);
        lv_timer_handler();
        delay(5);
        ota_in_progress = 0;
    }
    else if (success == 1)
    {
        // Update successful - show completion message
        ota_in_progress = 0;
        lv_label_set_text(ui_Label_FWUpdateProgress_percent, "100%");
        lv_label_set_text(ui_Label_FWUpdateInfo, "FWUpdate successful!\n\nperforming Reset");
        lv_task_handler();
        delay(5);
        delay(250);
    }
}

///////////////////// SOFTWARE TIMERS ////////////////////

/// @name Timer Intervals (milliseconds)
/// @{
const uint64_t config_sleep_timer = 120000; ///< 60 minute (1 hour) sleep timer
/// @}

/// @name Timer Backup Variables (last trigger time)
/// @{
uint64_t config_sleep_timer_backup = 0;
/// @}

///////////////////// JSON CONFIGURATION ////////////////////

/// Enable double precision for JSON parsing
#define ARDUINOJSON_USE_DOUBLE 1

///////////////////// WIFI MANAGER ////////////////////

#include <WiFi.h>
#include <WiFiMulti.h>

WiFiMulti wifiMulti; ///< WiFi multi-connection manager

/// WiFi connection timeout in milliseconds
const uint32_t connectTimeoutMs = 5000;

///////////////////// INTERNET CONNECTIVITY ////////////////////


/**
 * @enum InternetStatus
 * @brief Internet connection status enumeration
 */
enum InternetStatus
{
    INTERNET_DISCONNECTED = 0, ///< No internet connection
    INTERNET_CONNECTED = 1,    ///< Internet connection active
};

bool internet_status = INTERNET_DISCONNECTED; ///< Current internet status

/**
 * @brief Checks internet connectivity by pinging a known IP address
 *
 * @return 1 if internet is connected, 0 if disconnected
 */
int app_check_internet_status(IPAddress ip, uint16_t port)
{
    return helper.check_internet_status(ip, port);
}

/**
 * @brief Attempts to recover from internet disconnection by restarting WiFi
 *
 * Logs the event and increments the WiFi restart counter.
 */
void app_recover_offline()
{
    DBGprint;
    Serial.println("Client offline -> reconnect to WiFi");
    logger.notice("Client offline -> reconnect to WiFi");
    esp_status_counter_wifi_restart++;

    WiFi.disconnect();
    delay(2000);
    WiFi.reconnect();
}

///////////////////// MQTT CLIENT ////////////////////

#include "mqtt.h"

MQTT mqtt; ///< MQTT configuration and helper class

WiFiClient mqttClient;                ///< WiFi client for MQTT connection
PubSubClient mqtt_client(mqttClient); ///< MQTT client instance

JsonDocument json_mqtt; ///< JSON document for MQTT messages

// RAW dedup / retained handling
static bool    g_allow_retained_once = true;    // nach (Re)Connect einmal erlauben
static bool    g_allow_raw_first = true;    // nach (Re)connect einmal raw erlauben (retained)
static int64_t g_last_raw_meas_epoch = -1;  // letzte akzeptierte Messzeit (epoch seconds)

///////////////////// HELPER FUNCTIONS ////////////////////

/**
 * @brief Prints ESP32 system status information
 *
 * Logs comprehensive system information including:
 * - PSRAM availability
 * - Heap memory status
 * - WiFi reconnection counters
 * - LibreLinkUp authentication counters
 * - Configuration states (OTA, MQTT, WireGuard)
 * - Network information
 */
void esp_status()
{
    if (!psramFound())
    {
        logger.notice("No PSRAM available!");
    }
    else
    {
        logger.notice("PSRAM available!");
    }

    logger.notice("===== Heap Memory Status =====");
    logger.notice("Total free heap: %d Bytes", esp_get_free_heap_size());
    logger.notice("Largest free block: %d Bytes", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    logger.notice("Internal RAM (DMA capable): %d Bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    logger.notice("PSRAM available: %d Bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    logger.notice("==============================");

    logger.notice("WiFi Reconnects : %d", esp_status_counter_wifi_restart);
    logger.notice("LLU count reAuth: %d", esp_status_counter_llu_reauth);
    logger.notice("LLU count reTou : %d", esp_status_counter_llu_retou);
    logger.notice("OTA Server      : %d", settings.config.ota_update);
    logger.notice("MQTT Mode       : %d", settings.config.mqtt_mode);
    logger.notice("MQTT Master Mode: %d", settings.config.mqtt_master_mode);
    logger.notice("WG Mode         : %d", settings.config.wg_mode);
    logger.notice("Local Time      : %s", helper.get_esp_time_date().c_str());
    logger.notice("TimeZone        : %d", settings.config.timezone);
    logger.notice("Brightness LCD  : %d", settings.config.brightness);
    logger.notice("MQTT Client Name: %s", mqtt.mqtt_client_name.c_str());

    if (wg.is_initialized())
    {
        logger.notice("WG connected! IP: %s, Gateway: %s, SubNet: %s, dnsIP: %s",
                      local_ip.toString(), WiFi.gatewayIP().toString(),
                      WiFi.subnetMask().toString(), WiFi.dnsIP().toString());
    }
    else
    {
        logger.notice("WG not connected! IP: %s, Gateway: %s, SubNet: %s, dnsIP: %s",
                      WiFi.localIP().toString(), WiFi.gatewayIP().toString(),
                      WiFi.subnetMask().toString(), WiFi.dnsIP().toString());
    }
}

///////////////////// BACKLIGHT CONTROL ////////////////////

///////////////////// MQTT FUNCTIONS ////////////////////

/**
 * @brief Publishes current glucose data and system status to MQTT broker
 *
 * Publishes two messages:
 * 1. Glucose data (measurement, trend, system config)
 * 2. Network status (IP, SSID, RSSI)
 */
void mqtt_publish()
{

    // Publish glucose and system data
    json_mqtt["glucoseMeasurement"] = librelinkup.glucose_data().glucoseMeasurement;
    json_mqtt["trendArrow"] = librelinkup.glucose_data().trendArrow;
    json_mqtt["brightness"] = settings.config.brightness;
    json_mqtt["mqtt_mode"] = settings.config.mqtt_mode;
    json_mqtt["ota_server"] = settings.config.ota_update;
    json_mqtt["wireguard_mode"] = settings.config.wg_mode;

    serializeJson(json_mqtt, mqtt.mqtt_buffer);
    json_mqtt.clear();
    mqtt_client.publish((mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_client_data).c_str(),
                        mqtt.mqtt_buffer,
                        false);

    // Publish LibreLinkup Raw JSON data for graphing
    if (settings.config.mqtt_master_mode == true)
    {
        // In master mode, do not publish raw data
        const String &payload = librelinkup.get_last_graph_json();
        const String topic = mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data;
        /*
        logger.debug("raw len=%u topic=%s", (unsigned)payload.length(), topic.c_str());
        logger.debug("mqtt connected=%d buffer=%u",
                    mqtt_client.connected(),
                    mqtt_client.getBufferSize());   // if your PubSubClient version supports this
        */
        bool ok = mqtt_client.publish(topic.c_str(),
                                      (const uint8_t *)payload.c_str(),
                                      payload.length(),
                                      true); // retain nach Wunsch

        // logger.debug("raw publish ok=%d state=%d", ok, mqtt_client.state());
    }

    // Publish network status
    json_mqtt["IP"] = WiFi.localIP().toString();
    if (settings.config.wg_mode == 1)
    {
        json_mqtt["IP_WG"] = local_ip.toString();
    }
    else if (settings.config.wg_mode == 0)
    {
        json_mqtt["IP_WG"] = "not connected";
    }
    json_mqtt["SSID"] = WiFi.SSID();
    json_mqtt["RSSI"] = WiFi.RSSI();

    serializeJson(json_mqtt, mqtt.mqtt_buffer);

    Json_Buffer_Info buffer_info;
    buffer_info = helper.getBufferSize(&json_mqtt);

    json_mqtt.clear();
    mqtt_client.publish((mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_client_network).c_str(),
                        mqtt.mqtt_buffer,
                        false);
}

/**
 * @brief Updates MQTT broker if MQTT mode is enabled
 */
void update_mqtt_publish()
{
    if (settings.config.mqtt_mode == 1)
    {
        mqtt_publish();
    }
}

/**
 * @brief MQTT message received callback
 *
 * Handles incoming MQTT commands from broker.
 * Supported commands:
 * - "reset": Restart ESP32
 * - "brightness": Set LCD brightness
 * - "ota_server_mode": Enable/disable OTA server
 * - "wg_mode": Enable/disable WireGuard VPN
 * - "mqtt_mode": Enable/disable MQTT publishing
 *
 * @param[in] topic   MQTT topic of received message
 * @param[in] payload Message payload
 * @param[in] length  Payload length
 */
void mqtt_callback(char *topic, byte *payload, unsigned int length)
{

    if (ota_in_progress == true)
    {
        return; // <<< ignor all
    }

    String t(topic);

    // ---------- TOPICS ----------
    String topic_raw = mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data;
    String topic_cmd = mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic;

    logger.notice("MQTT RX topic=%s len=%u", t.c_str(), (unsigned)length);

    // =========================================================
    // 1) RAW DATA vom MASTER (Client-Mode)
    // =========================================================

    if (t == topic_raw && settings.config.mqtt_master_mode == false)
    {
        logger.notice("MQTT raw data received");

        bool ok = librelinkup.ingest_graph_json(payload, length);
        if (!ok) {
            logger.notice("MQTT raw ingest failed");
            return;
        }

        // --- dedup via measurement timestamp ---------------------------------
        // Use the measurement timestamp parsed from the incoming JSON.
        // (This is what you already convert later in update_glucose_data())
        const String &ts = librelinkup.glucose_data().str_measurement_timestamp;

        int64_t meas_epoch = (int64_t)helper.convertStrToUnixTime(ts);

        // If parsing fails, be conservative: ignore (prevents random double triggers)
        if (meas_epoch <= 0) {
            logger.notice("MQTT raw ignored: invalid meas timestamp '%s'", ts.c_str());
            return;
        }

        // 1) allow exactly one RAW message after (re)connect (retained)
        if (g_allow_raw_first) {
            g_allow_raw_first = false;
            g_last_raw_meas_epoch = meas_epoch;

            logger.notice("MQTT raw accepted (first/retained) meas_epoch=%lld", (long long)meas_epoch);

            flag_mqtt_master_rx = true;
            app_fsm_notify_mqtt_master_rx(g_fsm);
            return;
        }

        // 2) afterwards accept only *strictly newer* measurement times
        /*
        if (g_last_raw_meas_epoch >= 0 && meas_epoch <= g_last_raw_meas_epoch) {
            logger.notice("MQTT raw ignored (duplicate/old) meas_epoch=%lld last=%lld",
                        (long long)meas_epoch, (long long)g_last_raw_meas_epoch);
            return;
        }*/

        g_last_raw_meas_epoch = meas_epoch;

        logger.notice("MQTT raw accepted (new) meas_epoch=%lld", (long long)meas_epoch);

        flag_mqtt_master_rx = true; // kept for backward compatibility
        app_fsm_notify_mqtt_master_rx(g_fsm);
        return; // <<< GANZ WICHTIG
    }

    // =========================================================
    // 2) COMMANDS (dein bestehender Code)
    // =========================================================
    if (t == topic_cmd)
    {

        mqtt.mqtt_incomming_cmd = "";

        // Payload → String
        for (unsigned int i = 0; i < length; i++)
        {
            mqtt.mqtt_incomming_cmd += (char)payload[i];
        }

        DeserializationError error = deserializeJson(json_mqtt, mqtt.mqtt_incomming_cmd);
        if (error)
        {
            logger.notice("CMD deserialize failed: %s", error.f_str());
            mqtt.mqtt_incomming_cmd = "";
            return;
        }

        const char *cmd = json_mqtt["cmd"];
        float parameter1 = json_mqtt["parameter1"];
        float parameter2 = json_mqtt["parameter2"];

        logger.notice("CMD=%s p1=%.2f p2=%.2f", cmd, parameter1, parameter2);

        bool cmd_ok = false;

        // ---------- COMMAND HANDLING ----------
        if (strcmp(cmd, "reset") == 0)
        {
            cmd_ok = true;
            ESP.restart();
        }

        else if (strcmp(cmd, "brightness") == 0)
        {
            settings.config.brightness = tpanels3.set_backlight_brightness(parameter1);
            config_sleep_timer_backup = millis();
            app_fsm_notify_user_activity(g_fsm); // Notify FSM of user activity for potential state changes
            cmd_ok = true;
        }

        else if (strcmp(cmd, "ota_server_mode") == 0)
        {
            if (parameter1 == 1)
            {
                settings.config.ota_update = 1;
                server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                          { request->send(200, "text/plain", "ESP32 LibreLinkup Client"); });
                ElegantOTA.begin(&server);
                server.begin();
                cmd_ok = true;
            }
            else
            {
                settings.config.ota_update = 0;
                server.end();
                cmd_ok = true;
            }
        }

        else if (strcmp(cmd, "wg_mode") == 0)
        {
            settings.config.wg_mode = (parameter1 == 1);
            setup_wg(settings.config.wg_mode);
            cmd_ok = true;
        }

        else if (strcmp(cmd, "mqtt_mode") == 0)
        {
            settings.config.mqtt_mode = (parameter1 == 1);
            cmd_ok = true;
        }

        // ---------- ACK ----------
        json_mqtt.clear();
        json_mqtt["cmd"] = cmd;
        json_mqtt["parameter1"] = parameter1;
        json_mqtt["parameter2"] = parameter2;
        json_mqtt["cmd_ok"] = cmd_ok;

        serializeJson(json_mqtt, mqtt.mqtt_buffer);
        json_mqtt.clear();

        mqtt_client.publish(
            (mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_rec_toppic).c_str(),
            mqtt.mqtt_buffer);

        mqtt.mqtt_incomming_cmd = "";

        // optional: Status-Update nach Command
        mqtt_publish();

        return;
    }

    // =========================================================
    // 3) UNBEKANNTE TOPICS
    // =========================================================
    logger.debug("MQTT topic ignored");
}

///////////////////// LVGL FUNCTIONS ////////////////////

const int LVGL_TICK_RATE_MS = 10; ///< LVGL tick increment interval

/**
 * @brief LVGL tick task for FreeRTOS
 *
 * Increments LVGL internal tick counter every millisecond.
 * Runs in separate task for accurate timing.
 *
 * @param[in] pvParameter Task parameter (unused)
 */
void lv_tick_task(void *pvParameter)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(LVGL_TICK_RATE_MS);

    while (1)
    {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        lv_tick_inc(LVGL_TICK_RATE_MS);
    }
}

/**
 * @file callbacks.cpp
 * @brief LVGL event callbacks and chart helper functions
 * @version 1.0
 * @date 2025
 *
 * This file contains all LVGL event callback functions for user interaction
 * and chart manipulation helper functions for glucose data visualization.
 */

///////////////////// CHART HELPER FUNCTIONS ////////////////////

/**
 * @brief Finds the last valid X position in a chart series
 *
 * Iterates backwards through the chart data to find the last
 * non-NONE value position.
 *
 * @param[in] chart  LVGL chart object
 * @param[in] series Chart data series to search
 *
 * @return X position (index) of last valid point
 * @retval -1 No valid point found in series
 *
 * @note Currently unused but kept for potential future use
 */
int16_t get_last_valid_x_position(lv_obj_t *chart, lv_chart_series_t *series)
{
    uint16_t point_count = lv_chart_get_point_count(chart);
    lv_coord_t *y_array = lv_chart_get_y_array(chart, series);

    for (int i = point_count - 1; i >= 0; i--)
    {
        if (y_array[i] != LV_CHART_POINT_NONE)
        {
            return lv_chart_get_x_start_point(chart, series) + i; // Calculate last X position
        }
    }

    return -1; // No valid point found
}

/**
 * @brief Highlights the most recent glucose value on the chart
 *
 * Displays a circular marker at the last valid data point.
 * The marker color changes based on glucose range:
 * - Red: Out of target range (high or low alarm)
 * - Green: Within target range
 *
 * Position is calculated by scaling the data index to chart dimensions.
 *
 * @note Automatically hides marker if no valid value exists
 * @note Uses fixed Y-axis range of 40-225 mg/dL
 */
static void highlight_last_point()
{
    const uint8_t x_pos_offset = 24; ///< X-axis offset for marker centering
    const uint8_t y_pos_offset = 12; ///< Y-axis offset for marker centering

    // Get last stored value from array
    int16_t last_value = librelinkup.sensor_history_data().graph_data[librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE - 1];

    // Hide marker if no valid value exists
    if (last_value == LV_CHART_POINT_NONE ||
        librelinkup.sensor_history_data().graph_data[librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE - 1] == 0)
    {
        lv_obj_add_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
        return; // Exit if no value present
    }
    else
    {
        lv_obj_clear_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
    }

    // Calculate X position (scale point index to chart width)
    lv_coord_t chart_width = lv_obj_get_width(ui_Chart_Glucose_5Min);
    uint16_t last_index = librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE - 1;

    lv_coord_t x_pos = ((chart_width * last_index) /
                        (librelinkup.GRAPHDATAARRAYSIZE + librelinkup.GRAPHDATAARRAYSIZE_PLUS_ONE)) -
                       x_pos_offset;

    // Calculate Y position (scale Y value to chart height)
    lv_coord_t y_min = 40;  ///< Minimum Y-axis value (mg/dL)
    lv_coord_t y_max = 225; ///< Maximum Y-axis value (mg/dL)
    lv_coord_t chart_height = lv_obj_get_height(ui_Chart_Glucose_5Min);
    lv_coord_t y_pos = (chart_height - ((last_value - y_min) * chart_height) / (y_max - y_min)) - y_pos_offset;

    // Set marker color based on glucose range
    if (last_value >= librelinkup.glucose_data().glucosetargetHigh ||
        last_value <= librelinkup.glucose_data().glucoseAlarmLow)
    {
        lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker,
                                  lv_palette_main(LV_PALETTE_RED), 0);
    }
    else
    {
        lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker,
                                  lv_palette_main(LV_PALETTE_GREEN), 0);
    }

    // Set marker position
    lv_obj_set_pos(ui_Chart_Glucose_5Min_last_point_marker, x_pos, y_pos);

    // Move marker to foreground layer
    lv_obj_move_foreground(ui_Chart_Glucose_5Min_last_point_marker);
}

///////////////////// USER INTERACTION CALLBACKS ////////////////////

/**
 * @brief Brightness toggle callback for long press events
 *
 * Handles backlight control on long press:
 * - If OFF (brightness = 0): Fade in to standard brightness
 * - If ON: Log that it's already on (no action)
 *
 * @param[in] event LVGL event data (unused)
 *
 * @note Uses PWM fade-in effect with 10ms steps
 * @note Standard brightness is defined by TRGB_STD_BACKLIGHT_BRIGHTNESS
 *
 * @see TRGB_STD_BACKLIGHT_BRIGHTNESS
 */
static void brightness_on_off_cb(lv_event_t *event)
{
    logger.notice("Button Longpress for Brightness ON/OFF triggered!");
    app_fsm_notify_user_activity(g_fsm); // Notify FSM of user activity for potential state changes

    if (settings.config.brightness == 0)
    {
        logger.notice("Turn LCD backlight ON to %d", TRGB_STD_BACKLIGHT_BRIGHTNESS);

        // Fade in backlight smoothly
        for (uint8_t i = 0; i < TRGB_STD_BACKLIGHT_BRIGHTNESS; i++)
        {
            ledcWrite(0, i); // PWM channel 0, value 0-255
            delay(10);
        }
        settings.config.brightness = TRGB_STD_BACKLIGHT_BRIGHTNESS;
    }
    else
    {
        logger.notice("Turn LCD backlight already ON");

        /* Optional: Fade out code (currently disabled)
        for(uint8_t i=settings.config.brightness; i>0; i--){
            ledcWrite(0, i);
            delay(10);
        }
        ledcWrite(0, 0);
        settings.config.brightness = 0;
        */
    }
}

/**
 * @brief Touch gesture callback for screen navigation
 *
 * Handles swipe gestures in the upper screen area (y < 100 pixels):
 * - LEFT swipe: Navigate forward (Login → Debug → Main → Login)
 * - RIGHT swipe: Navigate backward (Main → Debug → Login → Main)
 * - TOP/BOTTOM: Reserved for future use
 *
 * @param[in] event LVGL event data containing touch information
 *
 * @note Only responds to gestures in upper 100px to avoid conflicts with chart interaction
 * @note Gestures below y=100 are ignored
 *
 * @warning Touch point is screen coordinates, not relative to any object
 */
static void touch_gesture_cb(lv_event_t *event)
{
    // Get current input device and position
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev)
        return;

    // Notify FSM of user activity for potential state changes
    app_fsm_notify_user_activity(g_fsm);

    lv_point_t p;
    lv_indev_get_point(indev, &p); // Screen coordinates

    // Only respond to touches in upper area (above 100px)
    if (p.y > 100)
    {
        // Below gesture zone → ignore
        return;
    }

    // Determine gesture direction
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    logger.debug("Touch gesture dir: %d", dir);

    // Get current active screen
    lv_obj_t *active = lv_scr_act();

    switch (dir)
    {
    case LV_DIR_LEFT:
        logger.debug("Touch gesture: left");
        if (active == ui_Login_screen)
        {
            lv_disp_load_scr(ui_Debug_screen);
        }
        else if (active == ui_Debug_screen)
        {
            lv_disp_load_scr(ui_Main_screen);
        }
        else if (active == ui_Main_screen)
        {
            lv_disp_load_scr(ui_Login_screen);
        }
        break;

    case LV_DIR_RIGHT:
        logger.debug("Touch gesture: right");
        if (active == ui_Main_screen)
        {
            lv_disp_load_scr(ui_Debug_screen);
        }
        else if (active == ui_Debug_screen)
        {
            lv_disp_load_scr(ui_Login_screen);
        }
        else if (active == ui_Login_screen)
        {
            lv_disp_load_scr(ui_Main_screen);
        }
        break;

    case LV_DIR_TOP:
        logger.debug("Touch gesture: top");
        // Reserved for future features
        break;

    case LV_DIR_BOTTOM:
        logger.debug("Touch gesture: bottom");
        // Reserved for future features
        break;

    default:
        break;
    }
}

///////////////////// BUTTON CALLBACKS ////////////////////

/**
 * @brief WireGuard VPN toggle button callback
 *
 * Handles WireGuard button click events:
 * - Toggles VPN mode on/off (XOR operation)
 * - Saves configuration to file
 * - Calls setup_wg() to apply changes
 *
 * @param[in] event LVGL event data
 *
 * @note Configuration is immediately saved to persistent storage
 * @note VPN connection may take a few seconds to establish/tear down
 *
 * @see setup_wg()
 */
static void btn_wireguard_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CLICKED)
    {
        LV_LOG_USER("Clicked");
        logger.debug("Wireguard Mode Button clicked");

        settings.config.wg_mode ^= 1; // Toggle: 0→1 or 1→0
        settings.saveConfiguration(settings.config_filename, settings.config);
        setup_wg(settings.config.wg_mode);
    }
    else if (code == LV_EVENT_VALUE_CHANGED)
    {
        LV_LOG_USER("Toggled");
    }
}

/**
 * @brief On-screen keyboard event callback
 *
 * Hides the virtual keyboard when user is done:
 * - READY event: User pressed "OK" or equivalent
 * - CANCEL event: User cancelled input
 *
 * @param[in] e LVGL event data containing keyboard reference
 *
 * @note Keyboard is hidden by setting LV_OBJ_FLAG_HIDDEN flag
 */
static void ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ui_kb = (lv_obj_t *)lv_event_get_target(e);

    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    {
        lv_obj_add_flag(ui_kb, LV_OBJ_FLAG_HIDDEN); // Hide keyboard
    }
}

/**
 * @brief MQTT connection toggle button callback
 *
 * Handles MQTT button click events:
 * - Connects/disconnects MQTT client
 * - Unsubscribes from topics on disconnect
 * - Saves configuration
 *
 * @param[in] event LVGL event data
 *
 * @note On disconnect, unsubscribes from command topic first
 * @note Configuration is saved regardless of connection state
 *
 * @see mqtt_callback()
 */
static void btn_mqtt_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CLICKED)
    {
        LV_LOG_USER("Clicked");
        logger.debug("MQTT Mode Button clicked");
        settings.saveConfiguration(settings.config_filename, settings.config);

        if (mqtt.mqtt_enable == 0)
        {
            mqtt.mqtt_enable = 1;
            logger.debug("MQTT client connect");
        }
        else if (mqtt.mqtt_enable == 1)
        {
            mqtt.mqtt_enable = 0;
            logger.debug("MQTT client disconnected");
            mqtt_client.unsubscribe((mqtt.mqtt_base + "/" + mqtt.mqtt_client_name +
                                     mqtt.mqtt_subscibe_toppic)
                                        .c_str());
            mqtt_client.disconnect();
        }
    }
    else if (code == LV_EVENT_VALUE_CHANGED)
    {
        LV_LOG_USER("Toggled");
    }
}

/**
 * @brief OTA update server toggle button callback
 *
 * Handles OTA button click events:
 * - Starts/stops ElegantOTA web server
 * - Saves configuration state
 * - Calls setup_OTA() to apply changes
 *
 * @param[in] event LVGL event data
 *
 * @note OTA server runs on port 80
 * @note WiFi scan task is started/stopped along with OTA server
 *
 * @see setup_OTA()
 */
static void btn_ota_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CLICKED)
    {
        LV_LOG_USER("Clicked");
        logger.debug("OTA Mode Button clicked");
        settings.saveConfiguration(settings.config_filename, settings.config);

        if (settings.config.ota_update == 1)
        {
            settings.config.ota_update = 0;
            setup_OTA(settings.config.ota_update);
        }
        else if (settings.config.ota_update == 0)
        {
            settings.config.ota_update = 1;
            setup_OTA(settings.config.ota_update);
        }
    }
    else if (code == LV_EVENT_VALUE_CHANGED)
    {
        LV_LOG_USER("Toggled");
    }
}

/**
 * @brief Login button callback
 *
 * Handles login button click:
 * - Extracts email and password from text areas
 * - Saves credentials to configuration
 * - Switches to main screen
 *
 * @param[in] event LVGL event data
 *
 * @note Credentials are stored in plain text in settings file
 * @note No validation is performed - validation happens during API call
 *
 * @warning Password is logged to serial console (remove in production!)
 */
static void btn_login_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_CLICKED)
    {
        // Extract credentials from text areas
        const char *email = lv_textarea_get_text(ui_ta_email);
        const char *password = lv_textarea_get_text(ui_ta_password);

        // Store in configuration
        settings.config.login_email = email;
        settings.config.login_password = password;
        librelinkup.set_credentials(settings.config.login_email, settings.config.login_password);

        Serial.printf("Entered Email: %s\n", settings.config.login_email);
        Serial.printf("Entered Password: %s\n", settings.config.login_password); // TODO: Remove in production

        // Save configuration to file
        settings.saveConfiguration(settings.config_filename, settings.config);

        // Switch to main screen
        lv_disp_load_scr(ui_Main_screen);
    }
}

///////////////////// CHART INTERACTION CALLBACKS ////////////////////

/// Last X-coordinate of touch on chart (for tracking drag operations)
static int last_x = -1;

/**
 * @brief Touch event callback for glucose chart interaction
 *
 * Displays glucose value at the touched position on the chart.
 * Features:
 * - Shows circular marker at touch point
 * - Updates main label with glucose value
 * - Color-codes marker (red=out of range, green=in range)
 * - Scales touch position to data index
 *
 * Algorithm:
 * 1. Convert screen coordinates to chart-relative coordinates
 * 2. Map X-coordinate to data point index
 * 3. Retrieve glucose value at that index
 * 4. Calculate Y-position by scaling glucose value to chart height
 * 5. Position marker and update display
 *
 * @param[in] e LVGL event data containing touch information
 *
 * @note Touch position is relative to chart object, not screen
 * @note Y-axis range is fixed at 40-225 mg/dL
 * @note Marker is hidden if touched position has no valid data
 *
 * @warning Index clamping prevents out-of-bounds array access
 */
static void touch_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Calculate X coordinate relative to chart
    lv_coord_t chart_x = lv_obj_get_x(ui_Chart_Glucose_5Min);
    lv_coord_t relative_x = point.x - chart_x;

    // Convert X coordinate to data point index
    int num_points = lv_chart_get_point_count(ui_Chart_Glucose_5Min);
    int index = (relative_x * num_points) / lv_obj_get_width(ui_Chart_Glucose_5Min);

    // Clamp index to valid range
    if (index >= num_points)
        index = num_points - 1;
    if (index < 0)
        index = 0;

    // Get Y value from chart series
    int16_t value = librelinkup.sensor_history_data().graph_data[index];

    // Hide marker if no valid value
    if (value == LV_CHART_POINT_NONE || value == 0)
    {
        lv_obj_add_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    else
    {
        lv_obj_clear_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
    }

    // Get chart dimensions (without padding)
    lv_coord_t chart_height = lv_obj_get_height(ui_Chart_Glucose_5Min);
    lv_coord_t y_min = 40;  ///< Chart Y-axis minimum (mg/dL)
    lv_coord_t y_max = 225; ///< Chart Y-axis maximum (mg/dL)

    // Scale Y value to chart height
    lv_coord_t y_pos = chart_height - ((value - y_min) * chart_height) / (y_max - y_min);

    // Offset correction for exact positioning
    const uint8_t x_pos_offset = 24; ///< X-axis centering offset
    const uint8_t y_pos_offset = 6;  ///< Y-axis centering offset

    // Set marker color based on glucose range
    if (value >= librelinkup.glucose_data().glucosetargetHigh ||
        value <= librelinkup.glucose_data().glucoseAlarmLow)
    {
        lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker,
                                  lv_palette_main(LV_PALETTE_RED), 0);
    }
    else
    {
        lv_obj_set_style_bg_color(ui_Chart_Glucose_5Min_last_point_marker,
                                  lv_palette_main(LV_PALETTE_GREEN), 0);
    }

    // Set marker position
    lv_obj_set_pos(ui_Chart_Glucose_5Min_last_point_marker,
                   relative_x - x_pos_offset,
                   y_pos - y_pos_offset);

    // Move marker to foreground layer
    lv_obj_move_foreground(ui_Chart_Glucose_5Min_last_point_marker);

    // Update main display with touched value
    uint8_t mode = 1; // Display mode: show value
    uint8_t color = (value >= librelinkup.glucose_data().glucosetargetHigh ||
                     value <= librelinkup.glucose_data().glucoseAlarmLow)
                        ? 4  // GlucoseLabelColor RED
                        : 1; // GlucoseLabelColor WHITE
    uint16_t glucose_value = value;

    draw_labels(mode, color, glucose_value,
                librelinkup.glucose_data().str_trendArrow,
                librelinkup.glucose_data().str_TrendMessage, 0);

    // Debug output
    logger.notice("Touch X (rel): %d, Index: %d, Value: %d, Y: %d",
                  relative_x, index, value, y_pos);

    // Store last X position for potential drag tracking
    last_x = relative_x;
}

/**
 * @page callbacks_usage Callback Usage Guide
 *
 * @section callbacks_registration Event Registration
 *
 * All callbacks must be registered in setup() after ui_init():
 *
 * @code
 * // Brightness control
 * lv_obj_add_event_cb(ui_Main_screen, brightness_on_off_cb,
 *                     LV_EVENT_PRESSED, NULL);
 *
 * // Screen navigation
 * lv_obj_add_event_cb(ui_Main_screen, touch_gesture_cb,
 *                     LV_EVENT_GESTURE, NULL);
 *
 * // Chart interaction
 * lv_obj_add_event_cb(ui_Chart_Glucose_5Min, touch_event_cb,
 *                     LV_EVENT_PRESSING, NULL);
 *
 * // Button callbacks
 * lv_obj_add_event_cb(ui_btn_wireguard, btn_wireguard_cb,
 *                     LV_EVENT_ALL, NULL);
 * @endcode
 *
 * @section callbacks_flow Event Flow
 *
 * 1. **User touches screen** → Touch interrupt fires
 * 2. **LVGL reads touch data** → my_touchpad_read()
 * 3. **LVGL processes gestures** → Gesture detection
 * 4. **Callback fires** → Your callback function executes
 * 5. **UI updates** → lv_timer_handler() renders changes
 *
 * @section callbacks_best_practices Best Practices
 *
 * **DO:**
 * - Keep callbacks short and fast
 * - Use logger for debugging
 * - Return early on invalid conditions
 * - Use const for read-only data
 *
 * **DON'T:**
 * - Block with long delays
 * - Perform heavy computations
 * - Access NULL pointers without checking
 * - Forget to save configuration when needed
 */

///////////////////// STATUS INDICATION ////////////////////

/**
 * @brief Shows/hides LCD API activity status indicator
 *
 * Displays a colored asterisk (*) in the corner to indicate LibreLinkUp
 * API activity and status.
 *
 * @param[in] on_off Enable/disable indicator
 *                   - 0: Hide indicator (blank)
 *                   - 1: Show indicator with color
 * @param[in] color  Color code for indicator
 *                   - 0: Yellow (0xFFFF00) - Warning/Processing
 *                   - 1: White (0xFFFFFF) - Normal
 *                   - 2: Red (0xFF0000) - Error
 *                   - Other: Default to white
 *
 * @note Calls lv_timer_handler() to update display immediately
 * @note 5ms delay ensures display update completes
 */
void lcd_status_indication(bool on_off, uint8_t color)
{
    if (on_off == 0)
    {
        lv_label_set_text(ui_Label_LiebreViewAPIActivity, " ");
    }
    else if (on_off == 1)
    {
        switch (color)
        {
        case 0:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity,
                                        lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case 1:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity,
                                        lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case 2:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity,
                                        lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;

        default:
            lv_obj_set_style_text_color(ui_Label_LiebreViewAPIActivity,
                                        lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        }
        lv_label_set_text(ui_Label_LiebreViewAPIActivity, "*");
    }
    lv_timer_handler();
    delay(5);
}

///////////////////// LIBRELINKUP CHART FUNCTIONS ////////////////////

/**
 * @brief Draws sensor validity progress bar
 *
 * Displays remaining sensor lifetime using appropriate progress bar:
 * - Days mode (14 or 15): Full sensor lifetime visualization
 * - Hours mode (24): Last day countdown
 * - Minutes mode (60): Final hour countdown
 *
 * Automatically switches between modes based on remaining time.
 *
 * @note Progress bar values are incremented by 1 for display purposes
 * @note Calls lv_timer_handler() to update UI immediately
 *
 * @see switch_sensor_valid_progress_bar()
 * @see update_chart_valid_values()
 */
void draw_chart_sensor_valid()
{
    // Rohwerte (wie berechnet)
    int rawDays = librelinkup.sensor_lifetime().sensor_valid_days;
    int rawHours = librelinkup.sensor_lifetime().sensor_valid_hours;
    int rawMinutes = librelinkup.sensor_lifetime().sensor_valid_minutes;

    // Expired/invalid detection (before +1!)
    bool expired = (rawDays < 0) || (rawHours < 0) || (rawMinutes < 0);

    if (expired)
    {
        switch_sensor_valid_progress_bar(NULL); // hide all bars
        update_chart_valid_values(&dayBar14, -1);
        update_chart_valid_values(&dayBar15, -1);
        update_chart_valid_values(&hourBar, -1);
        update_chart_valid_values(&minuteBar, -1);
        return;
    }

    // Displayvalues ("+1 like App")
    int days = rawDays + 1;
    int hours = rawHours + 1;
    int minutes = rawMinutes + 1;

    // --------------------------
    // DAYS MODE
    // --------------------------
    if (rawDays > 0)
    {
        if (librelinkup.sensor_data().sensor_runtime == 14 * 86400)
        {
            switch_sensor_valid_progress_bar(&dayBar14);
            update_chart_valid_values(&dayBar14, days);
        }
        else if (librelinkup.sensor_data().sensor_runtime == 15 * 86400)
        {
            switch_sensor_valid_progress_bar(&dayBar15);
            update_chart_valid_values(&dayBar15, days);
        }
        else
        {
            // Fallback: if runtime unknown
            switch_sensor_valid_progress_bar(&dayBar15);
            update_chart_valid_values(&dayBar15, days);
        }
        return;
    }

    // --------------------------
    // HOURS MODE
    // --------------------------
    if (rawHours > 0)
    {
        switch_sensor_valid_progress_bar(&hourBar);
        update_chart_valid_values(&hourBar, hours);
        return;
    }

    // --------------------------
    // MINUTES MODE
    // --------------------------
    if (rawMinutes >= 0)
    {
        switch_sensor_valid_progress_bar(&minuteBar);
        update_chart_valid_values(&minuteBar, minutes);
        return;
    }

    // (optional) if everything is 0 and you prefer showing "expired":
    // switch_sensor_valid_progress_bar(NULL);
}

/**
 * @brief Formats timestamp for chart axis labels
 *
 * Creates special markers for significant times:
 * - "00:00*" for midnight (day change marker)
 * - "12:00" for noon
 * - "HH:00" for 3-hour intervals (00:00, 03:00, 06:00, etc.)
 * - "" (empty) for all other times
 *
 * @param[out] buffer      Output buffer for formatted string
 * @param[in]  buffer_size Size of output buffer (must be ≥6 for "HH:MM\0")
 * @param[in]  timestamp   Unix timestamp to format
 *
 * @note Uses localtime() which respects timezone settings
 * @note Midnight marker (*) helps identify day boundaries
 *
 * @warning Buffer must have space for null terminator
 */
void format_time_label(char *buffer, size_t buffer_size, time_t timestamp)
{
    struct tm *tm_info = localtime(&timestamp);
    if (tm_info->tm_hour == 0 && tm_info->tm_min == 0)
    {
        snprintf(buffer, buffer_size, "00:00*"); // Day change
    }
    else if (tm_info->tm_hour == 12 && tm_info->tm_min == 0)
    {
        snprintf(buffer, buffer_size, "12:00"); // Noon
    }
    else if (tm_info->tm_min == 0 && (tm_info->tm_hour % 3 == 0))
    {
        snprintf(buffer, buffer_size, "%02d:00", tm_info->tm_hour); // Every 3 hours
    }
    else
    {
        snprintf(buffer, buffer_size, ""); // No label
    }
}

/**
 * @brief Adds X-axis time labels to glucose chart
 *
 * Displays three time labels on the chart:
 * - Start time (left)
 * - Middle time (center)
 * - End time (right)
 *
 * Timestamps are retrieved from sensor history data and formatted
 * using helper.format_time().
 *
 * @note Labels are static char arrays to maintain persistence
 * @note Middle timestamp is calculated from array midpoint
 * @note Empty data points may cause incorrect middle calculation
 *
 * @see helper.format_time()
 */
void add_axis_labels()
{

    // X-axis: Only three labels (start, middle, end)
    static char labels[3][6]; // "HH:MM" + null terminator
    uint8_t data_count = librelinkup.check_graphdata();
    if (data_count == 0)
    {
        lv_label_set_text(ui_Chart_x_label_start, "--:--");
        lv_label_set_text(ui_Chart_x_label_middle, "--:--");
        lv_label_set_text(ui_Chart_x_label_end, "--:--");
        return;
    }

    // Determine first, middle and last timestamps
    uint32_t first_timestamp = librelinkup.sensor_history_data().timestamp[0];
    uint32_t middle_timestamp = librelinkup.sensor_history_data().timestamp[data_count / 2];
    uint32_t last_timestamp = librelinkup.sensor_history_data().timestamp[data_count - 1];

    // Format timestamps
    helper.format_time(labels[0], sizeof(labels[0]), first_timestamp);  // First timestamp
    helper.format_time(labels[1], sizeof(labels[1]), middle_timestamp); // Middle timestamp
    helper.format_time(labels[2], sizeof(labels[2]), last_timestamp);   // Last timestamp

    // Set labels
    lv_label_set_text(ui_Chart_x_label_start, labels[0]);  // Leftmost
    lv_label_set_text(ui_Chart_x_label_middle, labels[1]); // Center
    lv_label_set_text(ui_Chart_x_label_end, labels[2]);    // Rightmost
}

/**
 * @brief Draws glucose chart with historical data
 *
 * Renders glucose chart based on selected mode:
 * - Mode 0: Target limit lines only
 * - Mode 1: Historical glucose data only
 * - Mode 3: Both limits and data (complete chart)
 *
 * Chart features:
 * - Automatic right-alignment for new sensors (first 12 hours)
 * - Left-aligned scrolling for established sensors
 * - Color-coded values (green=in range, red=out of range)
 * - Gap handling for missing data points
 * - Last point highlighting
 * - Dynamic X-axis labels
 *
 * @param[in] mode              Drawing mode (0/1/3)
 *
 * @note Mode 3 is most commonly used for complete display
 * @note Chart invalidation triggers LVGL redraw
 * @note Index 141 is reserved for current measurement
 *
 * @see highlight_last_point()
 * @see add_axis_labels()
 */
void draw_chart_glucose_data(uint8_t mode)
{
    if (mode == 0 || mode == 3)
    {
        // Draw limit lines
        lv_chart_set_x_start_point(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, 0);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_upperlimit,
                               librelinkup.glucose_data().glucosetargetHigh);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_lowerlimit,
                               librelinkup.glucose_data().glucosetargetLow);
    }

    if (mode == 1 || mode == 3)
    {
        uint16_t glucose_value = 0;
        uint8_t data_count = librelinkup.check_graphdata();

        // Clear all previous points
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, LV_CHART_POINT_NONE);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_alert, LV_CHART_POINT_NONE);
        lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_last, LV_CHART_POINT_NONE);

        // Iterate through all historical data
        uint32_t sensor_active_time = (librelinkup.sensor_lifetime().sensor_valid_days * 24 * 60 * 60) +
                                      (librelinkup.sensor_lifetime().sensor_valid_hours * 60 * 60) +
                                      (librelinkup.sensor_lifetime().sensor_valid_minutes * 60);

        if (sensor_active_time <= librelinkup.TIMEFULLGRAPHDATA)
        {
            // Sensor within first 12 hours - fill right-aligned
            for (int i = 0; i < librelinkup.GRAPHDATAARRAYSIZE; i++)
            {
                uint8_t index = (librelinkup.GRAPHDATAARRAYSIZE - 1) - i;

                // Safety check for array bounds
                if (index < 0 || index >= librelinkup.GRAPHDATAARRAYSIZE)
                    continue;

                glucose_value = librelinkup.sensor_history_data().graph_data[index];

                if (glucose_value != 0)
                {
                    // Check if value is out of target range
                    if (glucose_value > librelinkup.glucose_data().glucosetargetHigh ||
                        glucose_value < librelinkup.glucose_data().glucosetargetLow)
                    {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_alert,
                                                 index, glucose_value);
                    }
                    else
                    {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,
                                                 index, glucose_value);
                    }
                }
                else
                {
                    // No data - create gap
                    lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,
                                             index, LV_CHART_POINT_NONE);
                    lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_alert,
                                             index, LV_CHART_POINT_NONE);
                }
            }
        }
        else
        {
            // Sensor active longer than 12h - startup time handling
            //logger.debug("Draw graph startup time");

            for (int i = 0; i < data_count; i++)
            {
                uint8_t index = (data_count - 1) - i;
                glucose_value = librelinkup.sensor_history_data().graph_data[index];

                if (glucose_value != 0)
                {
                    if (glucose_value > librelinkup.glucose_data().glucosetargetHigh ||
                        glucose_value < librelinkup.glucose_data().glucosetargetLow)
                    {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_alert,
                                                 (librelinkup.GRAPHDATAARRAYSIZE - 1) - i, glucose_value);
                    }
                    else
                    {
                        lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,
                                                 (librelinkup.GRAPHDATAARRAYSIZE - 1) - i, glucose_value);
                    }
                }
            }
        }

        // Set current measurement at index 141
        uint16_t last_index = librelinkup.GRAPHDATAARRAYSIZE;

        if (librelinkup.status().timestamp_status == SENSOR_TIMECODE_VALID)
        {
            librelinkup.sensor_history_data().graph_data[last_index] =
                librelinkup.glucose_data().glucoseMeasurement;

            lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,
                                     last_index, librelinkup.glucose_data().glucoseMeasurement);

            // Highlight last point
            highlight_last_point();
        }
        else
        {
            // No data → create gap
            lv_chart_set_value_by_id(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min,
                                     last_index, LV_CHART_POINT_NONE);
        }

        // Add X-axis labels
        add_axis_labels();

        // Refresh chart display
        lv_obj_invalidate(ui_Chart_Glucose_5Min);
    }
}

/**
 * @brief Updates glucose value and trend labels on main screen
 *
 * Updates main display labels with current glucose data including:
 * - Glucose value (large number)
 * - Trend arrow (direction indicator)
 * - Delta value (change from previous)
 * - Trend message (alerts/warnings)
 *
 * @param[in] mode                      Display mode
 *                                      - 0: Show dashes (no data)
 *                                      - 1: Show actual values
 * @param[in] _glucose_measurement_color Color code for glucose value
 *                                      (COLOR_WHITE/YELLOW/ORANGE/RED/BLUE)
 * @param[in] _glucose_value            Glucose value in mg/dL
 * @param[in] _trendarrow               Trend arrow string (↑ ↗ → ↘ ↓)
 * @param[in] _trendmessage             Alert/warning message
 * @param[in] delta                     Change from previous reading
 *
 * @note Mode 0 displays "---" placeholders
 * @note Mode 1 displays actual glucose data
 * @note Delta formatting: ±0, +positive, negative
 * @note Trend message "null" is treated as empty
 *
 * @see lcd_status_indication()
 */
void draw_labels(uint8_t mode, uint8_t _glucose_measurement_color,
                 uint16_t _glucose_value, String _trendarrow,
                 String _trendmessage, int16_t delta)
{

    if (mode == 0)
    {
        // Set color based on glucose level
        if (_glucose_measurement_color == 1) // GlucoseLabelColor WHITE
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else if (_glucose_measurement_color == 2) // GlucoseLabelColor YELLOW
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFF00),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else if (_glucose_measurement_color == 3) // GlucoseLabelColor ORANGE
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFA500),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else if (_glucose_measurement_color == 4) // GlucoseLabelColor RED
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFF0000),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else if (_glucose_measurement_color == 5) // GlucoseLabelColor BLUE
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0x0000FF),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        // Display placeholders
        lv_label_set_text(ui_Label_GlucoseValue, "---");
        lv_label_set_text(ui_Label_GlucoseDelta, "--- mg/dL");
        lv_label_set_text(ui_Label_GlucoseTrendArrow, "-");
    }
    else if (mode == 1)
    {
        // Set color (same logic as mode 0)
        if (_glucose_measurement_color == 1) // GlucoseLabelColor WHITE
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else if (_glucose_measurement_color == 2) // GlucoseLabelColor YELLOW
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFF00),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else if (_glucose_measurement_color == 3) // GlucoseLabelColor ORANGE
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFA500),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else if (_glucose_measurement_color == 4) // GlucoseLabelColor RED
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFF0000),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else if (_glucose_measurement_color == 5) // GlucoseLabelColor BLUE
        {
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0x0000FF),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        else
        {
            // Fallback prevents stale color if an unknown code arrives.
            lv_obj_set_style_text_color(ui_Label_GlucoseValue, lv_color_hex(0xFFFFFF),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }

        // Display glucose value
        char buf_label1[4];
        snprintf(buf_label1, 4, "%d", _glucose_value);
        lv_label_set_text(ui_Label_GlucoseValue, buf_label1);
        lv_label_set_text(ui_Label_GlucoseTrendArrow, _trendarrow.c_str());

        // Display delta with proper sign
        char buf_label3[14];
        if (delta == 0)
        {
            snprintf(buf_label3, 14, "±%d mg/dL", delta); // Plus-minus for zero
        }
        else if (delta > 0)
        {
            snprintf(buf_label3, 14, "+%d mg/dL", delta); // Plus for positive
        }
        else if (delta < 0)
        {
            snprintf(buf_label3, 14, "%d mg/dL", delta); // Minus already in value
        }
        lv_label_set_text(ui_Label_GlucoseDelta, buf_label3);
    }

    // Show trend message if available
    if (strcmp(_trendmessage.c_str(), "null") != 0)
    {
        lv_label_set_text(ui_Label_GlucoseTrendMessage, _trendmessage.c_str());
    }
    else
    {
        lv_label_set_text(ui_Label_GlucoseTrendMessage, "");
    }
}

///////////////////// ERROR HANDLING FUNCTIONS ////////////////////

/**
 * @brief Handles internet disconnection scenario
 *
 * Counts consecutive offline seconds and triggers WiFi reconnection
 * after timeout period expires.
 *
 * @note Counter resets after successful reconnection attempt
 * @note Displays current glucose data with zero delta during offline period
 * @note Increments global wifi_restart counter for statistics
 *
 * @see RECONNECT_WIFI_TIMEOUT_MS
 */
void handle_internet_disconnection()
{
    static uint8_t counter_internet_offline = 0;

    if (++counter_internet_offline == RECONNECT_WIFI_TIMEOUT_MS / 1000)
    {
        counter_internet_offline = 0;
        logger.notice("Client offline -> reconnect to WiFi");
        esp_status_counter_wifi_restart++;
        WiFi.disconnect();
        WiFi.reconnect();
    }

    draw_labels(false, librelinkup.glucose_data().measurement_color,
                librelinkup.glucose_data().glucoseMeasurement,
                librelinkup.glucose_data().str_trendArrow,
                librelinkup.glucose_data().str_TrendMessage, 0);
}

/**
 * @brief Handles LibreLinkUp API error
 *
 * Sets error status flags and triggers sensor reconnect sequence.
 *
 * @note Sets internet_status to 2 (error state)
 * @note Hides LCD status indicator
 * @note Flags sensor for reconnection attempt
 */
void handle_llu_api_error()
{
    internet_status = 2;
    lcd_status_indication(0, 1);
    librelinkup.reconnect_flag() = 1;
    logger.notice("API Error: get graph data");
}

/**
 * @brief Handles sensor reconnection scenario
 *
 * Resets reconnection flag, clears delta counter, and redraws
 * complete chart with fresh data.
 *
 * @note Called when sensor comes back online
 * @note Resets glucose_delta to prevent incorrect change display
 */
void handle_sensor_reconnect()
{
    logger.notice("Sensor reconnect!");
    librelinkup.reconnect_flag() = 0;
    glucose_delta = 0;
    draw_chart_glucose_data(3);
}

/**
 * @brief Handles invalid timestamp scenarios
 *
 * Manages different error cases:
 * - SENSOR_TIMECODE_OUT_OF_RANGE: Triggers reconnect flag
 * - SENSOR_TIMECODE_ERROR + SENSOR_NOT_AVAILABLE:
 *   - After 5 occurrences: Attempts re-authentication
 *   - After 10 occurrences: Logs restart warning (commented out)
 *
 * @note Uses static counter for error tracking
 * @note Re-auth includes Terms of Use acceptance if needed
 * @note Restart is currently disabled (commented)
 *
 * @warning Counter persists across function calls
 */
void handle_invalid_timestamp()
{

    draw_labels(false, librelinkup.glucose_data().measurement_color,
                librelinkup.glucose_data().glucoseMeasurement,
                librelinkup.glucose_data().str_trendArrow,
                librelinkup.glucose_data().str_TrendMessage, 0);

    // Keep target limit lines and data visible even if the sensor data timestamp is invalid.
    draw_chart_glucose_data(1);
    lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_5Min, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_alert, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(ui_Chart_Glucose_5Min, glucoseValueSeries_last, LV_CHART_POINT_NONE);
    
    // Hide last point marker since current measurement is not valid
    lv_obj_add_flag(ui_Chart_Glucose_5Min_last_point_marker, LV_OBJ_FLAG_HIDDEN);
    
    // Refresh chart to show limit lines and data points
    lv_obj_invalidate(ui_Chart_Glucose_5Min);

    if (librelinkup.status().timestamp_status == SENSOR_TIMECODE_OUT_OF_RANGE)
    {
        logger.notice("glucoseMeasurement: no valid sensor data");
        librelinkup.reconnect_flag() = 1;
    }
    
    /*
    if(librelinkup.status().timestamp_status == SENSOR_LOST){
        logger.notice("glucoseMeasurement: sensor lost?!");
    }*/

    static uint8_t invalid_timestamp_counter = 0;

    if (librelinkup.status().timestamp_status == SENSOR_TIMECODE_ERROR &&
        librelinkup.status().sensor_state == SENSOR_NOT_AVAILABLE)
    {

        if (++invalid_timestamp_counter == 5)
        {
            esp_status_counter_llu_reauth++;
            logger.notice("LLU Re-auth...");
            librelinkup.auth_user(settings.config.login_email, settings.config.login_password);

            if (librelinkup.login_data().user_login_status == 4)
            {
                esp_status_counter_llu_retou++;
                librelinkup.tou_user();
            }
        }

        if (invalid_timestamp_counter == 10)
        {
            invalid_timestamp_counter = 0;
            logger.notice("invalid_timestamp_counter out of range! -> call restart");
        }
    }

}

// FactoryTimestamp-driven fetch scheduling: target fetch ~5s after the NEXT expected measurement.
// This mirrors the Home Assistant addon behaviour: expected_next = last_meas + 60s + desired_lag,
// and if the cloud still returns the old measurement, poll in a short window.
static int64_t  g_last_meas_epoch = -1;          // last measurement time (UTC epoch seconds)
static uint32_t g_last_meas_changed_ms = 0;      // millis() when g_last_meas_epoch changed

// Clamp a value to a specified range
static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Parse LibreLinkUp "FactoryTimestamp" robustly.
// Accepts either:
//  - numeric seconds or milliseconds since epoch
//  - ISO-8601 strings (with 'Z' or +/-hh:mm or without TZ)
// Parse LibreLinkUp FactoryTimestamp strictly as UTC
static bool parse_factory_timestamp_utc(const String &s_in, int64_t &out_epoch_s)
{
    String s = s_in;
    s.trim();
    if (s.length() == 0) return false;

    int mon=0, day=0, year=0, hh=0, mm=0, ss=0;
    char ampm[3] = {0};

    // Format: M/D/YYYY h:mm:ss AM
    // also accepts 02/23/2026 01:28:38 PM
    if (sscanf(s.c_str(), "%d/%d/%d %d:%d:%d %2s", &mon, &day, &year, &hh, &mm, &ss, ampm) != 7)
        return false;

    // 12h -> 24h
    bool isPM = (ampm[0] == 'P' || ampm[0] == 'p');
    bool isAM = (ampm[0] == 'A' || ampm[0] == 'a');

    if (!isAM && !isPM) return false;

    if (hh == 12) hh = 0;        // 12AM -> 0, 12PM handled below
    if (isPM) hh += 12;

    struct tm t {};
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;
    t.tm_mday = day;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;
    t.tm_isdst = -1; // important: let DST be determined automatically by TZ rules

    time_t epoch = mktime(&t);   // interprets t as *local time* according to TZ rules
    if (epoch <= 0) return false;

    out_epoch_s = (int64_t)epoch;
    return (out_epoch_s > 1700000000LL);
}

/**
 * @brief Synchronizes ESP32 time with LibreLinkUp server
 *
 * Calculates time difference between local and server time,
 * then adjusts next update timer to maintain synchronization.
 *
 * @note Uses helper.TIME_DIFF_THRESHOLD for acceptable drift
 * @note Adds API fetch time to compensation if drift is significant
 * @note Updates g_timer_60000ms_backup for next scheduled update
 *
 * @see helper.synchronizeWithServer()
 */
void synchronize_time_offset_epoch()
{
    // Goal: Fetch ~5s after the NEXT expected measurement based on FactoryTimestamp + local offset (DST aware)
    constexpr int32_t  kIntervalS     = 60;
    constexpr int32_t  kDesiredLagS   = 5;
    constexpr uint32_t kPollS         = 10;
    constexpr uint32_t kPollMaxS      = 120;

    const uint32_t period_ms = g_fsm.cfg.fetch_period_ms;
    const uint32_t now_ms    = millis();
    const int64_t  now_epoch = (int64_t)time(nullptr);

    if (now_epoch < 1700000000LL) {
        logger.debug("TimeSync(factory): system time not valid -> keep default cadence");
        return;
    }

    // This is your FactoryTimestamp string (MM/DD/YYYY hh:mm:ss AM/PM)
    const String &fts = librelinkup.glucose_data().str_measurement_factorytimestamp;

    // Parse FactoryTimestamp to epoch (raw). In your project, parseTimestamp() seems to work for this.
    // If you don't have direct access to parseTimestamp() here, keep your existing parse function.
    int64_t meas_epoch_raw = 0;
    if (!parse_factory_timestamp_utc(fts, meas_epoch_raw)) {
        logger.debug("TimeSync(factory): invalid FactoryTimestamp '%s' -> keep default cadence", fts.c_str());
        return;
    }

    // Convert to UTC anchor using the locked offset (winter: 3600, summer: 7200)
    const bool   tz_locked = (librelinkup.tz_locked != 0);
    const int32_t off_s    = (int32_t)librelinkup.tz_offset_s_locked;

    int64_t meas_epoch = meas_epoch_raw;

    if (tz_locked) {
        // IMPORTANT: raw factory epoch is "local wall clock interpreted as UTC"
        // Correct it by adding the local UTC offset.
        meas_epoch = meas_epoch_raw + (int64_t)off_s;
    } else {
        // If not locked, don't guess. Better keep default cadence.
        logger.debug("TimeSync(factory): tz not locked yet -> keep default cadence (raw=%lld)", (long long)meas_epoch_raw);
        return;
    }

    const int64_t lag_s = now_epoch - meas_epoch;

    // After correction, lag should be small (seconds). If it's still huge, something is wrong.
    if (llabs(lag_s) > 900) {
        logger.debug(
            "TimeSync(factory): WARNING huge lag=%llds (now=%lld raw=%lld meas=%lld off=%lds) -> keep default cadence",
            (long long)lag_s,
            (long long)now_epoch,
            (long long)meas_epoch_raw,
            (long long)meas_epoch,
            (long)off_s
        );
        return;
    }

    // Track measurement changes for poll-window logic (use corrected meas_epoch!)
    if (g_last_meas_epoch < 0 || meas_epoch != g_last_meas_epoch) {
        g_last_meas_epoch = meas_epoch;
        g_last_meas_changed_ms = now_ms;
    }

    const int64_t expected_next = g_last_meas_epoch + kIntervalS + kDesiredLagS;

    uint32_t next_in_ms = 0;
    if (now_epoch < expected_next) {
        next_in_ms = (uint32_t)((expected_next - now_epoch) * 1000LL);
    } else {
        const uint32_t poll_age_s = (now_ms - g_last_meas_changed_ms) / 1000U;
        if (poll_age_s <= kPollMaxS) {
            next_in_ms = kPollS * 1000U;
        } else {
            next_in_ms = period_ms; // give up polling and fall back
        }
    }

    // Never allow next_in_ms == period_ms, otherwise last_fetch_ms becomes "now_ms" and we drift to pure 60s cadence.
    next_in_ms = clamp_u32(next_in_ms, 0, (period_ms > 0 ? period_ms - 1 : 0));
    
    // Force the FSM to fire the next fetch in next_in_ms
    g_fsm.last_fetch_ms = now_ms - (period_ms - next_in_ms);
    g_fsm.fetch_schedule_override = true;

    logger.debug(
        "TimeSync(factory): now=%lld raw=%lld meas=%lld lag=%llds off=%lds desired=%ds expected_next=%lld next_in=%lums poll_age=%lus",
        (long long)now_epoch,
        (long long)meas_epoch_raw,
        (long long)g_last_meas_epoch,
        (long long)lag_s,
        (long)off_s,
        (int)kDesiredLagS,
        (long long)expected_next,
        (unsigned long)next_in_ms,
        (unsigned long)((now_ms - g_last_meas_changed_ms) / 1000U)
    );
}

/**
 * @brief Updates trend message based on sensor state
 *
 * Sets appropriate message for current sensor status:
 * - SENSOR_EXPIRED: "sensor expired!"
 * - SENSOR_NOT_AVAILABLE: "no active sensor"
 * - SENSOR_STARTING: "sensor ready in X min" (countdown)
 * - SENSOR_READY: "" (no message)
 *
 * @note Warmup countdown calculated from sensor activation time
 * @note Message stored in librelinkup.glucose_data().str_TrendMessage
 *
 * @warning Buffer size limited to 30 characters
 */
void update_trend_message()
{
    char buffer[30];
    int remaining_time = 0;

    switch (librelinkup.status().sensor_state)
    {
    case SENSOR_EXPIRED:
        librelinkup.glucose_data().str_TrendMessage = "sensor expired!";
        logger.notice("sensor expired!");
        break;

    case SENSOR_NOT_AVAILABLE:
        librelinkup.glucose_data().str_TrendMessage = "no active sensor";
        logger.notice("no active sensor");
        break;
    
    case SENSOR_STARTING:
        remaining_time = librelinkup.get_remaining_warmup_time(
            librelinkup.sensor_data().sensor_non_activ_unixtime);
        if (remaining_time < 1)
        {
            // If the countdown has reached 0 min, stop showing the warmup message.
            librelinkup.glucose_data().str_TrendMessage = "";
            logger.notice("Sensor warmup countdown reached 0 min.");
        }
        else
        {
            sprintf(buffer, "sensor ready in %d min", remaining_time);
            librelinkup.glucose_data().str_TrendMessage = buffer;
            logger.notice("Sensor in starting phase!");
        }
        break;

    case SENSOR_READY:
        // Only for ready sensors, timestamp status decides delayed/lost overlays.
        if (librelinkup.status().timestamp_status == SENSOR_LOST)
        {
            librelinkup.glucose_data().str_TrendMessage = "sensor lost!";
            logger.notice("sensor lost!");
            return;
        }
        if (librelinkup.status().timestamp_status == SENSOR_TIMECODE_OUT_OF_RANGE)
        {
            librelinkup.glucose_data().str_TrendMessage = "sensor delayed";
            logger.notice("sensor delayed");
            return;
        }
        librelinkup.glucose_data().str_TrendMessage = "";
        break;
    }
}

/**
 * @brief Updates 5-minute chart refresh counter
 *
 * Decrements counter every minute and triggers full chart redraw
 * when it reaches zero. Also updates glucose statistics and JSON logging
 * if sensor is ready.
 *
 * @note Counter resets to 5 after triggering update
 * @note Statistics only calculated when sensor state is SENSOR_READY
 * @note Called once per minute by main update cycle
 *
 * @see draw_chart_glucose_data()
 * @see update_glucose_json_logging()
 * @see glucose_statistics()
 */
void update_five_minute_counter()
{

    // Counter that redraws the glucose chart every 5 minutes
    static uint8_t five_minute_chart_update_counter = 5;

    // Decrement counter
    five_minute_chart_update_counter--;

    // Debug log for verification
    logger.debug("five_minute_chart_update_counter: %d", five_minute_chart_update_counter);

    // When counter reaches 0, perform chart update
    if (five_minute_chart_update_counter <= 0)
    {
        five_minute_chart_update_counter = 5; // Reset to 5 minutes
        logger.debug("Triggering 5-minute chart update...");
        draw_chart_glucose_data(1); // Perform 5-minute update

        if (librelinkup.status().sensor_state == SENSOR_READY)
        {
            update_glucose_json_logging();
            //glucose_statistics(); // Print glucose statistics
        }
    }
}

/**
 * @brief Main glucose data update function
 *
 * Performs complete data update cycle:
 * 1. Check WiFi connection
 * 2. Fetch data from LibreLinkUp API
 * 3. Determine sensor type (14-day or 15-day)
 * 4. Validate sensor status and timestamps
 * 5. Update trend message
 * 6. Process valid data or handle errors
 * 7. Update all UI elements
 *
 * @note Called every 60 seconds by main timer
 * @note Handles both sensor reconnect and normal update scenarios
 * @note Updates backup value for delta calculation
 *
 * @see handle_internet_disconnection()
 * @see handle_llu_api_error()
 * @see handle_sensor_reconnect()
 * @see handle_invalid_timestamp()
 */
void update_glucose_data()
{

    // Check WiFi connection first
    if (WiFi.status() != WL_CONNECTED)
    {
        handle_internet_disconnection();
        return;
    }

    glucose_delta = 0;
    lcd_status_indication(1, 1); // Show activity indicator

    // Fetch graph data from API
    if (settings.config.mqtt_master_mode == true)
    {
        logger.debug("Fetch graph data (master mode)...");
        if (librelinkup.get_graph_data() == 0)
        {
            handle_llu_api_error();
            return;
        }
        // settings.config.mqtt_master_mode = false; // Currently unused
    }
    else
    {
        logger.debug("Fetch graph data (client mode)...");
    }

    lcd_status_indication(0, 1); // Hide activity indicator

    // Check sensor type and create appropriate progress bars
    int sensor_type = librelinkup.check_sensor_type();
    if (sensor_type == 1)
    {
        switch_sensor_valid_progress_bar(&dayBar15); // 15-day sensor
    }
    else if (sensor_type == -1)
    {
        switch_sensor_valid_progress_bar(&dayBar14); // 14-day sensor
    }

    // Read sensor status and timestamp
    librelinkup.status().sensor_state = librelinkup.check_sensor_lifetime(
        librelinkup.sensor_data().sensor_non_activ_unixtime,
        librelinkup.sensor_data().sensor_runtime);
    //logger.debug("Sensor State: %d", librelinkup.status().sensor_state);

    // Force warmup state during first hour after activation if LLU still reports
    // a non-active sensor snapshot. This prevents stale timestamp "lost" overlays.
    if (librelinkup.sensor_data().sensor_sn_non_active.length() > 0 &&
        librelinkup.sensor_data().sensor_non_activ_unixtime > 0)
    {
        const time_t now = time(nullptr);
        if ((librelinkup.sensor_data().sensor_non_activ_unixtime + librelinkup.UNIXTIME1HOUR) > now)
        {
            librelinkup.status().sensor_state = SENSOR_STARTING;
        }
    }

    // Validate timestamp
    librelinkup.status().timestamp_status = librelinkup.check_valid_timestamp_factory(
        librelinkup.glucose_data().str_measurement_factorytimestamp,
        librelinkup.glucose_data().str_measurement_timestamp, 1);

    // Convert timestamp to Unix time
    librelinkup.status().last_timestamp_unixtime = helper.convertStrToUnixTime(
        librelinkup.glucose_data().str_measurement_timestamp);

    // Set trend message based on sensor status
    update_trend_message();

    // Debug snapshot for color transition analysis (API color vs local thresholds).
    const uint16_t mgdl = librelinkup.glucose_data().glucoseMeasurement;
    const uint16_t low_target = librelinkup.glucose_data().glucosetargetLow;
    const uint16_t high_target = librelinkup.glucose_data().glucosetargetHigh;
    const uint16_t low_alarm = librelinkup.glucose_data().glucoseAlarmLow;
    const uint16_t high_alarm = librelinkup.glucose_data().glucoseAlarmHigh;
    const uint8_t api_color = librelinkup.glucose_data().measurement_color;
    const bool local_marker_red = (mgdl >= high_target || mgdl <= low_alarm);

    const char *api_color_name = "UNKNOWN";
    switch (api_color)
    {
    case 1: // GlucoseLabelColor::COLOR_WHITE
        api_color_name = "WHITE";
        break;
    case 2: // GlucoseLabelColor::COLOR_YELLOW
        api_color_name = "YELLOW";
        break;
    case 3: // GlucoseLabelColor::COLOR_ORANGE
        api_color_name = "ORANGE";
        break;
    case 4: // GlucoseLabelColor::COLOR_RED
        api_color_name = "RED";
        break;
    case 5: // GlucoseLabelColor::COLOR_BLUE
        api_color_name = "BLUE";
        break;
    default:
        break;
    }

    logger.notice(
        "COLORDBG mgdl=%u api_color=%u(%s) targetLow=%u targetHigh=%u alarmLow=%u alarmHigh=%u marker=%s sensor_state=%u ts_status=%u",
        mgdl,
        api_color,
        api_color_name,
        low_target,
        high_target,
        low_alarm,
        high_alarm,
        local_marker_red ? "RED" : "GREEN",
        librelinkup.status().sensor_state,
        librelinkup.status().timestamp_status
    );

    // Check if LLU timestamp is valid and process data
    if (librelinkup.status().timestamp_status == SENSOR_TIMECODE_VALID)
    {

        if (settings.config.mqtt_master_mode)
        {
            synchronize_time_offset_epoch();
        }
        if (librelinkup.reconnect_flag() == 1)
        {
            handle_sensor_reconnect();
        }
        else
        {
            glucose_delta = librelinkup.glucose_data().glucoseMeasurement -
                            glucoseMeasurement_backup;
        }

        logger.notice("glucoseMeasurement: %d %s ∆: %d",
                      librelinkup.glucose_data().glucoseMeasurement,
                      librelinkup.glucose_data().str_trendArrow.c_str(),
                      glucose_delta);

        draw_chart_sensor_valid();
        draw_labels(true, librelinkup.glucose_data().measurement_color,
                    librelinkup.glucose_data().glucoseMeasurement,
                    librelinkup.glucose_data().str_trendArrow,
                    librelinkup.glucose_data().str_TrendMessage,
                    glucose_delta);
        draw_chart_glucose_data(3);

        glucoseMeasurement_backup = librelinkup.glucose_data().glucoseMeasurement;
    }
    else
    {
        // Invalid timestamp - handle error
        handle_invalid_timestamp();
        /*
        draw_labels(false, librelinkup.glucose_data().measurement_color,
                    librelinkup.glucose_data().glucoseMeasurement,
                    librelinkup.glucose_data().str_trendArrow,
                    librelinkup.glucose_data().str_TrendMessage, 0);
        draw_chart_glucose_data(1);
        */
    }
}

/**
 * @brief Logs glucose value to JSON file on LittleFS
 *
 * Stores current glucose measurement with timestamp for:
 * - HbA1c calculation
 * - Historical data analysis
 * - Weekly statistics
 *
 * @note Uses current epoch time from LibreLinkUp
 * @note Data stored in daily JSON files
 *
 * @see hba1c.addGlucoseValue()
 */
void update_glucose_json_logging()
{
    uint32_t unixtime_now = librelinkup.get_epoch_time();
    hba1c.addGlucoseValue(unixtime_now, librelinkup.glucose_data().glucoseMeasurement);
    // logger.debug("addGlucoseValue to LittleFS: %d / %d", unixtime_now, librelinkup.glucose_data().glucoseMeasurement);
}

/**
 * @brief Calculates and logs comprehensive glucose statistics
 *
 * Computes and displays:
 * - Current glucose value
 * - Mean glucose (from history and JSON)
 * - Weekly mean glucose
 * - HbA1c estimate (%)
 * - TIR - Time in Range 70-180 mg/dL (%)
 * - Standard deviation (σ)
 * - Coefficient of variation (CV)
 *
 * @note Statistics calculated from current sensor history
 * @note Weekly data loaded from JSON files
 * @note Results logged to console/telnet
 *
 * @see hba1c.calculateGlucoseMeanFromHistory()
 * @see hba1c.calculate_hba1c()
 * @see hba1c.calculate_time_in_range()
 */
void glucose_statistics()
{
    uint8_t data_count = librelinkup.check_graphdata();

    float mean_glucose_value_from_history = hba1c.calculateGlucoseMeanFromHistory(
        librelinkup.sensor_history_data().graph_data, data_count);
    float mean_glucose_value_from_json = hba1c.calculateGlucoseMeanFromJson(
        today_json_filename);
    float mean_glucose_weekly_value_from_json = hba1c.calculateGlucoseMeanForLast7Days();
    float std_dev = hba1c.calculate_standard_deviation(
        librelinkup.sensor_history_data().graph_data, data_count,
        mean_glucose_value_from_history);

    logger.debug("========== Glucose Statistics =============");
    logger.debug("Current glucose value        : %d mg/dl",
                 librelinkup.glucose_data().glucoseMeasurement);
    logger.debug("Mean of history glucose value: %.0f mg/dl",
                 mean_glucose_value_from_history);
    logger.debug("Mean of weekly glucose value : %.0f mg/dl",
                 mean_glucose_weekly_value_from_json);
    logger.debug("HbA1c-Value of history data  : %.2f %%",
                 hba1c.calculate_hba1c(mean_glucose_value_from_history));
    logger.debug("TIR-Value of history data    : %.2f %%",
                 hba1c.calculate_time_in_range(
                     librelinkup.sensor_history_data().graph_data,
                     data_count, 70, 180));
    logger.debug("Std-Dev of history data      : %.2f σ", std_dev);
    logger.debug("Glucose variability (CV)     : %.2f %%",
                 hba1c.calculate_coefficient_of_variation(std_dev,
                                                          mean_glucose_value_from_history));
    logger.debug("===========================================");
}

/**
 * @brief Updates debug screen with current system and sensor information
 *
 * Displays real-time data for debugging purposes:
 * - Data refresh countdown
 * - ESP32 system time
 * - Local IP address
 * - Sensor ID and status
 * - Current glucose value and trend
 *
 * @note Only updates if debug screen is active
 * @note Uses LibreLinkUp data for sensor information
 * @note Formats output for readability
 */
void update_debug_screen()
{
    if (lv_scr_act() == ui_Debug_screen)
        {
            // Data refresh countdown (FSM cadence)
            const uint32_t period_ms = g_fsm.cfg.fetch_period_ms ? g_fsm.cfg.fetch_period_ms : 60000U;
            const uint32_t elapsed_ms = (uint32_t)(millis() - (uint32_t)g_fsm.last_fetch_ms);
            uint32_t remaining_s = (elapsed_ms < period_ms) ? ((period_ms - elapsed_ms) / 1000U) : 0;

            char buf[96];

            snprintf(buf, sizeof(buf), "Data Refresh in: %lu sec.", (unsigned long)remaining_s);
            lv_label_set_text(ui_Label_DebugDataRefresh, buf);

            snprintf(buf, sizeof(buf), "ESP32 Time: %s", helper.get_esp_time_date().c_str());
            lv_label_set_text(ui_Label_DebugTime, buf);

            IPAddress ip = WiFi.localIP();
            snprintf(buf, sizeof(buf), "IP: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            lv_label_set_text(ui_Label_DebugIP, buf);

            snprintf(buf, sizeof(buf), "Sensor: %s", librelinkup.sensor_data().sensor_id.c_str());
            lv_label_set_text(ui_Label_DebugSensor, buf);

            snprintf(buf, sizeof(buf), "Valid: %dDays %dHours %dMinutes",
                        librelinkup.sensor_lifetime().sensor_valid_days,
                        librelinkup.sensor_lifetime().sensor_valid_hours,
                        librelinkup.sensor_lifetime().sensor_valid_minutes);
            lv_label_set_text(ui_Label_DebugSensorTimestamp, buf);

            const int st = librelinkup.sensor_data().sensor_state;
            const char *st_txt =
                (st == 0) ? "unknown" : (st == 1) ? "not started yet"
                                    : (st == 2)   ? "starting phase"
                                    : (st == 3)   ? "ready"
                                    : (st == 4)   ? "expired"
                                    : (st == 5)   ? "shut down"
                                    : (st == 6)   ? "has failure"
                                                    : "other";

            snprintf(buf, sizeof(buf), "Sensor State: %d => %s", st, st_txt);
            lv_label_set_text(ui_Label_DebugSensorState, buf);

            char delta_buf[16];
            if (glucose_delta == 0)
                snprintf(delta_buf, sizeof(delta_buf), "±%d", glucose_delta);
            else if (glucose_delta > 0)
                snprintf(delta_buf, sizeof(delta_buf), "+%d", glucose_delta);
            else
                snprintf(delta_buf, sizeof(delta_buf), "%d", glucose_delta);

            snprintf(buf, sizeof(buf), "Sensor Value: %d%s %s mg/dL",
                        librelinkup.glucose_data().glucoseMeasurement,
                        librelinkup.glucose_data().str_trendArrow.c_str(),
                        delta_buf);
            lv_label_set_text(ui_Label_DebugSensorValue, buf);
        }
}
///////////////////// FREERTOS BACKGROUND TASKS ////////////////////

/**
 * @brief Main loop task running on Core 1
 *
 * Handles background operations:
 * - ElegantOTA update loop
 * - MQTT client keep-alive
 * - UUID console/telnet service
 * - Shell command processing
 *
 * @param[in] pvParameters Task parameters (unused)
 *
 * @note Runs continuously on FreeRTOS Core 1
 * @note Pauses during OTA updates
 * @note 10ms delay prevents task starvation
 *
 * @warning Do not add blocking operations here
 */
void LoopTask(void *pvParameters)
{
    Serial.println("Loop Task started...");

    while (1)
    {
        // ElegantOTA update handling
        ElegantOTA.loop();

        //  Telnet console handling
        uuid::loop();
        telnet.loop();
        Shell::loop_all();
        yield();

        vTaskDelay(pdMS_TO_TICKS(10)); // Don't block task scheduler
    }
}

///////////////////// HARDWARE SETUP FUNCTIONS ////////////////////

/**
 * @brief Initializes serial console
 *
 * Configures USB CDC serial port at 115200 baud with timeout
 * workaround for cases where no host is connected.
 *
 * @note Uses F() macro for flash string storage
 */
void setup_serial()
{
    Serial.begin(115200);
    Serial.setTxTimeoutMs(1); // Workaround for blocking output if no host connected
    Serial.println();
    DBGprint;
    Serial.println(F("Libre Link Up Api client with lvgl"));
}

/**
 * @brief Initializes UART for IPC communication
 *
 * Sets up UART2 for Inter-Processor Communication between
 * ESP32-H2 and ESP32-S3.
 *
 * @note Uses 115200 baud, 8N1 configuration
 * @note Currently commented out in main setup()
 */
void setup_UART_IPC()
{
    SerialPort.begin(115200, SERIAL_8N1, ESP32H2_RX, ESP32H2_TX);
    Serial.println();
    DBGprint;
    Serial.println(F("Init SerialPort for IPC"));
}

/**
 * @brief Initializes LittleFS filesystem
 *
 * Mounts SPIFFS/LittleFS partition for:
 * - Configuration storage
 * - Glucose data logging
 * - Statistical data
 *
 * @note Returns silently on mount failure
 * @warning No retry mechanism implemented
 */
void setup_littlefs()
{
    if (!LittleFS.begin())
    {
        DBGprint;
        Serial.println("An Error has occurred while mounting SPIFFS");
        return;
    }
}

/**
 * @brief Initializes TPanel display and touch controller
 *
 * Complete hardware initialization sequence:
 * 1. Create LVGL tick task (Core 1)
 * 2. Configure backlight PWM (45% initial)
 * 3. Initialize I2C for touch controller
 * 4. Reset touch controller (hardware reset)
 * 5. Configure touch interrupt (falling edge)
 * 6. Initialize touch driver
 * 7. Initialize LCD (RGB parallel interface)
 * 8. Initialize LVGL library
 * 9. Create UI screens
 * 10. Display welcome message
 *
 * @note LVGL tick task runs on Core 1 with priority 1
 * @note Touch interrupt triggers on falling edge
 * @note Initial backlight set to 45% brightness
 *
 * @see lv_tick_task()
 * @see lvgl_initialization()
 * @see ui_init()
 */
void setup_tpanels3()
{

    // Create LVGL tick task
    xTaskCreatePinnedToCore(
        lv_tick_task,    // Task function
        "lv_tick_task",  // Task name
        2048,            // Stack size (bytes)
        NULL,            // Parameter (optional)
        1,               // Priority (1 is low)
        &LvglTaskHandle, // Task handle (optional)
        1                // Core (0 or 1)
    );

    // --- initialize display & touch hardware ---
    tpanels3.initTPanelS3();
    tpanels3.setRotation(0); // optional: 0=portrait, 1=landscape, etc.

    // Set initial backlight brightness to 45%
    if (settings.config.brightness == 0)
    {
        settings.config.brightness = 50; // Default to 45% if not set
    }
    tpanels3.set_backlight_brightness(settings.config.brightness); // Set to 45%

    // Initialize UI screens
    ui_init();
    lv_label_set_text(ui_Label_WelcomeInfo, "LibreLinkUp\nClient");
    lv_timer_handler(); // Let the GUI do its work
}

/**
 * @brief Loads system configuration from file
 *
 * Reads configuration JSON and applies timezone setting
 * to LibreLinkUp client.
 *
 * @note Configuration file path defined in settings.config_filename
 * @note Timezone setting affects time display and calculations
 *
 * @see settings.loadConfiguration()
 */
void setup_load_system_config()
{
    settings.loadConfiguration(settings.config_filename, settings.config);
    librelinkup.timezone_offset() = settings.config.timezone;
}

/**
 * @brief Establishes WiFi connection
 *
 * Attempts to connect to configured WiFi network:
 * - Success: Synchronizes time with NTP servers
 * - Failure: Starts Access Point for configuration
 *
 * WiFi settings:
 * - Mode: Station (STA)
 * - TX Power: +10 dBm
 * - Auto-reconnect: Enabled
 * - Persistent: Enabled
 *
 * NTP servers:
 * - pool.ntp.org
 * - ntp.nict.jp
 * - time.google.com
 *
 * @note AP credentials: settings.apSSID / settings.apPassword
 * @note AP IP: 192.168.4.1
 * @note Connection timeout: connectTimeoutMs (5000ms)
 *
 * @see WiFiMulti.run()
 */
// app/main/main.cpp

// app/main/main.cpp

static bool g_ap_mode = false; // optional: for UI/logik

void setup_wifi()
{
    lv_label_set_text(ui_Label_WelcomeWifiInfo, "connecting to Wifi...");
    lv_timer_handler();

    // --- STA ONLY attempt -------------------------------------------------
    g_ap_mode = false;

    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.setSleep(false);

    wifiMulti.addAP(settings.config.wifi_bssid.c_str(),
                    settings.config.wifi_password.c_str());

    DBGprint;
    Serial.println(F("connecting to Wifi..."));

    // NOTE: wifiMulti.run() returns uint8_t in your build; compare directly.
    if (wifiMulti.run(connectTimeoutMs) == WL_CONNECTED)
    {
        // Ensure AP is OFF (single-mode)
        WiFi.softAPdisconnect(true);

        delay(100);
        DBGprint;
        Serial.print(F("SSID:"));
        Serial.print(WiFi.SSID());
        Serial.print(F(" IP:"));
        Serial.print(WiFi.localIP());
        Serial.print(F(" RSSI: "));
        Serial.println(WiFi.RSSI());

        lv_label_set_text(ui_Label_WelcomeWifiInfo, "connected!");
        lv_timer_handler();
        delay(300);

        lv_label_set_text(ui_Label_WelcomeWifiInfo, WiFi.localIP().toString().c_str());
        lv_timer_handler();
        delay(300);

        DBGprint;
        Serial.println("Adjusting system time from ntp server..");
        configTime(0, 0, "pool.ntp.org", "ntp.nict.jp", "time.google.com");
        setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
        tzset();
        helper.printLocalTime(0);

        WiFi.setAutoReconnect(true);
        WiFi.persistent(true);
        return;
    }

    // --- AP ONLY fallback -------------------------------------------------
    g_ap_mode = true;

    // Stop STA cleanly, then AP-only
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.mode(WIFI_AP);

    // Optional: force AP IP (default is 192.168.4.1)
    IPAddress ip(192, 168, 4, 1), gw(192, 168, 4, 1), sn(255, 255, 255, 0);
    WiFi.softAPConfig(ip, gw, sn);

    WiFi.softAP(settings.apSSID, settings.apPassword);
    const IPAddress ap_ip = WiFi.softAPIP();

    DBGprint;
    Serial.println("Access Point started, AP:");
    DBGprint;
    Serial.println(settings.apSSID);
    DBGprint;
    Serial.print("AP IP: ");
    Serial.println(ap_ip);

    String msg = "Wifi AP " + ap_ip.toString();
    lv_label_set_text(ui_Label_WelcomeWifiInfo, msg.c_str());
    lv_timer_handler();
    delay(300);
}

/**
 * @brief Initializes WireGuard VPN client
 *
 * @param[in] enable True to start VPN, false to stop
 *
 * Configures and starts/stops WireGuard tunnel:
 * - Loads configuration from settings
 * - Attempts connection
 * - Verifies with ping test to 1.1.1.1
 * - Updates configuration mode flag
 *
 * @note Connection verified with ping to Cloudflare DNS (1.1.1.1)
 * @note 500ms delay allows connection establishment
 * @note Configuration keys loaded from settings.config
 *
 * @see wg.begin()
 * @see Ping.ping()
 */
// Simple reentrancy guard for WireGuard setup/teardown.
// (WG calls can block; we must avoid nested calls from CLI/FSM timers.)
static volatile bool g_wg_busy = false;

bool wg_is_busy()
{
    return g_wg_busy;
}

// Resolve WireGuard endpoint to IPv4 before wg.begin().
// This avoids transient DNS failures at boot and allows retries on each WG re-init.
static bool resolve_wg_endpoint_ipv4(const String &endpoint, IPAddress &resolved_ip)
{
    IPAddress parsed_ip;
    if (parsed_ip.fromString(endpoint))
    {
        resolved_ip = parsed_ip;
        logger.debug("[setup_wg] endpoint is static IPv4: %s", resolved_ip.toString().c_str());
        DBGprint;
        Serial.printf("[setup_wg] endpoint is static IPv4: %s\n", resolved_ip.toString().c_str());
        return true;
    }

    constexpr uint8_t max_attempts = 5;
    constexpr uint16_t retry_delay_ms = 400;

    for (uint8_t attempt = 1; attempt <= max_attempts; ++attempt)
    {
        if (WiFi.hostByName(endpoint.c_str(), resolved_ip) == 1)
        {
            logger.debug("[setup_wg] endpoint DNS resolved: %s -> %s",
                          endpoint.c_str(), resolved_ip.toString().c_str());
            DBGprint;
            Serial.printf("[setup_wg] endpoint DNS resolved: %s -> %s\n",
                          endpoint.c_str(), resolved_ip.toString().c_str());
            return true;
        }

        logger.debug("[setup_wg] endpoint DNS failed (%u/%u): %s",
                      (unsigned)attempt, (unsigned)max_attempts, endpoint.c_str());
        DBGprint;
        Serial.printf("[setup_wg] endpoint DNS failed (%u/%u): %s\n",
                      (unsigned)attempt, (unsigned)max_attempts, endpoint.c_str());
        if (attempt < max_attempts)
            delay(retry_delay_ms);
    }

    return false;
}

static bool wg_time_looks_valid()
{
    // Jan 1, 2024 00:00:00 UTC
    constexpr time_t min_valid_epoch = 1704067200;
    return time(nullptr) >= min_valid_epoch;
}

static void ensure_time_for_wg()
{
    if (wg_time_looks_valid())
        return;

    logger.notice("[setup_wg] system time not valid yet, retrying NTP sync...");
    configTime(0, 0, "pool.ntp.org", "ntp.nict.jp", "time.google.com");

    constexpr uint8_t max_attempts = 20; // ~10s
    for (uint8_t i = 0; i < max_attempts; ++i)
    {
        if (wg_time_looks_valid())
            break;
        delay(500);
    }

    logger.notice("[setup_wg] time check epoch=%ld valid=%d",
                  (long)time(nullptr), (int)wg_time_looks_valid());
}

/**
 * @brief Setup or teardown WireGuard.
 * @param enable        true to enable/ensure WG; false to disable.
 * @param force_reinit  if true, always tear down and re-init (even if initialized).
 * @return true if WG is up (or disabled successfully), false on failure or if skipped due to busy.
 */
bool setup_wg(bool enable, bool force_reinit)
{
    // Reentrancy guard
    if (g_wg_busy)
    {
        logger.notice("[setup_wg] skipped: wg busy");
        return false;
    }
    g_wg_busy = true;

    bool result = false;

    if (!enable)
    {
        logger.notice("[setup_wg] disabling WG interface...");
        wg.end();

        // IMPORTANT: settings.config.wg_mode represents the *desired* state.
        // If the user disables WG, then and only then we set it to 0.
        settings.config.wg_mode = 0;

        result = true;
        g_wg_busy = false;
        return result;
    }

    // desired state is ON
    settings.config.wg_mode = 1;

    // parse configured local IP
    const IPAddress local_ip = helper.parseIPAddress(settings.config.wgIpAddress);
    logger.notice("[setup_wg] requested: enable=1, force_reinit=%d, local_ip=%s",
                  (int)force_reinit, local_ip.toString().c_str());

    // If already initialized and not forcing, do a quick self-IP ping (interface presence)
    if (wg.is_initialized() && !force_reinit)
    {
        if (app_check_internet_status(IPAddress(192, 168, 0, 202), 1883) == true)
        {
            logger.notice("[setup_wg] WG already initialized (self IP ping ok).");
            g_wg_busy = false;
            return true;
        }
        logger.notice("[setup_wg] self IP ping failed, will reinitialize WG");
    }

    // Tear down first (safe to call even if not initialized)
    if (wg.is_initialized() || force_reinit)
    {
        logger.notice("[setup_wg] Shutting down WG interface...");
        lv_label_set_text(ui_Label_WelcomeWifiInfo, "Shutting down WG interface...");
        lv_timer_handler();
        wg.end();
        delay(50); // short settle
    }

    // Start/init WG
    logger.notice("[setup_wg] Initializing WG interface...");
    lv_label_set_text(ui_Label_WelcomeWifiInfo, "Initializing WG interface...");
    lv_timer_handler();

    ensure_time_for_wg();

    bool begin_ok = false;
    constexpr uint8_t wg_begin_attempts = 3;
    for (uint8_t attempt = 1; attempt <= wg_begin_attempts; ++attempt)
    {
        IPAddress endpoint_ip;
        if (!resolve_wg_endpoint_ipv4(settings.config.wgEndpoint, endpoint_ip))
        {
            logger.notice("[setup_wg] WG endpoint DNS failed (%u/%u): %s",
                          (unsigned)attempt, (unsigned)wg_begin_attempts, settings.config.wgEndpoint.c_str());
            if (attempt < wg_begin_attempts)
                delay(300);
            continue;
        }

        const String endpoint_ip_str = endpoint_ip.toString();
        logger.notice("[setup_wg] wg.begin attempt %u/%u with endpoint=%s",
                      (unsigned)attempt, (unsigned)wg_begin_attempts, endpoint_ip_str.c_str());

        begin_ok = wg.begin(
            local_ip,
            settings.config.wgPrivateKey.c_str(),
            endpoint_ip_str.c_str(),
            settings.config.wgPublicKey.c_str(),
            (uint16_t)settings.config.wgEndpointPort,
            settings.config.wgPresharedKey.c_str()
        );

        if (begin_ok)
            break;

        wg.end();
        if (attempt < wg_begin_attempts)
            delay(300);
    }

    if (!begin_ok)
    {
        logger.notice("[setup_wg] wg.begin() failed!");
        lv_label_set_text(ui_Label_WelcomeWifiInfo, "WG init FAILED");
        lv_timer_handler();
        wg.end(); // ensure clean

        // Do NOT flip wg_mode to 0 here. Let FSM retry.
        result = false;
        g_wg_busy = false;
        return result;
    }

    // Bounded ping checks. WG handshake can take a few seconds on some networks.
    const uint32_t deadline = millis() + 7000;
    bool ok = false;
    while (millis() < deadline)
    {
        if (app_check_internet_status(IPAddress(192, 168, 0, 202), 1883) == true)
        {
            ok = true;
            break;
        }
        delay(200);
    }

    if (ok)
    {
        logger.notice("[setup_wg] WG connected! IP:%s", local_ip.toString().c_str());
        lv_label_set_text(ui_Label_WelcomeWifiInfo, "WG connected!");
        lv_timer_handler();
        result = true;
    }
    else
    {
        logger.notice("[setup_wg] WG self IP ping: NOK (after init)");
        lv_label_set_text(ui_Label_WelcomeWifiInfo, "WG ping FAILED");
        lv_timer_handler();

        // leave interface stopped to avoid partial/ghost routes
        wg.end();

        // Do NOT flip wg_mode to 0 here. Let FSM retry.
        result = false;
    }

    g_wg_busy = false;
    return result;
}

/**
 * @brief Initializes mDNS responder
 *
 * Creates hostname from base name + unique chip ID:
 * - Format: librelinkup_XXXXXX
 * - Allows device to be accessed via hostname.local
 *
 * @note Hostname generated from flash memory chip ID
 * @note Accessible as hostname.local on local network
 *
 * @see helper.get_flashmemory_id()
 */
void setup_mdns()
{
    hostname = hostname_base + helper.get_flashmemory_id();

    if (!MDNS.begin(hostname.c_str()))
    {
        DBGprint;
        Serial.println("Error setting up MDNS responder!");
        logger.notice("Error setting up MDNS responder!");
    }

    DBGprint;
    Serial.println("mDNS responder started");
    logger.notice("mDNS responder started ... Hostname: %s", hostname.c_str());
}

/**
 * @brief Initializes LibreLinkUp API client
 *
 * If no credentials stored, shows login screen.
 * Otherwise begins API client with region code 2.
 *
 * @note Region code 2 = Europe
 * @note Login screen displayed if credentials empty
 *
 * @see librelinkup.begin()
 */
void setup_librelinkup()
{
    if (settings.config.login_email == "" || settings.config.login_password == "")
    {
        lv_disp_load_scr(ui_Login_screen);
    }
    else
    {
        librelinkup.set_credentials(settings.config.login_email, settings.config.login_password);
    }
    librelinkup.begin(2);
}

/**
 * @brief Initializes MQTT client connection
 *
 * Configures MQTT broker connection:
 * - Sets server and port
 * - Registers callback function
 * - Sets buffer size to 512 bytes
 * - Creates unique client name from chip ID
 * - Connects and subscribes to command topic
 *
 * @note Client name format: /CHIPID
 * @note Subscribes to: /librelinkup/CHIPID/cmd
 * @note Buffer size: 512 bytes
 *
 * @see mqtt_callback()
 */
bool setup_mqtt()
{
    mqtt_client.setServer(mqtt.mqtt_server, mqtt.mqtt_port);
    mqtt_client.setCallback(mqtt_callback);
    mqtt_client.setBufferSize(16384);                    // 16KB buffer size
    mqtt_client.setSocketTimeout(3);                     // seconds (prevents long blocking connect)
    mqtt_client.setKeepAlive(30);                        // seconds

    mqtt.mqtt_client_name = helper.get_flashmemory_id(); // e.g. "4B431EEB"
    const String clientId = mqtt.mqtt_client_name;

    if (mqtt_client.connected())
        return true;

    logger.notice("MQTT: connecting... clientId=%s target=%s:%u",
                  clientId.c_str(), mqtt.mqtt_server, (unsigned)mqtt.mqtt_port);

    const bool ok = mqtt_client.connect(clientId.c_str(), mqtt.mqtt_user, mqtt.mqtt_password);
    logger.debug("MQTT connect ok=%d state=%d", (int)ok, mqtt_client.state());

    if (!ok || !mqtt_client.connected())
    {
        logger.debug("MQTT: connect failed, state=%d", mqtt_client.state());
        return false;
    }

    logger.notice("MQTT: connected");
    g_allow_retained_once = true;

    const String subCmd = mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic;
    const String subRaw = mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data;

    mqtt_client.unsubscribe(subCmd.c_str());
    mqtt_client.unsubscribe(subRaw.c_str());

    const bool s1 = mqtt_client.subscribe(subCmd.c_str());
    bool s2 = true;

    if (settings.config.mqtt_master_mode == false)
    {
        s2 = mqtt_client.subscribe(subRaw.c_str());
        logger.notice("MQTT subscribe raw: %s ok=%d", subRaw.c_str(), (int)s2);
    }

    logger.notice("MQTT subscribe cmd: %s ok=%d", subCmd.c_str(), (int)s1);

    return (s1 && s2);
}

/**
 * @brief Initializes OTA update server
 *
 * @param[in] mode True to start server, false to stop
 *
 * When enabled:
 * - Creates WiFi scan background task (Core 1)
 * - Registers web routes
 * - Starts ElegantOTA service
 * - Configures OTA callbacks
 * - Starts HTTP server on port 80
 *
 * @note WiFi scan task: 4KB stack, priority 1, Core 1
 * @note HTTP server accessible on port 80
 * @note Web routes defined in register_webpage_routes()
 *
 * @see scanWiFiTask()
 * @see onOTAStart()
 * @see onOTAProgress()
 * @see onOTAEnd()
 */
void setup_OTA(bool mode)
{

    if (mode == 1)
    {

        if (g_ap_mode == false)
        {
            // Create WiFi scan background task
            xTaskCreatePinnedToCore(
                scanWiFiTask,     // Function
                "WiFi Scan Task", // Task name
                4096,             // Stack size
                NULL,             // Parameter
                1,                // Task priority
                &wifiScanHandle,  // Task handle
                1                 // Core 1
            );
        }

        // Register web routes
        register_webpage_routes(server);

        // Start ElegantOTA
        ElegantOTA.begin(&server);
        ElegantOTA.onStart(onOTAStart);
        ElegantOTA.onProgress(onOTAProgress);
        ElegantOTA.onEnd(onOTAEnd);

        server.begin();
        DBGprint;
        Serial.println("HTTP server started");
    }
    else
    {
        // server.end();
        // DBGprint;
        // Serial.println("HTTP server stopped");
    }
}

/**
 * @brief Initializes UUID console system
 *
 * Registers console commands and starts telnet service
 * for remote debugging and control.
 *
 * @note Telnet default port: 23
 * @note Commands defined in registerCommands()
 *
 * @see registerCommands()
 * @see telnet.start()
 */
void setup_uuid_console()
{
    registerCommands(commands);
    telnet.start();
}

/**
 * @brief Creates FreeRTOS background loop task
 *
 * Starts LoopTask on Core 1 with:
 * - 16KB (16384 words) stack
 * - Priority 1
 * - Handles OTA, MQTT, Telnet
 *
 * @note Task runs continuously on Core 1
 * @note Stack size: 64KB (16384 words × 4 bytes)
 *
 * @see LoopTask()
 */
void setup_task()
{
    xTaskCreatePinnedToCore(
        LoopTask,        // Function name
        "LoopTask",      // Task name
        16384,           // Stack size: 64KB = 16384 words
        NULL,            // Parameter
        1,               // Priority
        &LoopTaskhandle, // Task handle
        1                // Core 1
    );
}

//-----------------------------------------------------------------------------

/**
 * @brief Arduino setup entry point.
 *
 * Initializes all subsystems (UART, filesystem, configuration, display/LVGL,
 * networking, services, OTA, tasks) and registers all LVGL callbacks.
 *
 * @note This function mirrors your original startup sequence without changing behavior.
 */
void setup()
{
    // --- Serial / UART -------------------------------------------------------
    setup_serial(); ///< Initialize USB CDC serial (logging etc.)

    // Optional secondary UART (ESP32H2 <-> ESP32S3 IPC)
    // setup_UART_IPC();

    // --- Storage & Configuration --------------------------------------------
    setup_littlefs();           ///< Mount LittleFS (fail is non-fatal by design)
    setup_load_system_config(); ///< Load configuration from persistent storage

    // --- Display / Touch / LVGL ---------------------------------------------
    setup_tpanels3(); ///< Init panel, touch, backlight, LVGL core/UI

    // --- Network stack -------------------------------------------------------
    setup_wifi(); ///< Connect to Wi-Fi (or start AP as fallback)

    // --- Console / Telnet ----------------------------------------------------
    setup_uuid_console(); ///< Start UUID shell & Telnet service

    // --- VPN / mDNS / MQTT / App backends -----------------------------------
    setup_wg(settings.config.wg_mode); ///< Enable/disable WireGuard based on config
    setup_mdns();                      ///< Start mDNS responder
    setup_mqtt();                      ///< Configure and (re)connect MQTT client
    setup_librelinkup();               ///< Initialize LibreLinkUp client

    // --- OTA / HTTP server ---------------------------------------------------
    // setup_OTA(settings.config.ota_update); ///< Start/stop OTA + HTTP API (routes registered elsewhere)
    setup_OTA(settings.config.ota_update == 1 ? 1 : 0);

    // --- Background tasks ----------------------------------------------------
    setup_task(); ///< Launch LoopTask() on a dedicated core
    // ------------------------------------------------------------------------

    // ------------------------ LVGL callback registration ---------------------
    // Main screen: short press toggles brightness (handled by brightness_on_off_cb)
    lv_obj_add_event_cb(ui_Main_screen, brightness_on_off_cb, LV_EVENT_PRESSED, NULL);

    // Chart: live touch tracking for marker/labels while pressing
    lv_obj_add_event_cb(ui_Chart_Glucose_5Min, touch_event_cb, LV_EVENT_PRESSING, NULL);

    // Global gesture navigation across screens
    lv_obj_add_event_cb(ui_Main_screen, touch_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(ui_Debug_screen, touch_gesture_cb, LV_EVENT_GESTURE, NULL);

    // Toolbar buttons (WireGuard / MQTT / OTA toggles)
    lv_obj_add_event_cb(ui_btn_wireguard, btn_wireguard_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_btn_mqtt, btn_mqtt_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_btn_ota_update, btn_ota_cb, LV_EVENT_ALL, NULL);

    // On-screen keyboard show/hide for text areas
    lv_obj_add_event_cb(ui_ta_email, ta_event_cb, LV_EVENT_ALL, ui_kb);
    lv_obj_add_event_cb(btn_login, btn_login_event_cb, LV_EVENT_CLICKED, NULL);

    // Focus handlers to bind the keyboard to the active text area
    lv_obj_add_event_cb(
        ui_ta_email,
        [](lv_event_t *event)
        {
            lv_keyboard_set_textarea(ui_kb, ui_ta_email);
            lv_obj_clear_flag(ui_kb, LV_OBJ_FLAG_HIDDEN);
        },
        LV_EVENT_FOCUSED,
        NULL
    );

    lv_obj_add_event_cb(
        ui_ta_password,
        [](lv_event_t *event)
        {
            lv_keyboard_set_textarea(ui_kb, ui_ta_password);
            lv_obj_clear_flag(ui_kb, LV_OBJ_FLAG_HIDDEN);
        },
        LV_EVENT_FOCUSED,
        NULL
    );
// ------------------------------------------------------------------------

    // ------------------------ Application FSM -------------------------------
    app_fsm_init(g_fsm);

    // ------------------------ Initial data & UI push -------------------------
    if (settings.config.mqtt_master_mode == true)
    {
        flag_mqtt_master_rx = false;
        update_glucose_data();          ///< First data fetch from LibreLinkUp backend
        update_five_minute_counter();   ///< Prime 5-minute chart refresh counter
        update_mqtt_publish();          ///< Publish first MQTT snapshot if enabled
        update_glucose_json_logging();  ///< Persist first reading to JSON log
        g_fsm.last_fetch_ms = millis(); ///< Prevent immediate refetch after boot
    }
    
    // Switch to the main screen as the default UI
    lv_disp_load_scr(ui_Main_screen);
    // ------------------------------------------------------------------------
}

/**
 * @brief Arduino main loop.
 *
 * Runs lightweight foreground work while heavy/continuous processing is done
 * inside LoopTask() on another core. Keeps LVGL responsive, handles UART IPC,
 * and advances software timers for UI/logic cadence.
 */
void loop()
{
    // Only run main loop if no OTA update in progress
    if (ota_in_progress == false)
    {
        // NOTE: All other continuous loops run inside LoopTask().
        // Keep this loop short to maintain UI responsiveness.

        // --- UART IPC (ESP32H2 <-> ESP32S3) -------------------------------------
        if (SerialPort.available() > 0)
        {
            UART_IPC_DATA1 = SerialPort.read();
            DBGprint; Serial.print(UART_IPC_DATA1);
            logger.notice("UART_IPC: %c", UART_IPC_DATA1);
        }

        // --- LVGL: let the GUI process pending work ------------------------------
        lv_timer_handler();
        delay(1);

        // --- Application state machine (connectivity/fetch/publish) -----------------
        app_fsm_poll(g_fsm);

        // 1 s tick: update debug view labels if visible (and not during OTA)
        if (flag_debug_screen == true)
        {
            flag_debug_screen = false; // reset flag, will be set again by timer
            //logger.debug("1s timer tick: updating debug screen labels...");
            update_debug_screen();
        }
    }
}
