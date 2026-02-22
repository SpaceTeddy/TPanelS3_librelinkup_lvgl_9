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

#include <memory>         ///< Smart pointers
#include <string>         ///< String operations
#include <vector>         ///< Vector container
#include <uuid/common.h>  ///< UUID common utilities
#include <uuid/console.h> ///< Console interaction
#include <uuid/telnet.h>  ///< Telnet server
#include <uuid/log.h>     ///< Logging system

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

struct AppFsmConfig
{
  uint32_t display_dim_timeout_ms = 30000; // 300000;   ///< 5 min
  uint32_t display_dim_step_ms = 30;       ///< fade step interval
  uint32_t wifi_connect_timeout_ms = 20000;
  uint32_t mqtt_connect_timeout_ms = 8000;
  uint32_t fetch_period_ms = 60000;    ///< Master mode cadence
  uint32_t wg_check_period_ms = 60000; ///< Optional WireGuard/ping cadence
  uint32_t backoff_min_ms = 2000;
  uint32_t backoff_max_ms = 60000;
  uint32_t internet_check_period_ms = 60000; ///< periodic internet health check
};

struct AppFsm
{
  AppState state = AppState::BOOT;

  uint32_t last_state_change_ms = 0;
  uint32_t last_1s_tick_ms = 0;
  uint32_t last_fetch_ms = 0;
  uint32_t last_wg_check_ms = 0;
  uint32_t backoff_until_ms = 0;

  uint32_t last_internet_check_ms = 0;

  // Display dimming
  uint32_t last_user_activity_ms = 0;
  uint32_t last_dim_step_ms = 0;
  uint8_t brightness_before_dim = 0;
  bool display_dim_active = false;

  uint8_t consecutive_failures = 0;

  volatile bool mqtt_master_rx_pending = false;

  AppFsmConfig cfg;
};

void app_fsm_init(AppFsm &fsm);
void app_fsm_poll(AppFsm &fsm);

/**
 * @brief Notify the FSM that a master MQTT data payload arrived (client mode).
 *
 * Call this from your existing mqtt_callback() at the place where you currently set
 * `flag_mqtt_master_rx = true;`.
 */
void app_fsm_notify_mqtt_master_rx(AppFsm &fsm);
void app_fsm_notify_user_activity(AppFsm &fsm);
