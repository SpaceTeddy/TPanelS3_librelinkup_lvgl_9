/**
 * @file app_fsm.cpp
 * @brief Implementation of the application state machine.
 */

#include "app_fsm.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

#include "settings.h"
#include "mqtt.h"

#if __has_include(<ESPPing.h>)
#include <ESPPing.h>
#endif

// --- externs from your existing codebase (main.cpp) ---
extern SETTINGS settings;

extern MQTT mqtt;
extern PubSubClient mqtt_client;

extern bool ota_in_progress;

extern const IPAddress ping_ip;

// Existing setup/actions you already have:
extern void setup_wifi();
extern void setup_wg(bool enable);
extern bool setup_mqtt();

// Existing processing pipeline you already have:
extern void update_glucose_data();
extern void update_five_minute_counter();
extern void update_mqtt_publish();

// Existing helper in your code:
extern void handle_internet_disconnection();

// 60s cloud-aligned cadence (master mode)
extern uint64_t g_timer_60000ms_backup;
extern void synchronize_time_offset_epoch();


static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static uint32_t compute_backoff_ms(const AppFsm& fsm) {
  const uint32_t base = fsm.cfg.backoff_min_ms;
  const uint8_t exp = (fsm.consecutive_failures > 10) ? 10 : fsm.consecutive_failures;
  const uint32_t mul = (1u << exp);
  const uint64_t val = static_cast<uint64_t>(base) * static_cast<uint64_t>(mul);
  return clamp_u32(static_cast<uint32_t>(val), fsm.cfg.backoff_min_ms, fsm.cfg.backoff_max_ms);
}

static void enter_state(AppFsm& fsm, AppState s) {
  fsm.state = s;
  fsm.last_state_change_ms = millis();
}

static bool wifi_ok() {
  return WiFi.status() == WL_CONNECTED;
}

static bool mqtt_enabled() {
  return (settings.config.mqtt_mode == 1) && (mqtt.mqtt_enable == 1);
}

static bool mqtt_ok() {
  return mqtt_client.connected();
}

static void mqtt_subscribe_topics() {
  mqtt_client.subscribe((mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic).c_str());
  if (!settings.config.mqtt_master_mode) {
    mqtt_client.subscribe((mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data).c_str());
  }
}

static bool ensure_wifi_connected(uint32_t timeout_ms) {
  if (wifi_ok()) return true;

  setup_wifi();

  const uint32_t start = millis();
  while (!wifi_ok() && (millis() - start) < timeout_ms) {
    delay(50);
  }

  if (!wifi_ok()) {
    handle_internet_disconnection();
    return false;
  }
  return true;
}

static bool ensure_wireguard_ok() {
  if (settings.config.wg_mode != 1) return true;

#if __has_include(<ESPPing.h>)
  if (Ping.ping(ping_ip)) return true;
  setup_wg(true);
  delay(50);
  return Ping.ping(ping_ip);
#else
  // If ESPPing isn't available, just try to (re)enable WireGuard.
  setup_wg(true);
  delay(50);
  return true;
#endif
}

static bool ensure_mqtt_connected(uint32_t timeout_ms) {
  if (!mqtt_enabled()) return true;
  if (mqtt_ok()) return true;

  const uint32_t start = millis();
  while (!mqtt_ok() && (millis() - start) < timeout_ms) {
    mqtt_client.connect(
      (mqtt.mqtt_base + "/" + mqtt.mqtt_client_name).c_str(),
      mqtt.mqtt_user,
      mqtt.mqtt_password
    );
    mqtt_client.loop();
    delay(10);
  }

  if (mqtt_ok()) {
    mqtt_subscribe_topics();
    return true;
  }
  return false;
}

static bool should_fetch_master(const AppFsm& fsm) {
  if (!settings.config.mqtt_master_mode) return false;
  // Cloud-aligned cadence: synchronize_time_offset_epoch() adjusts g_timer_60000ms_backup.
  return (millis() - (uint32_t)g_timer_60000ms_backup) > (uint32_t)60000;
}


static bool should_wg_check(const AppFsm& fsm) {
  if (settings.config.wg_mode != 1) return false;
  return (millis() - fsm.last_wg_check_ms) >= fsm.cfg.wg_check_period_ms;
}

void app_fsm_init(AppFsm& fsm) {
  fsm = AppFsm{};
  // Ensure first master fetch happens immediately; later cycles are cloud-aligned.
  if (settings.config.mqtt_master_mode) {
    g_timer_60000ms_backup = millis() - 60000;
  }

  fsm.last_state_change_ms = millis();
  fsm.last_1s_tick_ms = millis();
  enter_state(fsm, AppState::BOOT);
}

void app_fsm_notify_mqtt_master_rx(AppFsm& fsm) {
  fsm.mqtt_master_rx_pending = true;
}

void app_fsm_poll(AppFsm& fsm) {
  if (ota_in_progress) {
    enter_state(fsm, AppState::OTA_MODE);
    return;
  }
  if (fsm.state == AppState::OTA_MODE && !ota_in_progress) {
    enter_state(fsm, AppState::BOOT);
  }

  // Keep MQTT alive in the SAME context as publish/fetch to avoid multicore races.
  if (mqtt_enabled()) {
    mqtt_client.loop();
  }

  // 1s tick hook (reserved)
  if ((millis() - fsm.last_1s_tick_ms) >= 1000) {
    fsm.last_1s_tick_ms = millis();
  }

  if (fsm.state == AppState::BACKOFF) {
    if (millis() < fsm.backoff_until_ms) return;
    enter_state(fsm, AppState::WIFI_CONNECT);
  }

  switch (fsm.state) {
    case AppState::BOOT: {
      enter_state(fsm, AppState::WIFI_CONNECT);
      break;
    }

    case AppState::WIFI_CONNECT: {
      if (!ensure_wifi_connected(fsm.cfg.wifi_connect_timeout_ms)) {
        fsm.consecutive_failures++;
        fsm.backoff_until_ms = millis() + compute_backoff_ms(fsm);
        enter_state(fsm, AppState::BACKOFF);
        break;
      }
      enter_state(fsm, AppState::VPN_CHECK);
      break;
    }

    case AppState::VPN_CHECK: {
      if (should_wg_check(fsm)) {
        fsm.last_wg_check_ms = millis();
        if (!ensure_wireguard_ok()) {
          fsm.consecutive_failures++;
          fsm.backoff_until_ms = millis() + compute_backoff_ms(fsm);
          enter_state(fsm, AppState::BACKOFF);
          break;
        }
      }
      enter_state(fsm, AppState::MQTT_CONNECT);
      break;
    }

    case AppState::MQTT_CONNECT: {
      if (!ensure_mqtt_connected(fsm.cfg.mqtt_connect_timeout_ms)) {
        fsm.consecutive_failures++;
        fsm.backoff_until_ms = millis() + compute_backoff_ms(fsm);
        enter_state(fsm, AppState::BACKOFF);
        break;
      }
      fsm.consecutive_failures = 0;
      enter_state(fsm, AppState::RUN_IDLE);
      break;
    }

    case AppState::RUN_IDLE: {
      if (!wifi_ok()) {
        enter_state(fsm, AppState::WIFI_CONNECT);
        break;
      }
      if (mqtt_enabled() && !mqtt_ok()) {
        enter_state(fsm, AppState::MQTT_CONNECT);
        break;
      }

      if (!settings.config.mqtt_master_mode && fsm.mqtt_master_rx_pending) {
        fsm.mqtt_master_rx_pending = false;
        enter_state(fsm, AppState::RUN_FETCH);
        break;
      }

      if (should_fetch_master(fsm)) {
        enter_state(fsm, AppState::RUN_FETCH);
        break;
      }

      if (should_wg_check(fsm)) {
        enter_state(fsm, AppState::VPN_CHECK);
        break;
      }
      break;
    }

    case AppState::RUN_FETCH: {
      update_glucose_data();
      update_five_minute_counter();
fsm.last_fetch_ms = millis();
      enter_state(fsm, AppState::RUN_PUBLISH);
      break;
    }

    case AppState::RUN_PUBLISH: {
      update_mqtt_publish();
      enter_state(fsm, AppState::RUN_IDLE);
      break;
    }

    case AppState::OTA_MODE:
    case AppState::BACKOFF:
      break;
  }
}
