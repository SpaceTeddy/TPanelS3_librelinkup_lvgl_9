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
#include <esp_task_wdt.h>
#include "main.h"
#include "app_fsm.h"
#include "lvgl.h"
#include "Arduino_GFX_Library.h"
#include "pin_config.h"

///////////////////// HELPER UTILITIES ////////////////////

#include "helper.h"
HELPER helper; ///< Helper class instance for time and utility functions

///////////////////// UART IPC COMMUNICATION ////////////////////

#include "h2_ota.h"
#include "zigbee_h2.h"

///////////////////// CONFIGURATION ////////////////////

#include "settings.h"

SETTINGS settings; ///< Settings manager instance

bool flag_mqtt_master_rx = true; // for first run
bool g_sim_sensor_pending = false;
bool flag_debug_screen = true;   // Debug flag to trigger debug screen on first loop iteration

AppFsm g_fsm; ///< Application state machine (polled from loop())

/// @name System Status Counters
/// @{
uint8_t esp_status_counter_wifi_restart = 0; ///< WiFi reconnection counter
uint8_t esp_status_counter_wg_reinit    = 0; ///< WireGuard tunnel reinit counter
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

TaskHandle_t LoopTaskHandle = NULL; ///< Main loop task handle
TaskHandle_t LvglTaskHandle = NULL; ///< LVGL tick task handle

/// @name Main-loop hang detection
/// @brief Arduino loop() (FSM/glucose fetch) runs on its own task, separate
/// from LoopTask() (telnet/OTA/MQTT). If loop() blocks forever inside a
/// network call, telnet keeps working while the FSM silently stops -- this
/// pair lets LoopTask() notice and log it from the outside.
/// @{
static volatile uint32_t g_loop_alive_ms = 0;      ///< millis() at each loop() iteration start
static TaskHandle_t g_main_loop_task_handle = NULL; ///< Handle of the task running loop(), captured on first iteration
/// General-purpose marker for blocking calls outside librelinkup (e.g. post-fetch
/// UI/LittleFS work, WireGuard check, MQTT connect). See librelinkup.breadcrumb()
/// for the LibreLinkUp-HTTP-specific counterpart.
volatile const char* g_loop_breadcrumb = "idle";

/// Feed the hang-detector heartbeat from inside a long, legitimately blocking
/// single loop() iteration (e.g. the FW_INSTALLING download/flash), so
/// LoopTask() doesn't mistake genuine progress for a stall. g_loop_alive_ms
/// is otherwise only touched once per loop() iteration, which is too coarse
/// for a step that can run 30+ seconds without returning.
void feed_loop_heartbeat()
{
    g_loop_alive_ms = millis();
}
/// Hard task-watchdog timeout for the task running setup()/loop() (FSM/fetch/publish).
/// Must comfortably exceed any legitimate single blocking step (slow TLS handshake,
/// firmware chunk download, ...) while still recovering promptly from a true hang.
static const uint32_t LOOP_TASK_WDT_TIMEOUT_S = 90;
/// @}

volatile uint32_t g_lv_mem_total = 0;      ///< pool size reported by LVGL, bytes
volatile uint32_t g_lv_mem_used_max = 0;   ///< LVGL's own high-water mark, bytes
volatile uint32_t g_lv_mem_free = 0;       ///< free bytes at the last sample
volatile uint8_t  g_lv_mem_used_pct = 0;   ///< pool usage at the last sample
volatile uint8_t  g_lv_mem_frag_pct = 0;   ///< fragmentation at the last sample

/// Refresh the LVGL pool figures. Cheap, but pointless more than once a second.
///
/// Only ever called from the Arduino loop task: lv_mem_monitor() walks the pool's
/// block list, so doing it from another task while this one allocates would read
/// half-updated structures. Everything else (esp_status on the console task, the
/// stall log on LoopTask) reads the sampled values below instead.
static void sample_lvgl_mem()
{
    static uint32_t last_ms = 0;
    const uint32_t now = millis();
    if ((uint32_t)(now - last_ms) < 1000) return;
    last_ms = now;

    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    g_lv_mem_total     = (uint32_t)m.total_size;
    g_lv_mem_used_max  = (uint32_t)m.max_used;
    g_lv_mem_free      = (uint32_t)m.free_size;
    g_lv_mem_used_pct  = m.used_pct;
    g_lv_mem_frag_pct  = m.frag_pct;
}

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

/// Number of log messages a telnet shell may buffer before it starts discarding.
///
/// uuid::console::Shell::MAX_LOG_MESSAGES defaults to 20, and on overflow the
/// *oldest* message is dropped (shell_log.cpp). A command that logs more than
/// that in one burst therefore loses its first lines: esp_status() emits ~27 and
/// silently lost its whole heap block. The shell cannot drain in between because
/// it runs on the same LoopTask that is busy inside the command.
static const size_t SHELL_LOG_QUEUE = 64;

/// Custom shell: overrides display_banner() to show a welcome screen on connect.
class AppTelnetShell : public uuid::console::Shell {
public:
    AppTelnetShell(Stream &stream, std::shared_ptr<uuid::console::Commands> commands)
        : uuid::console::Shell(stream, commands) {
        maximum_log_messages(SHELL_LOG_QUEUE);
    }
protected:
    void display_banner() override { displayWelcomeBanner(*this); }
};

/// Console command handler instance
static std::shared_ptr<uuid::console::Commands> commands = std::make_shared<uuid::console::Commands>();

/// Telnet service: shell factory so display_banner() fires on each new connection.
static uuid::telnet::TelnetService telnet{
    [](Stream &stream, IPAddress, uint16_t) -> std::shared_ptr<uuid::console::Shell> {
        return std::make_shared<AppTelnetShell>(stream, commands);
    }
};

/// Logger instance for this module
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

///////////////////// WIREGUARD VPN ////////////////////

#include <WireGuard-ESP32.h>
#include "wireguardif.h"

WireGuard wg; ///< WireGuard VPN client instance

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
#include "ui_display.h"

///////////////////// TIME ZONE CONFIGURATION ////////////////////

/// Time zone string for Central European Time (CET/CEST)
/// Format: std offset dst [offset],start[/time],end[/time]
const char *tz = "CET-1CEST,M3.5.0/2,M10.5.0/3";

///////////////////// OTA UPDATE ////////////////////

#include <ElegantOTA.h>
#include "http_update.h"
#include "ota_handler.h"

AsyncWebServer server(80); ///< Async web server for OTA and config

bool ota_in_progress = 0; ///< OTA update in progress flag


///////////////////// WIFI BACKGROUND SCAN ////////////////////

TaskHandle_t scanTaskHandle; ///< WiFi scan task handle

///////////////////// WEB INTERFACE ////////////////////

#include "webpage.h"

///////////////////// OTA CALLBACKS ////////////////////

/**
 * @brief OTA update start callback
 *
 * Called when OTA update begins. Pauses background tasks and
 * sets OTA progress flag.
 */

///////////////////// SOFTWARE TIMERS ////////////////////

///////////////////// JSON CONFIGURATION ////////////////////

/// Enable double precision for JSON parsing
#define ARDUINOJSON_USE_DOUBLE 1

///////////////////// WIFI MANAGER ////////////////////

#include <WiFi.h>
#include <WiFiMulti.h>

WiFiMulti wifiMulti; ///< WiFi multi-connection manager

/// WiFi connection timeout in milliseconds
const uint32_t connectTimeoutMs = 5000;

/// SSID of the network that last failed the internet check; skipped on next reconnect.
static String g_wifi_skip_ssid;

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

// Resolves host (IP string or hostname) then TCP-connects to port.
// Returns 1 on success, 0 on DNS failure or connect timeout.
int app_check_host_status(const char* host, uint16_t port)
{
    IPAddress addr;
    if (!WiFi.hostByName(host, addr))
    {
        logger.debug("[probe] DNS failed for %s", host);
        return 0;
    }

    // Logged because the raw result is a bare 0/1: when the FSM declares the
    // tunnel "sick" while MQTT is demonstrably exchanging data, the resolved
    // address is the first thing worth comparing against the address the MQTT
    // client actually uses.
    const int result = helper.check_internet_status(addr, port);
    logger.debug("[probe] %s -> %s:%u = %s",
                 host, addr.toString().c_str(), (unsigned)port,
                 result ? "reachable" : "FAILED");
    return result;
}

void start_ap_mode(); // defined later in this file

/**
 * @brief Attempts to recover from internet disconnection by restarting WiFi
 *
 * Logs the event and increments the WiFi restart counter.
 */
void app_recover_offline()
{
    DBGprint;
    esp_status_counter_wifi_restart++;

    logger.notice("Client offline on '%s' -> starting AP mode for reconfiguration",
                  WiFi.SSID().c_str());
    start_ap_mode();
}

///////////////////// MQTT CLIENT ////////////////////

#include "mqtt.h"
#include "mqtt_handler.h"

MQTT mqtt; ///< MQTT configuration and helper class

WiFiClient mqttClient;                ///< WiFi client for MQTT connection
PubSubClient mqtt_client(mqttClient); ///< MQTT client instance

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

    // LVGL allocates from its own fixed pool, not from the ESP heap above -- the
    // two say nothing about each other. Values come from the loop task's last
    // sample (see sample_lvgl_mem); this runs on the console task.
    logger.notice("LVGL mem: %u%% used, max_used %u/%u Bytes, free %u, frag %u%%",
                  (unsigned)g_lv_mem_used_pct, (unsigned)g_lv_mem_used_max,
                  (unsigned)g_lv_mem_total, (unsigned)g_lv_mem_free,
                  (unsigned)g_lv_mem_frag_pct);

    logger.notice("WiFi Reconnects : %d", esp_status_counter_wifi_restart);
    logger.notice("WG Reinits      : %d", esp_status_counter_wg_reinit);
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
    app_fsm_notify_user_activity(g_fsm);
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
/// Opens the firmware info screen from the update hint on the main screen.
static void fw_hint_click_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    app_fsm_notify_user_activity(g_fsm);
    ui_fwinfo_refresh();
    lv_disp_load_scr(ui_FWInfo_screen);
}

/// @name Firmware action requests
/// @brief The buttons run inside lv_timer_handler(), while
/// fw_update_request_install() takes a mutex with a one second wait. Blocking
/// there would stall the renderer, so the buttons only record the wish and
/// loop() performs it afterwards.
/// @{
static volatile int8_t g_fw_action_request = 0;  ///< 1 = install, 2 = check

static void process_fw_action_request()
{
    const int8_t action = g_fw_action_request;
    g_fw_action_request = 0;
    if (action == 0) return;

    if (action == 1)
    {
        String msg;
        const bool ok = fw_update_request_install(msg);
        logger.notice("FW install requested from touch UI: %s (%s)",
                      ok ? "accepted" : "rejected", msg.c_str());
    }
    else
    {
        fw_update_request_check_now();
        logger.notice("FW check requested from touch UI");
    }
    ui_fwinfo_refresh();
}
/// @}

static void btn_fwinfo_install_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    app_fsm_notify_user_activity(g_fsm);
    g_fw_action_request = 1;
    // fw_ui_start_install() switches to the progress ring screen by itself once
    // the FSM picks the request up, so nothing to load here.
}

static void btn_fwinfo_check_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    app_fsm_notify_user_activity(g_fsm);
    g_fw_action_request = 2;
}

static void btn_fwinfo_cancel_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    app_fsm_notify_user_activity(g_fsm);
    lv_disp_load_scr(ui_Main_screen);
}

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
        if (screen_rotation_next(active, -1) == ui_FWInfo_screen) ui_fwinfo_refresh();
        lv_disp_load_scr(screen_rotation_next(active, -1));
        break;

    case LV_DIR_RIGHT:
        logger.debug("Touch gesture: right");
        if (screen_rotation_next(active, 1) == ui_FWInfo_screen) ui_fwinfo_refresh();
        lv_disp_load_scr(screen_rotation_next(active, 1));
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

static void btn_fw_check_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    logger.debug("FW button clicked");
    if (fw_update_is_update_available())
    {
        String msg;
        fw_update_request_install(msg);
    }
    else
    {
        fw_update_request_check_now();
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

        // Save configuration to file
        settings.saveConfiguration(settings.config_filename, settings.config);

        // Switch to main screen
        lv_disp_load_scr(ui_Main_screen);
    }
}

///////////////////// CHART INTERACTION CALLBACKS ////////////////////


/// True while the main glucose header shows a touched value instead of the
/// live reading. Guards the restore below so a drag does not rewrite the
/// header labels on every LV_EVENT_PRESSING tick.
static bool s_header_shows_cursor = false;

/**
 * @brief Hides the chart touch cursor and restores the live glucose header.
 *
 * touch_event_cb() writes the touched reading into the main glucose header, so
 * ending the touch has to put the live reading back. The validity gate is the
 * same one the fetch loop uses: without a valid timestamp the header falls back
 * to dashes (mode 0) instead of showing a stale value.
 */
static void chart_cursor_hide(void)
{
    lv_obj_add_flag(ui_Chart_Cursor_Line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Chart_Cursor_Dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Chart_Cursor_Time_Label, LV_OBJ_FLAG_HIDDEN);

    if (!s_header_shows_cursor)
        return;
    s_header_shows_cursor = false;

    const bool live_valid =
        (librelinkup.status().timestamp_status == SENSOR_TIMECODE_VALID);

    draw_labels(live_valid ? 1 : 0,
                librelinkup.glucose_data().measurement_color,
                librelinkup.glucose_data().glucoseMeasurement,
                librelinkup.glucose_data().str_trendArrow,
                librelinkup.glucose_data().str_TrendMessage,
                live_valid ? glucose_delta : 0);
}

/**
 * @brief Charted value at point @p id, or LV_CHART_POINT_NONE.
 *
 * The glucose curve is drawn from two series -- in-range points live in
 * glucoseValueSeries_5Min, out-of-range points in glucoseValueSeries_alert
 * (see draw_chart_glucose_data()) -- so a point has to be looked up in both.
 * Reading the value back out of the chart instead of out of graph_data[] is
 * what keeps the cursor on the drawn curve: it is the same array LVGL renders
 * from, so the dot cannot disagree with the line.
 *
 * @param[in]  chart   The glucose chart.
 * @param[in]  id      Chart point id.
 * @param[out] ser_out Series that owns the point (may be NULL).
 * @return Value in mg/dL, or LV_CHART_POINT_NONE if no series has this point.
 */
static int32_t chart_value_at(lv_obj_t *chart, int id, lv_chart_series_t **ser_out)
{
    lv_chart_series_t *candidates[2] = { glucoseValueSeries_5Min, glucoseValueSeries_alert };

    for (int i = 0; i < 2; i++)
    {
        int32_t *y_array = lv_chart_get_series_y_array(chart, candidates[i]);
        if (y_array != NULL && y_array[id] != LV_CHART_POINT_NONE && y_array[id] != 0)
        {
            if (ser_out != NULL)
                *ser_out = candidates[i];
            return y_array[id];
        }
    }

    return LV_CHART_POINT_NONE;
}

/**
 * @brief Chart touch release callback.
 *
 * Bound to LV_EVENT_RELEASED and LV_EVENT_PRESS_LOST on the chart.
 *
 * @param[in] e LVGL event data (unused; same handler for both events)
 */
static void chart_touch_release_cb(lv_event_t *e)
{
    (void)e;
    chart_cursor_hide();
}

/**
 * @brief Touch event callback for glucose chart interaction
 *
 * Shows a crosshair at the touched position, matching the official
 * LibreLinkUp app: a vertical line, a dot on the curve, and a time readout
 * pinned below it at the x-axis. The touched glucose value goes into the main
 * glucose header rather than into a label that slides along with the finger --
 * a readout moving under the fingertip was hard to read on the panel.
 *
 * ui_Chart_Glucose_5Min_last_point_marker (the green "live value" dot) is
 * intentionally left alone here -- it used to be reused as the touch marker,
 * which meant touching the chart hijacked the live-value indicator until the
 * next periodic redraw put it back. It now always shows the live reading,
 * same as the app's screenshot, while ui_Chart_Cursor_Dot is the separate
 * touch indicator.
 *
 * Algorithm:
 * 1. Convert screen coordinates to chart-relative coordinates
 * 2. Map X-coordinate to data point index
 * 3. Retrieve glucose value + timestamp at that index
 * 4. Ask LVGL for the pixel position of that point
 * 5. Position the crosshair line and dot, write the header, place the time
 *
 * @param[in] e LVGL event data containing touch information
 *
 * @note Cursor children are positioned in content-box coordinates, while
 *       lv_chart_get_point_pos_by_id() reports from the chart's outer edge --
 *       content_ofs_x/y bridges the two
 * @note The y mapping is LVGL's own, so lv_chart_set_range() stays the single
 *       source of truth for the axis
 * @note Cursor is hidden if the touched position has no valid data
 *
 * @warning Index clamping prevents out-of-bounds array access
 */
static void touch_event_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_obj_t *chart = ui_Chart_Glucose_5Min;

    // The touch point is in display coordinates, while a chart lays its points
    // out inside its *content* box -- the default theme puts a pad_small on
    // charts, so that box is inset by roughly 10-14 px per side. Children
    // positioned with lv_obj_set_pos() live in that same content box, so
    // everything below is content-relative and the two agree by construction.
    lv_area_t chart_area;
    lv_area_t content;
    lv_obj_get_coords(chart, &chart_area);
    lv_obj_get_content_coords(chart, &content);
    lv_coord_t content_w = lv_area_get_width(&content);

    // Distance from the chart's outer edge to its content box (pad + border).
    // lv_chart_get_point_pos_by_id() adds this to every point it reports, but
    // lv_obj_set_pos() on a child already counts from the content box -- so it
    // has to come back off, or the cursor sits pad_left/pad_top off the curve.
    lv_coord_t content_ofs_x = content.x1 - chart_area.x1;
    lv_coord_t content_ofs_y = content.y1 - chart_area.y1;

    int num_points = (int)lv_chart_get_point_count(chart);
    if (num_points < 2 || content_w <= 0)
        return;

    lv_coord_t relative_x = point.x - content.x1;

    // Same mapping LVGL uses internally (lv_chart.c: get_index_from_x): a line
    // chart puts point id at x = content_w * id / (num_points - 1), so the
    // nearest point is round(x * (num_points - 1) / content_w).
    int index;
    if (relative_x <= 0)
        index = 0;
    else if (relative_x >= content_w)
        index = num_points - 1;
    else
        index = ((int)relative_x * (num_points - 1) + content_w / 2) / content_w;

    // Hide cursor if no series has a value at this point
    lv_chart_series_t *ser = NULL;
    int32_t charted = chart_value_at(chart, index, &ser);
    if (charted == LV_CHART_POINT_NONE)
    {
        chart_cursor_hide();
        return;
    }
    const int16_t value = (int16_t)charted;

    // Exact pixel of that point, straight from LVGL: no second copy of the
    // value->y mapping here that could drift from lv_chart_set_range().
    lv_point_t p;
    lv_chart_get_point_pos_by_id(chart, ser, (uint32_t)index, &p);

    const lv_coord_t point_x = p.x - content_ofs_x;
    const lv_coord_t point_y = p.y - content_ofs_y;

    const bool out_of_range = (value >= librelinkup.glucose_data().glucosetargetHigh ||
                                value <= librelinkup.glucose_data().glucoseAlarmLow);

    // Cursor geometry, logged once per point (not per touch tick) so the
    // numbers stay readable while dragging. If the cursor ever looks off the
    // curve again, this says whether the touch->index mapping or the point
    // position is to blame, instead of leaving it to guesswork.
    static int last_logged_index = -1;
    if (index != last_logged_index)
    {
        last_logged_index = index;
        logger.debug("cursor: idx=%d rel_x=%d content=[x%d y%d %dx%d] point=(%d,%d) child=(%d,%d) val=%d",
                      index, (int)relative_x, (int)content.x1, (int)content.y1,
                      (int)content_w, (int)lv_area_get_height(&content),
                      (int)p.x, (int)p.y, (int)point_x, (int)point_y, (int)value);
    }

    // --- Vertical crosshair line, clamped inside the chart ---
    // Snapped to the point, not to the fingertip, so line, dot and curve meet.
    lv_coord_t line_x = point_x - 1; // center the 2px line on the point
    if (line_x < 0) line_x = 0;
    if (line_x > content_w - 2) line_x = content_w - 2;
    lv_obj_set_pos(ui_Chart_Cursor_Line, line_x, 0);
    lv_obj_clear_flag(ui_Chart_Cursor_Line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_Chart_Cursor_Line);

    // --- Dot on the curve at the touched point ---
    lv_obj_set_style_border_color(ui_Chart_Cursor_Dot,
                                  out_of_range ? lv_palette_main(LV_PALETTE_RED)
                                               : lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_pos(ui_Chart_Cursor_Dot, point_x - 7, point_y - 7);
    lv_obj_clear_flag(ui_Chart_Cursor_Dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_Chart_Cursor_Dot);

    // --- Touched value into the main glucose header ---
    // The delta line keeps its meaning while scrubbing: it reports the touched
    // point against the previous valid sample, not the live delta.
    int16_t cursor_delta = 0;
    for (int prev = index - 1; prev >= 0; prev--)
    {
        int32_t prev_value = chart_value_at(chart, prev, NULL);
        if (prev_value != LV_CHART_POINT_NONE)
        {
            cursor_delta = (int16_t)(value - prev_value);
            break;
        }
    }

    draw_labels(1,                      // mode 1: show actual values
                out_of_range ? 4 : 1,   // GlucoseLabelColor RED : WHITE
                (uint16_t)value,
                librelinkup.glucose_data().str_trendArrow,
                librelinkup.glucose_data().str_TrendMessage,
                cursor_delta);
    s_header_shows_cursor = true;

    // --- Time readout, pinned near the bottom (x-axis), same clamping ---
    char time_text[8] = "--:--";
    uint32_t point_ts = librelinkup.sensor_history_data().timestamp[index];
    if (point_ts > 0)
    {
        helper.format_time(time_text, sizeof(time_text), (time_t)point_ts);
    }
    lv_label_set_text(ui_Chart_Cursor_Time_Label, time_text);
    lv_coord_t time_w = lv_obj_get_width(ui_Chart_Cursor_Time_Label);
    lv_coord_t time_x = point_x - time_w / 2;
    if (time_x < 0) time_x = 0;
    if (time_x > content_w - time_w) time_x = content_w - time_w;
    lv_obj_set_pos(ui_Chart_Cursor_Time_Label, time_x, CHART_HEIGHT - 41);
    lv_obj_clear_flag(ui_Chart_Cursor_Time_Label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_Chart_Cursor_Time_Label);
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

    // Keep target limit lines visible even if the sensor data timestamp is invalid.
    draw_chart_glucose_data(3);
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
 */
void update_trend_message()
{
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
            librelinkup.glucose_data().str_TrendMessage =
            String("sensor ready in ") + remaining_time + " min";
            logger.notice("Sensor in starting phase!");
        }
        break;

    case SENSOR_READY:
    {
        // A sensor that has just finished warmup has not produced a reading yet,
        // so the newest measurement still belongs to the *previous* sensor and is
        // over an hour old. check_valid_timestamp_factory() only judges age, so it
        // reports SENSOR_LOST -- alarming, and wrong. Comparing the measurement
        // against this sensor's activation time (API field sensor.a) separates
        // "no reading from this sensor yet" from "sensor really lost".
        const uint32_t activation = librelinkup.sensor_data().sensor_non_activ_unixtime;
        int64_t meas_epoch = 0;
        if (activation != 0 &&
            parse_factory_timestamp_utc(librelinkup.glucose_data().str_measurement_factorytimestamp,
                                        meas_epoch) &&
            meas_epoch < (int64_t)activation)
        {
            librelinkup.glucose_data().str_TrendMessage = "waiting for data";
            logger.notice("Sensor ready, newest measurement predates activation -> waiting for first reading");
            return;
        }

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
        g_loop_breadcrumb = "fetch.draw_chart_5min";
        draw_chart_glucose_data(1); // Perform 5-minute update

        if (librelinkup.status().sensor_state == SENSOR_READY)
        {
            g_loop_breadcrumb = "fetch.json_logging";
            update_glucose_json_logging();
            //glucose_statistics(); // Print glucose statistics
        }
        g_loop_breadcrumb = "idle";
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

    // Track loop()-task stack headroom on every fetch cycle. TLS handshakes and
    // JSON parsing are the most stack-hungry paths here; a shrinking watermark
    // over time would point to a slow stack-overflow rather than a network hang.
    logger.debug("loop task stack high water mark: %u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));

    lcd_status_indication(0, 1); // Hide activity indicator

    g_loop_breadcrumb = "fetch.post_process";

    // Refresh cached sensor_runtime from the sensor type. We deliberately do
    // NOT switch the visible bar here - draw_chart_sensor_valid() below picks
    // the correct bar (days/hours/minutes) for the current remaining lifetime.
    // The previous unconditional dayBar switch caused a visible flicker each
    // cycle once remaining lifetime fell into hours/minutes mode.
    (void)librelinkup.check_sensor_type();

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

    // Always update sensor validity progress bar regardless of timestamp status
    draw_chart_sensor_valid();

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
        else if (glucoseMeasurement_backup == 0)
        {
            // There is no previous reading after boot. Do not report the
            // current glucose value as the first delta.
            glucose_delta = 0;
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

        g_loop_breadcrumb = "fetch.draw_ui";
        draw_labels(true, librelinkup.glucose_data().measurement_color,
                    librelinkup.glucose_data().glucoseMeasurement,
                    librelinkup.glucose_data().str_trendArrow,
                    librelinkup.glucose_data().str_TrendMessage,
                    glucose_delta);
        draw_chart_glucose_data(3);

        glucoseMeasurement_backup = librelinkup.glucose_data().glucoseMeasurement;

        // g_lv_mem_total, not LV_MEM_SIZE: that macro only exists while the
        // builtin allocator is selected and would break the build under CLIB.
        logger.debug("LVGL mem: used=%u%% max_used=%u/%u bytes free=%u frag=%u%%",
                      (unsigned)g_lv_mem_used_pct, (unsigned)g_lv_mem_used_max,
                      (unsigned)g_lv_mem_total, (unsigned)g_lv_mem_free,
                      (unsigned)g_lv_mem_frag_pct);
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

    g_loop_breadcrumb = "idle";
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

    // Writing to LittleFS disables the flash cache, which starves the RGB
    // panel's DMA and leaves the picture torn until something redraws it.
    // Bounce buffers (see Arduino_ESP32RGBPanel.cpp) widen the tolerance but
    // cannot close the window entirely, because the refill ISR is not
    // IRAM-safe in the prebuilt IDF libraries. Repainting right afterwards
    // repairs whatever the write disturbed instead of leaving it on screen
    // until the next unrelated update.
    lv_obj_t *active_screen = lv_screen_active();
    if (active_screen != NULL)
        lv_obj_invalidate(active_screen);
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
/// Main-loop stall considered "hung" after this many ms without a heartbeat update.
static const uint32_t LOOP_HANG_THRESHOLD_MS = 30000;
/// Minimum gap between repeated hang log lines, to avoid flooding the log while stuck.
static const uint32_t LOOP_HANG_LOG_REPEAT_MS = 60000;

void LoopTask(void *pvParameters)
{
    Serial.println("Loop Task started...");

    uint32_t last_hang_check_ms = 0;
    uint32_t last_hang_log_ms = 0;

    while (1)
    {
        // ElegantOTA update handling
        ElegantOTA.loop();

        //  Telnet console handling
        uuid::loop();
        telnet.loop();
        Shell::loop_all();
        yield();

        // --- loop() hang detection ------------------------------------------
        // Runs here because this task is independent of the Arduino loop()
        // task that drives the FSM/glucose fetch: if that task blocks forever
        // inside a network call, this task keeps running and can report it
        // (that's also why telnet stays reachable while the FSM appears dead).
        uint32_t now = millis();
        if (now - last_hang_check_ms >= 5000)
        {
            last_hang_check_ms = now;
            uint32_t stall_ms = now - g_loop_alive_ms;
            if (stall_ms >= LOOP_HANG_THRESHOLD_MS &&
                (now - last_hang_log_ms >= LOOP_HANG_LOG_REPEAT_MS))
            {
                last_hang_log_ms = now;
                UBaseType_t hwm = g_main_loop_task_handle
                                      ? uxTaskGetStackHighWaterMark(g_main_loop_task_handle)
                                      : 0;
                logger.err(
                    "MAIN LOOP STALLED for %ums (fsm_state=%s, llu_breadcrumb=%s, loop_breadcrumb=%s, "
                    "free_heap=%u, min_free_heap=%u, loop_stack_hwm=%u bytes)",
                    stall_ms, app_fsm_state_name(g_fsm.state),
                    librelinkup.breadcrumb(), g_loop_breadcrumb,
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                    (unsigned)hwm);
            }
        }

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
    ui_warmup_screen(); // Sensor warmup progress ring (hidden until SENSOR_STARTING)
    lv_label_set_text(ui_Label_WelcomeInfo, "LibreLinkUp\nClient");
    lv_timer_handler(); // Let the GUI do its work

    // Keep pumping LVGL until the logo fade-in (started in
    // ui_Welcome_screen_init()) finishes, otherwise the upcoming blocking
    // setup_wifi() call freezes rendering mid-fade and it jumps straight
    // to full opacity instead of animating.
    const uint32_t fade_deadline_ms = millis() + 2000;
    while (millis() < fade_deadline_ms) {
        lv_timer_handler();
        delay(5);
    }
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

    // settings.config.mqtt* (config.json / web UI) was never applied to the
    // live mqtt object that setup_mqtt() actually connects with - see
    // MQTT::applyConfig() in mqtt.h.
    mqtt.applyConfig(settings.config.mqttServer, settings.config.mqtt_port,
                      settings.config.mqttUsername, settings.config.mqttPassword);
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

bool g_ap_mode = false;       // non-static: accessed from ota_handler.cpp
bool g_force_ap_mode = false; // set when internet check fails → stay in AP

/**
 * @brief Switches the device to AP-only mode so the user can reach the config page.
 *
 * Called when an internet check fails and no fallback network is available.
 * Sets g_ap_mode and g_force_ap_mode so that ensure_wifi_connected() stops
 * calling setup_wifi() and the FSM keeps cycling in BACKOFF while the AP lives.
 */
void start_ap_mode()
{
    g_ap_mode        = true;
    g_force_ap_mode  = true;

    WiFi.disconnect(true, true);
    delay(100);
    WiFi.mode(WIFI_AP);

    IPAddress ip(192, 168, 4, 1), gw(192, 168, 4, 1), sn(255, 255, 255, 0);
    WiFi.softAPConfig(ip, gw, sn);
    WiFi.softAP(settings.apSSID, settings.apPassword);
    const IPAddress ap_ip = WiFi.softAPIP();

    DBGprint;
    Serial.println("Access Point started, AP:");
    Serial.println(settings.apSSID);
    Serial.print("AP IP: ");
    Serial.println(ap_ip);

    String msg = "Wifi AP " + ap_ip.toString();
    lv_label_set_text(ui_Label_WelcomeWifiInfo, msg.c_str());
    lv_timer_handler();
    delay(300);

    // Rebind telnet server on the new AP interface so it's reachable at 192.168.4.1
    telnet.start();
}

void setup_wifi()
{
    lv_label_set_text(ui_Label_WelcomeWifiInfo, "connecting to Wifi...");
    lv_timer_handler();

    // --- STA ONLY attempt -------------------------------------------------
    g_ap_mode = false;

    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.setSleep(false);

    // Rebuild wifiMulti from scratch each call so we can skip the failing network.
    wifiMulti = WiFiMulti();

    auto add_networks = [&](bool skip_bad) {
        int added = 0;
        if (!settings.config.wifi_networks.empty()) {
            for (const auto& net : settings.config.wifi_networks) {
                if (skip_bad && net.ssid == g_wifi_skip_ssid) continue;
                wifiMulti.addAP(net.ssid.c_str(), net.password.c_str());
                added++;
            }
        } else {
            if (!skip_bad || settings.config.wifi_bssid != g_wifi_skip_ssid) {
                wifiMulti.addAP(settings.config.wifi_bssid.c_str(),
                                settings.config.wifi_password.c_str());
                added++;
            }
        }
        return added;
    };

    // First try without the bad network; if nothing else is configured, use all.
    if (g_wifi_skip_ssid.length() > 0) {
        logger.notice("WiFi: skipping '%s' (no internet last time), trying others first",
                      g_wifi_skip_ssid.c_str());
        if (add_networks(true) == 0) {
            logger.notice("WiFi: no alternative network available, retrying '%s'",
                          g_wifi_skip_ssid.c_str());
            add_networks(false);
        }
    } else {
        add_networks(false);
    }
    g_wifi_skip_ssid = ""; // clear after building the list

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

        // When WireGuard is active, disable WiFi auto-reconnect so that
        // reconnection always flows through the FSM (WIFI_CONNECT → VPN_CHECK).
        // Auto-reconnect bypasses the FSM and leaves the WG tunnel in a stale
        // state after WiFi comes back — the tunnel needs an explicit reinit.
        const bool wg_active = (settings.config.wg_mode == 1);
        WiFi.setAutoReconnect(!wg_active);
        WiFi.persistent(true);
        logger.notice("[WiFi] connected: SSID=%s IP=%s autoReconnect=%s",
                      WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str(),
                      wg_active ? "OFF(WG)" : "ON");
        return;
    }

    // WiFi connect failed — return without starting AP mode.
    // The FSM (WIFI_CONNECT state) tracks consecutive failures and triggers
    // start_ap_mode() only after wifi_ap_threshold attempts, so a single
    // transient failure (e.g. router IP change) does not lock the device.
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

bool wg_is_initialized()
{
    return wg.is_initialized();
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

    // If already initialized and not forcing, trust the WG interface state.
    // We do NOT TCP-ping the MQTT broker here: the broker being momentarily
    // unreachable is not evidence of a WG failure and would cause an unnecessary
    // full reinit (which blocks the main loop for ~20s).
    if (wg.is_initialized() && !force_reinit)
    {
        logger.notice("[setup_wg] WG already initialized, skipping reinit.");
        g_wg_busy = false;
        return true;
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

    if (!helper.timeLooksValid())
    {
        logger.notice("[setup_wg] system time not valid yet, retrying NTP sync...");
        const bool synced = helper.ensureTimeSynced();
        logger.notice("[setup_wg] time check epoch=%ld valid=%d",
                      (long)time(nullptr), (int)synced);
    }

    bool begin_ok = false;
    constexpr uint8_t wg_begin_attempts = 3;
    for (uint8_t attempt = 1; attempt <= wg_begin_attempts; ++attempt)
    {
        IPAddress endpoint_ip;
        if (!helper.resolveHostnameIPv4(settings.config.wgEndpoint, endpoint_ip, 5, 400))
        {
            logger.notice("[setup_wg] WG endpoint DNS failed (%u/%u): %s",
                          (unsigned)attempt, (unsigned)wg_begin_attempts, settings.config.wgEndpoint.c_str());
            DBGprint;
            Serial.printf("[setup_wg] endpoint DNS failed (%u/%u): %s\n",
                          (unsigned)attempt, (unsigned)wg_begin_attempts, settings.config.wgEndpoint.c_str());
            if (attempt < wg_begin_attempts)
                delay(300);
            continue;
        }

        const String endpoint_ip_str = endpoint_ip.toString();
        logger.debug("[setup_wg] endpoint DNS resolved: %s -> %s",
                     settings.config.wgEndpoint.c_str(), endpoint_ip_str.c_str());
        DBGprint;
        Serial.printf("[setup_wg] endpoint DNS resolved: %s -> %s\n",
                      settings.config.wgEndpoint.c_str(), endpoint_ip_str.c_str());
        logger.notice("[setup_wg] wg.begin attempt %u/%u with endpoint=%s",
                      (unsigned)attempt, (unsigned)wg_begin_attempts, endpoint_ip_str.c_str());
        DBGprint;
        Serial.printf("[setup_wg] wg.begin attempt %u/%u endpoint=%s port=%u local_ip=%s\n",
                      (unsigned)attempt,
                      (unsigned)wg_begin_attempts,
                      endpoint_ip_str.c_str(),
                      (unsigned)settings.config.wgEndpointPort,
                      local_ip.toString().c_str());

        begin_ok = wg.begin(
            local_ip,
            settings.config.wgPrivateKey.c_str(),
            endpoint_ip_str.c_str(),
            settings.config.wgPublicKey.c_str(),
            (uint16_t)settings.config.wgEndpointPort,
            settings.config.wgPresharedKey.c_str()
        );

        DBGprint;
        Serial.printf("[setup_wg] wg.begin result attempt=%u ok=%d\n",
                      (unsigned)attempt, (int)begin_ok);

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

    // wg.begin() only reports that the interface was created -- it says nothing
    // about the handshake, which needs a round trip to the endpoint. Waiting for
    // the peer to actually come up avoids firing the first fetch into a tunnel
    // that is not carrying traffic yet.
    //
    // Bounded on purpose: setup_OTA() runs after this, so blocking here for long
    // would delay the recovery path that lets us re-flash over LAN.
    constexpr uint32_t wg_peer_wait_ms = 5000;
    constexpr uint32_t wg_peer_poll_ms = 100;

    IPAddress peer_ip;
    bool peer_up = false;
    const uint32_t wait_start = millis();
    while ((millis() - wait_start) < wg_peer_wait_ms)
    {
        if (wg.isUp(peer_ip))
        {
            peer_up = true;
            break;
        }
        lv_timer_handler(); // keep the UI alive while waiting
        delay(wg_peer_poll_ms);
    }
    const uint32_t waited_ms = millis() - wait_start;

    bool ok = begin_ok; // wg.begin() succeeded → interface exists

    if (peer_up)
        logger.notice("[setup_wg] peer handshake up after %ums, peer=%s",
                      (unsigned)waited_ms, peer_ip.toString().c_str());
    else
        logger.notice("[setup_wg] peer did NOT come up within %ums -- interface is "
                      "created but no traffic will pass yet", (unsigned)wg_peer_wait_ms);

    Serial.printf("[setup_wg] post-begin: wg.begin ok=%d, peer_up=%d after %ums\n",
                  (int)ok, (int)peer_up, (unsigned)waited_ms);

    if (ok)
    {
        if (force_reinit)
            esp_status_counter_wg_reinit++;
        logger.notice("[setup_wg] WG connected! IP:%s (reinit#%u)",
                      local_ip.toString().c_str(), (unsigned)esp_status_counter_wg_reinit);
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

/// Root CA bundle embedded from data/cert/x509_crt_bundle.bin (see platformio.ini embed_files)
extern const uint8_t x509_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_data_cert_x509_crt_bundle_bin_end");
/// Bundle length, required by Arduino core 3.x setCACertBundle().
static const size_t x509_crt_bundle_size = x509_crt_bundle_end - x509_crt_bundle_start;

/**
 * @brief Initializes LibreLinkUp API client
 *
 * If no credentials stored, shows login screen.
 * Otherwise begins API client and validates its TLS connection against the
 * embedded root CA bundle instead of a single pinned certificate.
 *
 * @note Login screen displayed if credentials empty
 *
 * @see librelinkup.begin()
 * @see librelinkup.setCACertBundle()
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
    uint8_t ca_mode = 3; // 0=none, 1=API_ROOT_CA, 2=GoogleTrustService Root R4 from littleFS, 3=bundle
    librelinkup.begin(ca_mode);
    if (ca_mode == 3){
        librelinkup.setCACertBundle(x509_crt_bundle_start, x509_crt_bundle_size);
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
    telnet.maximum_connections(3);
    telnet.initial_idle_timeout(3600);
    telnet.default_write_timeout(0);
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
        &LoopTaskHandle, // Task handle
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

    // Report why we rebooted, in case it was the task watchdog below recovering
    // from a stuck main loop. On a WDT/panic reset, ESP-IDF also writes a full
    // backtrace to the "coredump" flash partition (already provisioned in
    // partitions_16mb_ota3m5.csv); retrieve it over USB with `pio run -t coredump`.
    {
        esp_reset_reason_t reset_reason = esp_reset_reason();
        if (reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_PANIC ||
            reset_reason == ESP_RST_INT_WDT)
        {
            Serial.printf("[BOOT] Last reset was caused by a watchdog/panic (reason=%d) -- "
                          "the main loop was likely stuck. See the coredump partition for a backtrace.\r\n",
                          (int)reset_reason);
        }
    }

    // Optional secondary UART (ESP32H2 <-> ESP32S3 IPC)
    setup_UART_IPC();

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
    fw_update_init();
    // setup_OTA(settings.config.ota_update); ///< Start/stop OTA + HTTP API (routes registered elsewhere)
    setup_OTA(settings.config.ota_update == 1 ? 1 : 0);

    // --- Background tasks ----------------------------------------------------
    setup_task(); ///< Launch LoopTask() on a dedicated core
    // ------------------------------------------------------------------------

    // ------------------------ LVGL callback registration ---------------------
    // Main screen: short press toggles brightness (handled by brightness_on_off_cb)
    lv_obj_add_event_cb(ui_Main_screen, brightness_on_off_cb, LV_EVENT_PRESSED, NULL);

    // Chart: crosshair cursor while pressing, hidden again on release/press-lost
    lv_obj_add_event_cb(ui_Chart_Glucose_5Min, touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(ui_Chart_Glucose_5Min, chart_touch_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(ui_Chart_Glucose_5Min, chart_touch_release_cb, LV_EVENT_PRESS_LOST, NULL);

    // Global gesture navigation across screens
    lv_obj_add_event_cb(ui_Main_screen, touch_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(ui_FWInfo_screen, touch_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(ui_Label_FWUpdateHint, fw_hint_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_btn_fwinfo_install, btn_fwinfo_install_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_btn_fwinfo_check, btn_fwinfo_check_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_btn_fwinfo_cancel, btn_fwinfo_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Debug_screen, touch_gesture_cb, LV_EVENT_GESTURE, NULL);

    // Toolbar buttons (WireGuard / MQTT / OTA toggles)
    lv_obj_add_event_cb(ui_btn_wireguard, btn_wireguard_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_btn_mqtt, btn_mqtt_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_btn_ota_update, btn_ota_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_btn_fw_check, btn_fw_check_cb, LV_EVENT_ALL, NULL);

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

    // --- Task watchdog for this task (setup()/loop(), i.e. the FSM/fetch/publish
    // path). Hard safety net: if this task doesn't get back to esp_task_wdt_reset()
    // (called each loop() iteration) within LOOP_TASK_WDT_TIMEOUT_S, ESP-IDF panics
    // with a full backtrace, writes a coredump to flash, and reboots -- turning a
    // silent, permanent hang into an automatic recovery plus a diagnosable crash log.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    // ESP-IDF 5.x (Arduino core 3.x) takes a config struct here instead of
    // (timeout, panic). Arduino may already have started the TWDT itself, in
    // which case init reports ESP_ERR_INVALID_STATE and the timeout has to be
    // applied with esp_task_wdt_reconfigure() instead.
    // Keep whatever idle-task watching the core was configured with instead of
    // silently switching it off -- Arduino enables CPU0 by default.
    uint32_t idle_core_mask = 0;
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0)
    idle_core_mask |= (1 << 0);
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1)
    idle_core_mask |= (1 << 1);
#endif

    esp_task_wdt_config_t twdt_config = {
        .timeout_ms     = LOOP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = idle_core_mask,
        .trigger_panic  = true,  // panic + coredump instead of a silent reset
    };

    // Arduino starts the TWDT itself when CONFIG_ESP_TASK_WDT_INIT is set, and
    // calling init() again logs an ESP_LOGE("TWDT already initialized") before
    // returning ESP_ERR_INVALID_STATE. Go straight to reconfigure() in that
    // case so the boot log stays free of a scary but meaningless error.
#if defined(CONFIG_ESP_TASK_WDT_INIT)
    esp_err_t twdt_err = esp_task_wdt_reconfigure(&twdt_config);
#else
    esp_err_t twdt_err = esp_task_wdt_init(&twdt_config);
#endif
    if (twdt_err != ESP_OK)
        logger.err("Task watchdog setup failed: %s", esp_err_to_name(twdt_err));
#else
    esp_task_wdt_init(LOOP_TASK_WDT_TIMEOUT_S, true);
#endif

    // Subscribe this task (setup()/loop()). Logged because a failure here is
    // exactly what produces "task_wdt: esp_task_wdt_reset(): task not found"
    // later on, and that error alone does not say why the task is missing.
    const esp_err_t twdt_add_err = esp_task_wdt_add(NULL);
    if (twdt_add_err != ESP_OK)
        logger.err("Task watchdog subscribe failed: %s", esp_err_to_name(twdt_add_err));
    else
        logger.notice("Task watchdog: loop task subscribed, timeout %us",
                      (unsigned)LOOP_TASK_WDT_TIMEOUT_S);

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
    // Heartbeat for the hang-detector in LoopTask(): updated unconditionally
    // (even during OTA) so a legitimate long-running OTA isn't mistaken for
    // a stuck FSM/glucose fetch.
    g_loop_alive_ms = millis();
    if (g_main_loop_task_handle == NULL)
    {
        g_main_loop_task_handle = xTaskGetCurrentTaskHandle();
    }
    esp_task_wdt_reset();

    // Only run main loop if no OTA update in progress
    if (ota_in_progress == false)
    {
        // NOTE: All other continuous loops run inside LoopTask().
        // Keep this loop short to maintain UI responsiveness.

        // --- UART IPC (ESP32H2 <-> ESP32S3) -------------------------------------
        zigbee_h2_poll_uart();

        // --- LVGL: let the GUI process pending work ------------------------------
        lv_timer_handler();
        sample_lvgl_mem();
        delay(1);

        if (g_sim_sensor_pending) {
            g_sim_sensor_pending = false;
            draw_chart_sensor_valid();
        }

        // --- Application state machine (connectivity/fetch/publish) -----------------
        app_fsm_poll(g_fsm);
        process_fw_action_request();

        // 1 s tick: update debug view labels if visible (and not during OTA)
        if (flag_debug_screen == true)
        {
            flag_debug_screen = false; // reset flag, will be set again by timer
            //logger.debug("1s timer tick: updating debug screen labels...");
            update_debug_screen();
        }
    }
}
