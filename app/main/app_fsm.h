/**
 * @file app_fsm.h
 * @brief Application state machine (single-threaded: call from Arduino loop()).
 *
 * Rules:
 * - Call app_fsm_poll() frequently from `loop()` (UI thread).
 * - Do NOT call mqtt_client.loop()/publish from other tasks/cores in parallel.
 * - LVGL/UI updates remain in `loop()` (single writer).
 */

#pragma once

#include <Arduino.h>

enum class AppState : uint8_t {
  BOOT = 0,
  WIFI_CONNECT,
  VPN_CHECK,
  MQTT_CONNECT,
  RUN_IDLE,
  RUN_FETCH,
  RUN_PUBLISH,
  BACKOFF,
  OTA_MODE,
};

struct AppFsmConfig {
  uint32_t wifi_connect_timeout_ms = 20000;
  uint32_t mqtt_connect_timeout_ms = 8000;
  uint32_t fetch_period_ms = 60000;      ///< Master mode cadence
  uint32_t wg_check_period_ms = 60000;   ///< Optional WireGuard/ping cadence
  uint32_t backoff_min_ms = 2000;
  uint32_t backoff_max_ms = 60000;
};

struct AppFsm {
  AppState state = AppState::BOOT;

  uint32_t last_state_change_ms = 0;
  uint32_t last_1s_tick_ms = 0;
  uint32_t last_fetch_ms = 0;
  uint32_t last_wg_check_ms = 0;
  uint32_t backoff_until_ms = 0;

  uint8_t consecutive_failures = 0;

  volatile bool mqtt_master_rx_pending = false;

  AppFsmConfig cfg;
};

void app_fsm_init(AppFsm& fsm);
void app_fsm_poll(AppFsm& fsm);

/**
 * @brief Notify the FSM that a master MQTT data payload arrived (client mode).
 *
 * Call this from mqtt_callback() where you previously set `flag_mqtt_master_rx = true;`.
 */
void app_fsm_notify_mqtt_master_rx(AppFsm& fsm);
