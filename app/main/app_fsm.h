/**
 * @file app_fsm.h
 * @brief Event/timeout-driven application state machine for this ESP32 project.
 *
 * @author Christian Weithe
 * @date 2026-03-04
 * @version 1.1
 * @copyright MIT
 * Design goals:
 * - Deterministic sequencing: WiFi -> WireGuard(optional) -> MQTT(optional) -> Run
 * - Centralized timeouts/backoff handling
 * - Single-threaded MQTT usage (loop + publish) to avoid race conditions
 * - LVGL safety: intended to be polled from Arduino `loop()` (UI thread)
 */

#pragma once

#include <Arduino.h>


/**
 * @brief Finite-state machine states used by the application.
 */
enum class AppState : uint8_t
{
  BOOT = 0,
  WIFI_CONNECT,
  VPN_CHECK,
  MQTT_CONNECT,
  RUN_IDLE,
  RUN_FETCH,
  RUN_PUBLISH,
  DISPLAY_DIM,
  INTERNET_CHECK,
  BACKOFF,
  OTA_MODE,
};

/**
 * @brief Configuration parameters for the FSM timing/behavior.
 *
 * Simple POD values; can be adjusted at runtime before calling `app_fsm_init()`.
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
  uint32_t backoff_min_ms = 2000;
  uint32_t backoff_max_ms = 60000;
  uint32_t internet_check_period_ms = 60000;  ///< periodic internet health check
  uint32_t debug_screen_period_ms = 1000; // 1s
};

/**
 * @brief Runtime state and metadata for the FSM.
 *
 * This structure holds all runtime counters, timestamps and flags used and modified by
 * `app_fsm_poll()` and helper functions.
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

  // Debug screen 1s polling
  uint32_t last_1s_tick_ms = 0;
  uint32_t last_debug_screen_ms = 0;

  // Display dimming
  uint32_t last_user_activity_ms = 0;
  uint32_t last_dim_step_ms = 0;
  uint8_t brightness_before_dim = 0;
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
