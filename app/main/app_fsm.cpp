/**
 * @file app_fsm.cpp
 * @brief Implementation of the application state machine.
 *
 * All static helpers are internal to this translation unit.
 * The public API (app_fsm_init, app_fsm_poll, app_fsm_notify_*) is declared
 * in app_fsm.h and intended to be called from Arduino loop().
 */

#include "app_fsm.h"

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "settings.h"
#include "mqtt.h"
#include "main.h"
#include "http_update.h"
#include "ui_display.h"
#include "tpanels3.h"
#include "zigbee_h2.h"
#include <math.h>


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
extern bool wg_is_initialized();
extern bool setup_mqtt();

// Existing processing pipeline you already have:
extern void update_glucose_data();
extern void update_five_minute_counter();
extern void update_mqtt_publish();

// Existing helper in your code:
extern void handle_internet_disconnection();

// Internet health check (implemented in main.cpp)
extern int app_check_internet_status(IPAddress ip, uint16_t port);
extern int app_check_host_status(const char* host, uint16_t port);
extern void app_recover_offline();
extern uint8_t esp_status_counter_wifi_restart;
extern uint8_t esp_status_counter_wg_reinit;
extern void start_ap_mode();
extern bool g_force_ap_mode;

// Diagnostic breadcrumb for the main-loop hang detector (main.cpp).
extern volatile const char* g_loop_breadcrumb;

// Backlight PWM. Arduino core 3.x dropped the LEDC channel API (ledcSetup /
// ledcAttachPin), and its ledcWrite() now takes the *pin*, not the channel --
// the hand-rolled `extern void ledcWrite(uint8_t channel, uint32_t duty)` that
// used to sit here would have addressed GPIO 0 instead of the backlight. Go
// through the panel driver, which owns the pin, like the rest of the code does.
extern TPanelS3 tpanels3;

//------------------------[FSM implementation]-----------------------------------
/** @brief Clamp @p v to [@p lo, @p hi]. */
static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/**
 * @brief Compute the next exponential backoff duration with jitter.
 *
 * Formula: clamp(base * 2^failures + random(0..1000), min, max).
 * The exponent is capped at 10 to prevent overflow (max multiplier = 1024).
 *
 * @param fsm FSM instance whose consecutive_failures and cfg are read.
 * @return Backoff duration in milliseconds.
 */
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

/**
 * @brief Return a human-readable name for an AppState value.
 * @param s State to convert.
 * @return Null-terminated string literal (e.g. "RUN_IDLE"). Never nullptr.
 */
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
    case AppState::DISPLAY_UNDIM:   return "DISPLAY_UNDIM";
    case AppState::INTERNET_CHECK:  return "INTERNET_CHECK";
    case AppState::BACKOFF:         return "BACKOFF";
    case AppState::OTA_MODE:        return "OTA_MODE";
    case AppState::FW_CHECKING:     return "FW_CHECKING";
    case AppState::FW_INSTALLING:   return "FW_INSTALLING";
    default:                        return "UNKNOWN";
    }
}

const char* app_fsm_state_name(AppState s)
{
    return app_state_to_string(s);
}

/** @brief Return @p r if non-empty, otherwise "-". Safe to pass nullptr. */
static const char* safe_reason(const char* r)
{
    return (r && r[0]) ? r : "-";
}

/**
 * @brief Return the remaining backoff time in whole seconds.
 * @param fsm FSM instance to inspect.
 * @return Seconds until backoff expires, or 0 if not in BACKOFF state.
 */
static uint32_t backoff_remaining_s(const AppFsm &fsm)
{
    if (fsm.state != AppState::BACKOFF)
        return 0;

    const uint32_t now = millis();
    if (now >= fsm.backoff_until_ms)
        return 0;

    return (fsm.backoff_until_ms - now) / 1000U;
}

/**
 * @brief Transition the FSM to @p new_state and log the change.
 *
 * No-op if @p new_state equals the current state (prevents log spam).
 * Records the transition reason, increments the state counter, and resets
 * the state-entry timestamp.
 *
 * @param fsm       FSM instance to update.
 * @param new_state Target state.
 * @param reason    Short human-readable reason string for the log (may be nullptr).
 */
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

/** @brief Return true if WiFi is currently associated (WL_CONNECTED). */
static bool wifi_ok()
{
    return WiFi.status() == WL_CONNECTED;
}

/** @brief Return true if MQTT is configured and enabled in settings. */
static bool mqtt_enabled()
{
    return (settings.config.mqtt_mode == 1) && (mqtt.mqtt_enable == 1);
}

/** @brief Return true if the PubSubClient is currently connected to the broker. */
static bool mqtt_ok()
{
    return mqtt_client.connected();
}

/**
 * @brief Subscribe to all MQTT topics required by the current operating mode.
 *
 * Always subscribes to the device's own command topic. In client mode
 * (mqtt_master_mode == 0) also subscribes to the master's data topic.
 */
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

    // AP mode is active — don't tear it down by calling setup_wifi().
    // The user must reconfigure via 192.168.4.1 and the device will reboot.
    if (g_force_ap_mode)
        return false;

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
 * @brief Checks WireGuard tunnel health and manages the sick timer.
 *
 * Two-level check:
 *  1. If the lwIP netif is gone (wg_is_initialized() == false) → immediate reinit via setup_wg().
 *  2. TCP probe to the MQTT broker (which sits behind the WG peer) to confirm end-to-end routing.
 *     On probe failure we do NOT reinit — reinit blocks the main loop for seconds and can
 *     destabilise WiFi. Instead we track how long the tunnel has been "sick" and call
 *     esp_restart() if it exceeds wg_reboot_timeout_ms (default 5 min).
 *
 * @return true  if the WG interface is up (even if broker is temporarily unreachable).
 * @return false if WG is enabled but the interface could not be brought up.
 */
static bool ensure_wireguard_ok(AppFsm &fsm)
{
    if (settings.config.wg_mode != 1)
        return true;

    if (wg_is_busy())
        return true;

    // Step 1: if the lwIP netif is gone, reinit immediately.
    bool wg_up = wg_is_initialized();
    if (!wg_up)
    {
        logger.notice("[VPN] interface gone — reinit");
        wg_up = setup_wg(true, true);
    }

    // Step 2: probe the MQTT broker through the tunnel.
    // is_initialized() only means begin() was called; it does NOT confirm the
    // WireGuard handshake completed or that packets flow. A TCP connect to the
    // broker (which sits behind the WG peer) gives a real end-to-end signal.
    // On failure we do NOT reinit — reinit blocks the main loop for seconds and
    // can destabilise WiFi, causing an AP-mode lockup. Instead we track how
    // long the tunnel has been "sick" and reboot if it exceeds the threshold.
    bool broker_up = false;
    if (wg_up)
    {
        broker_up = (app_check_host_status(
            settings.config.mqttServer.c_str(), settings.config.mqtt_port) == 1);
    }

    const bool healthy = wg_up && broker_up;

    if (!healthy)
    {
        if (fsm.wg_sick_since_ms == 0)
            fsm.wg_sick_since_ms = millis();

        const uint32_t sick_ms = millis() - fsm.wg_sick_since_ms;
        logger.notice("[VPN] sick: wg_up=%d broker_up=%d duration=%lus / %lus",
                      (int)wg_up, (int)broker_up,
                      (unsigned long)(sick_ms / 1000),
                      (unsigned long)(fsm.cfg.wg_reboot_timeout_ms / 1000));

        if (sick_ms >= fsm.cfg.wg_reboot_timeout_ms)
        {
            logger.notice("[VPN] tunnel sick for %lu min — rebooting",
                          (unsigned long)(sick_ms / 60000));
            delay(200); // flush serial
            ui_blank_screen_for_reset();
            esp_restart();
        }
    }
    else
    {
        if (fsm.wg_sick_since_ms != 0)
        {
            logger.notice("[VPN] tunnel healthy again after %lus sick",
                          (unsigned long)((millis() - fsm.wg_sick_since_ms) / 1000));
            fsm.wg_sick_since_ms = 0;
        }
        logger.debug("[VPN] healthy: wg_up=%d broker_up=%d sick=0s/%lus wifi_reconnects=%u wg_reinits=%u",
                     (int)wg_up, (int)broker_up,
                     (unsigned long)(fsm.cfg.wg_reboot_timeout_ms / 1000),
                     (unsigned)esp_status_counter_wifi_restart,
                     (unsigned)esp_status_counter_wg_reinit);
    }

    // Return wg_up so VPN_CHECK can decide whether to reinit/backoff.
    // Even if broker is temporarily unreachable, the FSM continues to
    // MQTT_CONNECT which handles broker reconnect independently.
    return wg_up;
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

/**
 * @brief Return true if a master-mode glucose fetch is due.
 *
 * Only relevant when mqtt_master_mode is active; checks whether
 * fetch_period_ms has elapsed since the last successful fetch.
 *
 * @param fsm FSM instance to inspect.
 */
static bool should_fetch_master(const AppFsm &fsm)
{
    if (!settings.config.mqtt_master_mode)
        return false;
    return (millis() - fsm.last_fetch_ms) >= fsm.cfg.fetch_period_ms;
}

/**
 * @brief Return true if the display should start dimming due to inactivity.
 *
 * False if dimming is already active or the inactivity timeout has not elapsed.
 *
 * @param fsm FSM instance to inspect.
 */
static bool should_dim_display(const AppFsm &fsm)
{
    if (fsm.display_dim_active)
        return false;
    if (settings.config.display_dim_timeout_s == 0)
        return false; // disabled
    return (millis() - fsm.last_user_activity_ms) >= (uint32_t)settings.config.display_dim_timeout_s * 1000UL;
}

// Ambient-light auto-brightness (settings.config.auto_brightness). Maps the
// freshest lux reading from any paired Zigbee illuminance sensor onto a
// brightness target, log-scaled since perceived brightness (and the lux
// values sensors report) are roughly logarithmic, not linear.
static const uint16_t AMBIENT_LUX_MIN = 1;      // at or below: minimum brightness
static const uint16_t AMBIENT_LUX_MAX = 1000;   // at or above: maximum brightness
static const uint8_t  AMBIENT_BRI_MIN = 20;     // never below this -- stays legible in the dark
static const uint8_t  AMBIENT_BRI_MAX = 255;
static const uint32_t AMBIENT_STEP_PERIOD_MS = 1000;
static const uint8_t  AMBIENT_STEP_MAX_DELTA = 4; // brightness units per period -- glides, does not jump

static uint8_t ambient_lux_to_brightness(uint16_t lux)
{
    if (lux <= AMBIENT_LUX_MIN) return AMBIENT_BRI_MIN;
    if (lux >= AMBIENT_LUX_MAX) return AMBIENT_BRI_MAX;
    float t = log10f((float)lux / AMBIENT_LUX_MIN) / log10f((float)AMBIENT_LUX_MAX / AMBIENT_LUX_MIN);
    return (uint8_t)(AMBIENT_BRI_MIN + t * (AMBIENT_BRI_MAX - AMBIENT_BRI_MIN));
}

/**
 * @brief Glides settings.config.brightness toward the ambient-lux target.
 *
 * No-op unless auto_brightness is enabled, a sensor currently reports lux,
 * and the display is awake -- this never fights display_dim_step()/
 * display_undim_step() during an actual dim/undim transition, and inactivity
 * dimming still runs on its own schedule regardless of ambient light.
 *
 * @param fsm FSM instance; brightness and the ambient step timestamp are
 *            updated in-place.
 */
static void display_ambient_step(AppFsm &fsm)
{
    if (!settings.config.auto_brightness) return;
    if (fsm.display_dim_active) return;

    const uint32_t now = millis();
    if (fsm.last_ambient_step_ms != 0 && (now - fsm.last_ambient_step_ms) < AMBIENT_STEP_PERIOD_MS)
        return;
    fsm.last_ambient_step_ms = now;

    uint16_t lux;
    if (!zigbee_h2_ambient_lux(lux)) return;

    const uint8_t target = ambient_lux_to_brightness(lux);
    const int16_t diff = (int16_t)target - (int16_t)settings.config.brightness;
    if (diff == 0) return;

    const int16_t step = (diff > 0)
        ? ((diff < AMBIENT_STEP_MAX_DELTA) ? diff : (int16_t)AMBIENT_STEP_MAX_DELTA)
        : ((diff > -AMBIENT_STEP_MAX_DELTA) ? diff : -(int16_t)AMBIENT_STEP_MAX_DELTA);

    settings.config.brightness = (uint8_t)((int16_t)settings.config.brightness + step);
    tpanels3.set_backlight_brightness(settings.config.brightness);
    // Keeps a later real DISPLAY_DIM/UNDIM cycle consistent with the
    // ambient-adjusted level instead of fading back to a stale one.
    fsm.brightness_before_dim = settings.config.brightness;
}

/**
 * @brief Decrement backlight brightness by one step if the step interval has elapsed.
 *
 * Rate-limited by cfg.display_dim_step_ms (default 30 ms). Stops at 0.
 * Writes through TPanelS3::set_backlight_brightness().
 *
 * @param fsm FSM instance; brightness and step timestamp are updated in-place.
 */
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
    tpanels3.set_backlight_brightness(settings.config.brightness);
}

/**
 * @brief Increment backlight brightness by one step toward display_undim_target.
 *
 * Same rate-limiting as display_dim_step (cfg.display_dim_step_ms). Stops at target.
 *
 * @param fsm FSM instance; brightness and step timestamp are updated in-place.
 */
static void display_undim_step(AppFsm &fsm)
{
    const uint32_t step_ms = (fsm.cfg.display_dim_step_ms == 0) ? 30u : fsm.cfg.display_dim_step_ms;
    const uint32_t now = millis();
    if (fsm.last_dim_step_ms != 0 && (now - fsm.last_dim_step_ms) < step_ms)
        return;
    fsm.last_dim_step_ms = now;
    if (settings.config.brightness >= fsm.display_undim_target)
        return;
    settings.config.brightness++;
    tpanels3.set_backlight_brightness(settings.config.brightness);
}

/**
 * @brief Return true if a periodic WireGuard health check is due.
 *
 * Always false when wg_mode != 1 (WireGuard disabled).
 *
 * @param fsm FSM instance to inspect.
 */
static bool should_wg_check(const AppFsm &fsm)
{
    if (settings.config.wg_mode != 1)
        return false;
    return (millis() - fsm.last_wg_check_ms) >= fsm.cfg.wg_check_period_ms;
}

/**
 * @brief Reset the FSM to its initial state and start from BOOT.
 *
 * Zeroes all counters and timestamps, restores brightness tracking, and
 * immediately transitions to AppState::BOOT. Safe to call more than once
 * (e.g. after an OTA update completes).
 *
 * @param fsm FSM instance to initialise.
 */
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

/**
 * @brief Signal that a master MQTT data payload has arrived.
 *
 * Sets the mqtt_master_rx_pending flag which causes RUN_IDLE to transition
 * to RUN_FETCH on the next poll cycle (client mode only).
 * Safe to call from the PubSubClient callback context.
 *
 * @param fsm FSM instance to notify.
 */
void app_fsm_notify_mqtt_master_rx(AppFsm &fsm)
{
    fsm.mqtt_master_rx_pending = true;
}

/**
 * @brief Notify the FSM of user activity (touch, button, etc.).
 *
 * Resets the inactivity timer. If the display is currently dimmed, restores
 * brightness to its pre-dim value (minimum 50) and transitions back to
 * RUN_IDLE.
 *
 * @param fsm FSM instance to notify.
 */
void app_fsm_notify_user_activity(AppFsm &fsm)
{
    fsm.last_user_activity_ms = millis();
    const bool brightness_is_off = (settings.config.brightness == 0);
    if (fsm.display_dim_active || brightness_is_off)
    {
        fsm.display_dim_active = false;
        fsm.last_dim_step_ms = 0;
        uint8_t target = fsm.brightness_before_dim;
        if (target == 0)
            target = 50;
        fsm.display_undim_target = target;
        enter_state(fsm, AppState::DISPLAY_UNDIM, "USER ACTIVITY");
    }
}

/**
 * @brief Main FSM tick — call once per Arduino loop() iteration.
 *
 * Execution order per tick:
 *  1. Firmware update check (fw_update_op_pending / fw_update_poll).
 *  2. ElegantOTA guard — park in OTA_MODE while ota_in_progress is set.
 *  3. MQTT keepalive (mqtt_client.loop()).
 *  4. 1-second housekeeping tick.
 *  5. BACKOFF expiry check.
 *  6. Main state switch (BOOT → WIFI_CONNECT → … → RUN_IDLE → …).
 *
 * @param fsm FSM instance to advance.
 */
void app_fsm_poll(AppFsm &fsm)
{
    const int fw_pending = fw_update_op_pending();
    const AppState pre_fw_state = fsm.state;

    if      (fw_pending == 1) enter_state(fsm, AppState::FW_CHECKING,   "FW CHECK");
    else if (fw_pending == 2) enter_state(fsm, AppState::FW_INSTALLING, "FW INSTALL");

    fw_update_poll();

    if (fw_pending > 0) {
        enter_state(fsm, pre_fw_state, fw_update_get_status());
        ui_update_fw_hint();
    }

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
            fsm.wifi_fail_count++;
            if (fsm.wifi_fail_count >= fsm.cfg.wifi_ap_threshold)
            {
                logger.warning("[WiFi] %u consecutive failures — AP fallback", fsm.wifi_fail_count);
                start_ap_mode();
            }
            fsm.last_backoff_ms = compute_backoff_ms(fsm);
            fsm.backoff_until_ms = millis() + fsm.last_backoff_ms;
            enter_state(fsm, AppState::BACKOFF, "WIFI FAIL");
            break;
        }
        fsm.wifi_fail_count = 0;
        fsm.consecutive_failures = 0;
        enter_state(fsm, AppState::VPN_CHECK, "WIFI OK");
        break;
    }
    case AppState::VPN_CHECK:
    {
        // WG check cadence (optional). If WG is disabled, just continue.
        if (should_wg_check(fsm))
        {
            fsm.last_wg_check_ms = millis();

            g_loop_breadcrumb = "fsm.vpn.ensure_wg_ok";
            if (!ensure_wireguard_ok(fsm))
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
        g_loop_breadcrumb = "idle";
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
        g_loop_breadcrumb = "fsm.mqtt.connect_step";
        if (mqtt_connect_step(fsm, fsm.cfg.mqtt_connect_timeout_ms))
        {
            g_loop_breadcrumb = "idle";
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
            g_loop_breadcrumb = "idle";
            break;
        }

        // Timed out -> backoff
        g_loop_breadcrumb = "idle";
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

        // 9) Ambient-light auto-brightness (no state transition -- glides
        // settings.config.brightness in place while the display is awake).
        display_ambient_step(fsm);

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
            logger.debug("[DIM] start: from=%u", (unsigned)fsm.brightness_before_dim);
        }
    
        display_dim_step(fsm);
    
        if (settings.config.brightness == 0)
        {
            logger.debug("[DIM] done: bright=0");
            // Return to RUN_IDLE while staying dimmed until activity wakes it.
            enter_state(fsm, AppState::RUN_IDLE, "DIM DONE");
        }
        break;
    }
    case AppState::DISPLAY_UNDIM:
    {
        static bool undim_start_logged = false;
        if (!undim_start_logged)
        {
            logger.debug("[UNDIM] start: from=%u target=%u",
                         (unsigned)settings.config.brightness,
                         (unsigned)fsm.display_undim_target);
            undim_start_logged = true;
        }

        display_undim_step(fsm);

        if (settings.config.brightness >= fsm.display_undim_target)
        {
            // The value that matters: what the code believes it just wrote to
            // the backlight PWM at the moment it calls the ramp finished. If
            // the panel stays dark despite this reading a normal number, the
            // break is downstream of here -- between this write and the
            // physical output -- not in this state machine's own bookkeeping.
            logger.debug("[UNDIM] done: settled=%u (target was %u)",
                         (unsigned)settings.config.brightness,
                         (unsigned)fsm.display_undim_target);
            undim_start_logged = false;
            enter_state(fsm, AppState::RUN_IDLE, "UNDIM DONE");
        }
        break;
    }
    case AppState::INTERNET_CHECK:
    {
        if (!wifi_ok())
        {
            fsm.inet_fail_count = 0;
            enter_state(fsm, AppState::WIFI_CONNECT, "WIFI LOST");
            break;
        }

        if (settings.config.wg_mode == 1)
        {
            // VPN_CHECK already handles tunnel health via ensure_wireguard_ok().
            fsm.inet_fail_count = 0;
            enter_state(fsm, AppState::RUN_IDLE, "INET OK (WG)");
            break;
        }

        // WG disabled: verify public internet reachability.
        // Tolerate inet_fail_max consecutive failures (covers ~3 min router
        // IP-change reconnect) before forcing a fresh WiFi association.
        // AP mode is only triggered by setup_wifi() itself when no known SSID
        // is reachable — not by a transient internet outage.
        {
            const int inet_status = app_check_internet_status(IPAddress(1, 1, 1, 1), 443);
            if (inet_status != 1)
            {
                fsm.inet_fail_count++;
                logger.warning("INET check failed (%u/%u)", fsm.inet_fail_count, fsm.cfg.inet_fail_max);
                if (fsm.inet_fail_count >= fsm.cfg.inet_fail_max)
                {
                    // Force a full WiFi reconnect so setup_wifi() runs.
                    // If the SSID is unreachable setup_wifi() will fall back to
                    // AP mode on its own; no need to call app_recover_offline() here.
                    fsm.inet_fail_count = 0;
                    WiFi.disconnect();
                    enter_state(fsm, AppState::WIFI_CONNECT, "INET FAIL");
                }
                else
                {
                    // Transient failure — return to RUN_IDLE and retry next period
                    enter_state(fsm, AppState::RUN_IDLE, "INET RETRY");
                }
                break;
            }
            fsm.inet_fail_count = 0;
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
