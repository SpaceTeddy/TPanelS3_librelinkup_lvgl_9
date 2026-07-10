/**
 * @file app_fsm.h
 * @brief Event/timeout-driven application state machine for this ESP32 project.
 *
 * Design goals:
 * - Deterministic sequencing: WiFi -> WireGuard(optional) -> MQTT(optional) -> Run
 * - Centralized timeouts/backoff handling
 * - Single-threaded MQTT usage (loop + publish) to avoid race conditions
 * - LVGL safety: intended to be polled from Arduino `loop()` (UI thread)
 */

#pragma once

#include <Arduino.h>


/**
 * @brief Application state machine states.
 *
 * Sequencing: BOOT → WIFI_CONNECT → VPN_CHECK → MQTT_CONNECT → RUN_IDLE
 * From RUN_IDLE the FSM fans out to RUN_FETCH, DISPLAY_DIM, INTERNET_CHECK,
 * BACKOFF, OTA_MODE, FW_CHECKING, and FW_INSTALLING as needed.
 */
enum class AppState : uint8_t
{
  BOOT = 0,       ///< Initial state; transitions immediately to WIFI_CONNECT.
  WIFI_CONNECT,   ///< Waiting for WiFi association.
  VPN_CHECK,      ///< Optional WireGuard VPN bring-up / health check.
  MQTT_CONNECT,   ///< Connecting to the MQTT broker (if enabled).
  RUN_IDLE,       ///< Normal operating state; orchestrates periodic tasks.
  RUN_FETCH,      ///< Fetching glucose data from LibreLink API.
  RUN_PUBLISH,    ///< Publishing data to MQTT.
  DISPLAY_DIM,    ///< Non-blocking display fade-to-black on inactivity.
  DISPLAY_UNDIM,  ///< Non-blocking display fade-up after user activity.
  INTERNET_CHECK, ///< Periodic TCP probe to verify internet reachability.
  BACKOFF,        ///< Exponential backoff after a recoverable failure.
  OTA_MODE,       ///< ElegantOTA web update in progress; FSM is suspended.
  FW_CHECKING,    ///< HTTP manifest check for a newer firmware version.
  FW_INSTALLING,  ///< Downloading and flashing a firmware binary.
};

/**
 * @brief Compile-time tunable timing parameters for the FSM.
 *
 * All values are in milliseconds unless the field name says otherwise.
 * Assign before calling app_fsm_init() to override defaults.
 */
struct AppFsmConfig
{
  uint32_t display_dim_timeout_ms = 300000;   ///< 5 min
  uint32_t display_dim_step_ms = 30;          ///< fade step interval
  uint32_t wifi_connect_timeout_ms = 20000;
  uint32_t mqtt_connect_timeout_ms = 8000;
  uint32_t mqtt_check_period_ms = 30000;        ///< periodic MQTT health check
  uint32_t fetch_period_ms = 60000;           ///< Master mode cadence
  uint32_t wg_check_period_ms = 60000;        ///< Optional WireGuard/ping cadence
  uint32_t wg_reboot_timeout_ms = 300000;     ///< Reboot if WG tunnel sick for this long (5 min)
  uint32_t backoff_min_ms = 2000;
  uint32_t backoff_max_ms = 60000;
  uint32_t internet_check_period_ms = 60000;  ///< periodic internet health check
  uint8_t  inet_fail_max = 3;                 ///< consecutive failures before triggering AP mode (covers ~3 min router reconnect)
  uint8_t  wifi_ap_threshold = 5;            ///< consecutive WIFI_CONNECT failures before AP fallback (~87s with default backoff)
  uint32_t debug_screen_period_ms = 1000; // 1s
};

/**
 * @brief Runtime state of the application FSM.
 *
 * Holds the current state, all timing bookmarks, display dimming state,
 * MQTT counters, and failure tracking. Initialise with app_fsm_init().
 *
 * @note Not thread-safe. All access must come from the same task/core
 *       that drives app_fsm_poll() (typically Arduino loop() on Core 1).
 */
struct AppFsm
{
  bool fetch_schedule_override = false; ///< If true, last_fetch_ms was adjusted for the next fetch

  AppState state = AppState::BOOT;

  uint32_t last_state_change_ms = 0;
  uint32_t last_fetch_ms = 0;
  uint32_t last_wg_check_ms = 0;
  uint32_t backoff_until_ms = 0;
  uint32_t last_mqtt_check_ms = 0;
  uint32_t mqtt_connect_started_ms = 0;
  uint32_t last_mqtt_attempt_ms = 0;
  uint32_t state_change_counter = 0;
  const char* last_transition_reason = nullptr;
  uint32_t last_backoff_ms = 0;

  uint32_t last_internet_check_ms = 0;
  uint32_t wg_sick_since_ms = 0; ///< millis() when WG trouble started; 0 = healthy
  uint8_t  inet_fail_count = 0;  ///< consecutive INTERNET_CHECK failures; AP mode only after reaching inet_fail_max
  uint8_t  wifi_fail_count = 0; ///< consecutive WIFI_CONNECT failures; AP fallback only after wifi_ap_threshold

  // Debug screen 1s polling
  uint32_t last_1s_tick_ms = 0;
  uint32_t last_debug_screen_ms = 0;

  // Display dimming
  uint32_t last_user_activity_ms = 0;
  uint32_t last_dim_step_ms = 0;
  uint8_t brightness_before_dim = 0;
  uint8_t display_undim_target = 0;
  bool display_dim_active = false;

  uint8_t consecutive_failures = 0;

  volatile bool mqtt_master_rx_pending = false;

  AppFsmConfig cfg;
};

/**
 * @brief Initializes the FSM.
 *
 * @param fsm Reference to the FSM instance to initialize.
 */
void app_fsm_init(AppFsm &fsm);

/**
 * @brief Human-readable name for an AppState value (diagnostic use).
 * @param s State to convert.
 * @return Null-terminated string literal (e.g. "RUN_IDLE"). Never nullptr.
 */
const char* app_fsm_state_name(AppState s);

/**
 * @brief Polls the FSM to handle state transitions and timeouts.
 *
 * This function should be called regularly (e.g., from the main loop) to allow the FSM
 * to process events and manage state transitions based on timeouts and conditions.
 *
 * @param fsm Reference to the FSM instance to poll.
 */
void app_fsm_poll(AppFsm &fsm);

/**
 * @brief Notify the FSM that a master MQTT data payload arrived (client mode).
 *
 * Call this from your existing mqtt_callback() at the place where you currently set
 * `flag_mqtt_master_rx = true;`.
 */
void app_fsm_notify_mqtt_master_rx(AppFsm &fsm);

/**
 * @brief Notify the FSM of user activity to reset display dimming.
 *
 * Call this from any place where you detect user activity that should prevent or
 * interrupt display dimming (e.g., touch input, button press, etc.).
 *
 * If the display is currently dimmed, this will also restore brightness and return
 * to RUN_IDLE.
 *
 * @param fsm Reference to the FSM instance to notify.
 */
void app_fsm_notify_user_activity(AppFsm &fsm);
