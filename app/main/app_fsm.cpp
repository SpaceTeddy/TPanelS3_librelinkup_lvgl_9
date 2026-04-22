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
#include "main.h"
#include "http_update.h"


//------------------------[uuid logger]-----------------------------------
/** @brief Module logger instance. */
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

// --- externs from your existing codebase (main.cpp) ---
extern SETTINGS settings;
extern MQTT mqtt;
extern PubSubClient mqtt_client;

extern bool ota_in_progress;
extern bool flag_debug_screen;

// Existing setup/actions you already have:
extern void setup_wifi();
extern bool setup_wg(bool enable, bool force_reinit);
extern bool wg_is_busy();
extern bool setup_mqtt();

// Existing processing pipeline you already have:
extern void update_glucose_data();
extern void update_five_minute_counter();
extern void update_mqtt_publish();

// Existing helper in your code:
extern void handle_internet_disconnection();

// Internet health check (implemented in main.cpp)
extern int app_check_internet_status(IPAddress ip, uint16_t port);
extern void app_recover_offline();

// Backlight PWM
extern void ledcWrite(uint8_t channel, uint32_t duty);

//------------------------[FSM implementation]-----------------------------------
static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

// Exponential backoff with jitter: base * 2^exp + random(0..1000)
static uint32_t compute_backoff_ms(const AppFsm &fsm)
{
    const uint32_t base = fsm.cfg.backoff_min_ms;
    const uint8_t exp = (fsm.consecutive_failures > 10) ? 10 : fsm.consecutive_failures;
    const uint32_t mul = (1u << exp);
    const uint64_t val = static_cast<uint64_t>(base) * static_cast<uint64_t>(mul);
    const uint32_t jitter = static_cast<uint32_t>(random(0, 1001));
    const uint32_t with_jitter = static_cast<uint32_t>(val) + jitter;
    return clamp_u32(with_jitter, fsm.cfg.backoff_min_ms, fsm.cfg.backoff_max_ms);
}

// State transition helper
// Helper to convert state enum to string for logging/debug UI
static const char* app_state_to_string(AppState s)
{
    switch (s)
    {
    case AppState::BOOT:            return "BOOT";
    case AppState::WIFI_CONNECT:    return "WIFI_CONNECT";
    case AppState::VPN_CHECK:       return "VPN_CHECK";
    case AppState::MQTT_CONNECT:    return "MQTT_CONNECT";
    case AppState::RUN_IDLE:        return "RUN_IDLE";
    case AppState::RUN_FETCH:       return "RUN_FETCH";
    case AppState::RUN_PUBLISH:     return "RUN_PUBLISH";
    case AppState::DISPLAY_DIM:     return "DISPLAY_DIM";
    case AppState::INTERNET_CHECK:  return "INTERNET_CHECK";
    case AppState::BACKOFF:         return "BACKOFF";
    case AppState::OTA_MODE:        return "OTA_MODE";
    default:                        return "UNKNOWN";
    }
}

// Helper to convert state enum to string for logging/debug UI
static const char* safe_reason(const char* r)
{
    return (r && r[0]) ? r : "-";
}

// Helper to compute remaining backoff time in seconds for logging/debug UI
static uint32_t backoff_remaining_s(const AppFsm &fsm)
{
    if (fsm.state != AppState::BACKOFF)
        return 0;

    const uint32_t now = millis();
    if (now >= fsm.backoff_until_ms)
        return 0;

    return (fsm.backoff_until_ms - now) / 1000U;
}

// State transition helper (logs old->new + reason + runtime + failures + backoff)
static void enter_state(AppFsm &fsm, AppState new_state, const char* reason = nullptr)
{
    if (fsm.state == new_state)
        return; // avoid duplicate log spam

    const AppState old_state = fsm.state;
    const uint32_t old_runtime_ms = millis() - fsm.last_state_change_ms;

    fsm.state = new_state;
    fsm.last_state_change_ms = millis();
    fsm.state_change_counter++;
    fsm.last_transition_reason = reason;

    logger.notice(
        "[FSM] %2u %-14s -> %2u %-14s | reason=%-15s | run=%6lums",
        (unsigned)old_state,
        app_state_to_string(old_state),
        (unsigned)new_state,
        app_state_to_string(new_state),
        safe_reason(reason),
        (unsigned long)old_runtime_ms
    );
}

// Helper functions for state actions and conditions
static bool wifi_ok()
{
    return WiFi.status() == WL_CONNECTED;
}

static bool mqtt_enabled()
{
    return (settings.config.mqtt_mode == 1) && (mqtt.mqtt_enable == 1);
}

static bool mqtt_ok()
{
    return mqtt_client.connected();
}

// Subscribe to MQTT topics based on the current configuration
static void mqtt_subscribe_topics()
{
    mqtt_client.subscribe((mqtt.mqtt_base + "/" + mqtt.mqtt_client_name + mqtt.mqtt_subscibe_toppic).c_str());
    if (!settings.config.mqtt_master_mode)
    {
        mqtt_client.subscribe((mqtt.mqtt_base + "/" + mqtt.mqtt_master_id + mqtt.mqtt_client_data).c_str());
    }
}

/**
 * @brief Ensures that the WiFi connection is established within the specified timeout.
 *
 * This function checks if the WiFi is already connected. If not, it attempts to connect
 * by calling `setup_wifi()`. It then waits for the connection to be established, checking
 * periodically until the timeout is reached.
 *
 * @param timeout_ms The maximum time to wait for a WiFi connection, in milliseconds.
 * @return `true` if the WiFi connection is established successfully, `false` otherwise.
 */
static bool ensure_wifi_connected(uint32_t timeout_ms)
{
    if (wifi_ok())
        return true;

    setup_wifi();

    const uint32_t start = millis();
    while (!wifi_ok() && (millis() - start) < timeout_ms)
    {
        delay(50);
    }

    if (!wifi_ok())
    {
        handle_internet_disconnection();
        return false;
    }
    return true;
}

/**
 * @brief Ensures that the WireGuard VPN connection is active.
 *
 * This function checks if the WireGuard VPN is enabled in the settings. If it is enabled,
 * it performs a connectivity check (using ESPPing if available) to verify that the VPN
 * connection is working. If the check fails, it attempts to re-enable WireGuard and checks again.
 *
 * @return `true` if the WireGuard VPN connection is active or not enabled, `false` if it is enabled but not working.
 */
static bool ensure_wireguard_ok()
{
    // settings.config.wg_mode is the *desired* state (user config).
    if (settings.config.wg_mode != 1)
        return true;

    // Test (as requested): ping our configured WG interface IP.
    // NOTE: this proves interface presence, not peer handshake.
    if (app_check_internet_status(IPAddress(192, 168, 0, 202), 1883) == true)
        return true;

    // If setup is already running, treat as "in progress" instead of failing the FSM.
    if (wg_is_busy())
        return true;

    // Force reinit. This function is responsible for being bounded in time.
    return setup_wg(true, true);
}

/**
 * @brief Ensures that the MQTT connection is established within the specified timeout.
 *
 * This function checks if MQTT is enabled and already connected. If not, it attempts to connect
 * to the MQTT broker using the configured credentials. It waits for the connection to be established,
 * checking periodically until the timeout is reached. If the connection is successful, it subscribes
 * to the necessary MQTT topics.
 *
 * @param timeout_ms The maximum time to wait for an MQTT connection, in milliseconds.
 * @return `true` if the MQTT connection is established successfully or MQTT is not enabled, `false` otherwise.
 */
static bool mqtt_connect_step(AppFsm &fsm, uint32_t timeout_ms)
{
    if (!mqtt_enabled())
        return true;

    if (mqtt_ok())
        return true;

    const uint32_t now = millis();

    if (fsm.mqtt_connect_started_ms == 0)
    {
        fsm.mqtt_connect_started_ms = now;
        fsm.last_mqtt_attempt_ms = 0;
    }

    // Give other tasks time; attempt at most once per second
    if (fsm.last_mqtt_attempt_ms != 0 && (now - fsm.last_mqtt_attempt_ms) < 1000)
        return false;

    // Timeout reached?
    if ((now - fsm.mqtt_connect_started_ms) >= timeout_ms)
        return false;

    fsm.last_mqtt_attempt_ms = now;

    // One connect attempt (setup_mqtt() must return quickly)
    return setup_mqtt();
}

// Exponential backoff with jitter: base * 2^exp + random(0..1000)
static bool should_fetch_master(const AppFsm &fsm)
{
    if (!settings.config.mqtt_master_mode)
        return false;
    return (millis() - fsm.last_fetch_ms) >= fsm.cfg.fetch_period_ms;
}

// Display dimming logic
static bool should_dim_display(const AppFsm &fsm)
{
    if (fsm.display_dim_active)
        return false;
    return (millis() - fsm.last_user_activity_ms) >= fsm.cfg.display_dim_timeout_ms;
}

// Display dimming step
static void display_dim_step(AppFsm &fsm)
{
    const uint32_t step_ms = (fsm.cfg.display_dim_step_ms == 0) ? 30u : fsm.cfg.display_dim_step_ms;
    const uint32_t now = millis();
    if (fsm.last_dim_step_ms != 0 && (now - fsm.last_dim_step_ms) < step_ms)
        return;
    fsm.last_dim_step_ms = now;
    if (settings.config.brightness == 0)
        return;
    settings.config.brightness--;
    ledcWrite(0, settings.config.brightness);
}

// WireGuard check logic
static bool should_wg_check(const AppFsm &fsm)
{
    if (settings.config.wg_mode != 1)
        return false;
    return (millis() - fsm.last_wg_check_ms) >= fsm.cfg.wg_check_period_ms;
}

// Internet health check logic
void app_fsm_init(AppFsm &fsm)
{
    fsm = AppFsm{};
    fsm.last_state_change_ms = millis();
    fsm.last_internet_check_ms = millis();
    fsm.last_1s_tick_ms = millis();
    fsm.last_user_activity_ms = millis();
    fsm.last_dim_step_ms = 0;
    fsm.brightness_before_dim = settings.config.brightness;
    fsm.display_dim_active = false;
    fsm.last_debug_screen_ms = millis();
    enter_state(fsm, AppState::BOOT, "OTA OFF");
}

// External triggers
void app_fsm_notify_mqtt_master_rx(AppFsm &fsm)
{
    fsm.mqtt_master_rx_pending = true;
}

// Call this on any user activity that should wake the display (e.g., touch, button press, etc.)
void app_fsm_notify_user_activity(AppFsm &fsm)
{
    fsm.last_user_activity_ms = millis();
    if (fsm.display_dim_active)
    {
        fsm.display_dim_active = false;
        fsm.last_dim_step_ms = 0;
        // Restore brightness
        uint8_t restore = fsm.brightness_before_dim;
        if (restore == 0)
            restore = 50;
        ledcWrite(0, restore);
        settings.config.brightness = restore;
        enter_state(fsm, AppState::RUN_IDLE, "USER ACTIVITY");
    }
}

// Main FSM polling function to be called regularly (e.g., from loop())
void app_fsm_poll(AppFsm &fsm)
{
    fw_update_poll();

    if (ota_in_progress)
    {
        enter_state(fsm, AppState::OTA_MODE, "OTA ON");
        return;
    }
    if (fsm.state == AppState::OTA_MODE && !ota_in_progress)
    {
        fsm.last_user_activity_ms = millis();
        fsm.last_dim_step_ms = 0;
        fsm.brightness_before_dim = settings.config.brightness;
        fsm.display_dim_active = false;
        enter_state(fsm, AppState::BOOT, "OTA OFF");
    }

    // Keep MQTT alive in the SAME context as publish/fetch to avoid multicore races.
    if (mqtt_enabled())
    {
        mqtt_client.loop();
    }

    // 1s tick hook (reserved)
    if ((millis() - fsm.last_1s_tick_ms) >= 1000)
    {
        fsm.last_1s_tick_ms = millis();
    }

    if (fsm.state == AppState::BACKOFF)
    {
        if (millis() < fsm.backoff_until_ms)
            return;
        enter_state(fsm, AppState::WIFI_CONNECT, "BACKOFF EXPIRED");
    }

    switch (fsm.state)
    {
    case AppState::BOOT:
    {
        enter_state(fsm, AppState::WIFI_CONNECT, "BOOT");
        break;
    }
    case AppState::WIFI_CONNECT:
    {
        if (!ensure_wifi_connected(fsm.cfg.wifi_connect_timeout_ms))
        {
            fsm.consecutive_failures++;
            fsm.last_backoff_ms = compute_backoff_ms(fsm);
            fsm.backoff_until_ms = millis() + fsm.last_backoff_ms;
            enter_state(fsm, AppState::BACKOFF, "WIFI FAIL");
            break;
        }
        enter_state(fsm, AppState::VPN_CHECK, "WIFI OK");
        break;
    }
    case AppState::VPN_CHECK:
    {
        // WG check cadence (optional). If WG is disabled, just continue.
        if (should_wg_check(fsm))
        {
            fsm.last_wg_check_ms = millis();
    
            if (!ensure_wireguard_ok())
            {
                // If WG setup is currently in progress, don't treat as failure.
                if (wg_is_busy())
                {
                    // Stay in VPN_CHECK and try again on next poll.
                    enter_state(fsm, AppState::VPN_CHECK, "WG BUSY");
                    break;
                }
    
                fsm.consecutive_failures++;
                fsm.last_backoff_ms = compute_backoff_ms(fsm);
                fsm.backoff_until_ms = millis() + fsm.last_backoff_ms;
                enter_state(fsm, AppState::BACKOFF, "WG FAIL");
                break;
            }
        }
        enter_state(fsm, AppState::MQTT_CONNECT, (settings.config.wg_mode == 1) ? "WG OK" : "WG OFF");
        break;
    }
    case AppState::MQTT_CONNECT:
    {
        if (!mqtt_enabled())
        {
            // MQTT disabled -> proceed
            enter_state(fsm, AppState::RUN_IDLE, "MQTT OFF");
            break;
        }
    
        if (mqtt_ok())
        {
            fsm.consecutive_failures = 0;
            fsm.mqtt_connect_started_ms = 0;
            fsm.last_mqtt_attempt_ms = 0;
            enter_state(fsm, AppState::RUN_IDLE, "MQTT OK");
            break;
        }
    
        // Step-wise connect attempts (bounded time, avoids long blocking loops)
        if (mqtt_connect_step(fsm, fsm.cfg.mqtt_connect_timeout_ms))
        {
            fsm.consecutive_failures = 0;
            fsm.mqtt_connect_started_ms = 0;
            fsm.last_mqtt_attempt_ms = 0;
            enter_state(fsm, AppState::RUN_IDLE, "MQTT OK");
            break;
        }
    
        // Still trying within timeout -> stay here without backoff.
        if (fsm.mqtt_connect_started_ms != 0 &&
            (millis() - fsm.mqtt_connect_started_ms) < fsm.cfg.mqtt_connect_timeout_ms)
        {
            break;
        }
    
        // Timed out -> backoff
        fsm.mqtt_connect_started_ms = 0;
        fsm.last_mqtt_attempt_ms = 0;
        fsm.consecutive_failures++;
        fsm.last_backoff_ms = compute_backoff_ms(fsm);
        fsm.backoff_until_ms = millis() + fsm.last_backoff_ms;
        enter_state(fsm, AppState::BACKOFF, "MQTT FAIL");
        break;
    }
    case AppState::RUN_IDLE:
    {
        // 1) Connectivity sanity
        if (!wifi_ok())
        {
            enter_state(fsm, AppState::WIFI_CONNECT, "WIFI LOST");
            break;
        }
    
        // 2) MQTT: immediate check (if disconnected) + periodic health check
        if (mqtt_enabled())
        {
            const uint32_t now = millis();
    
            if (!mqtt_ok())
            {
                enter_state(fsm, AppState::MQTT_CONNECT, "MQTT LOST");
                break;
            }
    
            if ((now - fsm.last_mqtt_check_ms) >= fsm.cfg.mqtt_check_period_ms)
            {
                fsm.last_mqtt_check_ms = now;
    
                // Lightweight periodic check: if loop() got stuck, connected() might still be true,
                // but this keeps the "MQTT check" visible and future-proof for added probes.
                //logger.debug("[mqtt] periodic check ok=1");
            }
        }
    
        // 3) Client mode trigger (incoming MASTER/raw message)
        if (!settings.config.mqtt_master_mode && fsm.mqtt_master_rx_pending)
        {
            fsm.mqtt_master_rx_pending = false;
            enter_state(fsm, AppState::RUN_FETCH, "CLIENT RX");
            break;
        }
    
        // 4) Master fetch trigger
        if (should_fetch_master(fsm))
        {
            enter_state(fsm, AppState::RUN_FETCH, "MASTER TICK");
            break;
        }
    
        // 5) Periodic WG check
        if (should_wg_check(fsm))
        {
            enter_state(fsm, AppState::VPN_CHECK, "WG PERIODIC");
            break;
        }

        // 6) Debug screen tick (every second)
        // Debug screen update (every 1s) — no FSM transition (avoids log spam)
        if (!fsm.display_dim_active &&
            (millis() - fsm.last_debug_screen_ms) >= fsm.cfg.debug_screen_period_ms)
        {
            fsm.last_debug_screen_ms = millis();
            flag_debug_screen = true; // Set flag so the main loop knows to refresh the debug screen
        }
    
        // 7) Periodic internet health check
        if ((millis() - fsm.last_internet_check_ms) >= fsm.cfg.internet_check_period_ms)
        {
            fsm.last_internet_check_ms = millis();
            enter_state(fsm, AppState::INTERNET_CHECK, "INET PERIODIC");
            break;
        }
    
        // 8) Display inactivity dim
        if (should_dim_display(fsm))
        {
            enter_state(fsm, AppState::DISPLAY_DIM, "INACTIVITY");
            break;
        }
    
        break;
    }
    case AppState::RUN_FETCH:
    {
        update_glucose_data();
        update_five_minute_counter();
    
        if (!fsm.fetch_schedule_override)
        {
            fsm.last_fetch_ms = millis();
        }
        else
        {
            // time-sync already adjusted last_fetch_ms to hit the target
            fsm.fetch_schedule_override = false;
        }
    
        enter_state(fsm, AppState::RUN_PUBLISH, "FETCH DONE");
        break;
    }
    case AppState::DISPLAY_DIM:
    {
        // Non-blocking fade to zero brightness. Stay in DISPLAY_DIM until we reach 0.
        if (!fsm.display_dim_active)
        {
            fsm.display_dim_active = true;
            fsm.brightness_before_dim = settings.config.brightness;
            fsm.last_dim_step_ms = 0;
            //logger.debug("DIM start: from=%u", (unsigned)fsm.brightness_before_dim);
        }
    
        display_dim_step(fsm);
    
        if (settings.config.brightness == 0)
        {
            //logger.debug("DIM done: bright=0");
            // Return to RUN_IDLE while staying dimmed until activity wakes it.
            enter_state(fsm, AppState::RUN_IDLE, "DIM DONE");
        }
        break;
    }
    case AppState::INTERNET_CHECK:
    {
        if (!wifi_ok())
        {
            enter_state(fsm, AppState::WIFI_CONNECT, "WIFI LOST");
            break;
        }
    
        const int internet_status = app_check_internet_status(IPAddress(192, 168, 0, 202), 1883);
        if (internet_status != 1)
        {
            app_recover_offline();
            enter_state(fsm, AppState::WIFI_CONNECT, "INET FAIL");
            break;
        }
    
        enter_state(fsm, AppState::RUN_IDLE, "INET OK");
        break;
    }
    case AppState::RUN_PUBLISH:
    {
        update_mqtt_publish();
        enter_state(fsm, AppState::RUN_IDLE, "PUBLISH DONE");
        break;
    }
    case AppState::OTA_MODE:
    {
        // Keep OTA running; exit handled at top of poll()
        break;
    }
    case AppState::BACKOFF:
        break;
    }
}
