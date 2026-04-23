#include "http_update.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>

#include <uuid/log.h>
#include "ui.h"

extern bool ota_in_progress;
extern uint8_t update_ota_progress_screen(int progress);

#ifndef APP_FIRMWARE_VERSION
#define APP_FIRMWARE_VERSION "0.0.0-dev"
#endif
#ifndef FW_UPDATE_MANIFEST_URL
#define FW_UPDATE_MANIFEST_URL ""
#endif
#ifndef FW_UPDATE_CHECK_INTERVAL_MS
#define FW_UPDATE_CHECK_INTERVAL_MS 3600000UL
#endif

// Root CA bundle embedded from data/cert/x509_crt_bundle.bin (136 Mozilla root CAs)
extern const uint8_t x509_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

struct FirmwareUpdateState {
    bool check_requested = true;
    bool install_requested = false;
    bool checking = false;
    bool installing = false;
    bool update_available = false;
    uint32_t last_check_ms = 0;
    String latest_version = "";
    String binary_url = "";
    String last_error = "";
    String status = "idle";
};

static FirmwareUpdateState g_fw_update;
static SemaphoreHandle_t g_fw_update_mutex = nullptr;
static uint32_t g_fw_progress_last_log_ms = 0;
static int g_fw_progress_last_percent = -1;
static uint32_t g_fw_poll_last_ms = 0;

static bool fw_update_lock(TickType_t wait = pdMS_TO_TICKS(1000)) {
    return (g_fw_update_mutex != nullptr) && (xSemaphoreTake(g_fw_update_mutex, wait) == pdTRUE);
}

static void fw_update_unlock() {
    if (g_fw_update_mutex != nullptr) xSemaphoreGive(g_fw_update_mutex);
}

static int fw_parse_version_part(const String& v, int& idx) {
    const int n = v.length();
    while (idx < n && !isDigit(v[idx])) idx++;
    if (idx >= n) return -1;
    int out = 0;
    while (idx < n && isDigit(v[idx])) {
        out = out * 10 + (v[idx] - '0');
        idx++;
    }
    return out;
}

static int fw_compare_versions(const String& a, const String& b) {
    // Separate numeric version from pre-release suffix (e.g. "1.0.0-rc1" → "1.0.0" + pre)
    const int da = a.indexOf('-'), db = b.indexOf('-');
    const String na = (da >= 0) ? a.substring(0, da) : a;
    const String nb = (db >= 0) ? b.substring(0, db) : b;

    int ia = 0, ib = 0, result = 0;
    for (int k = 0; k < 3; ++k) {
        const int pa = fw_parse_version_part(na, ia);
        const int pb = fw_parse_version_part(nb, ib);
        const int va = (pa < 0) ? 0 : pa;
        const int vb = (pb < 0) ? 0 : pb;
        if (va < vb) { result = -1; break; }
        if (va > vb) { result =  1; break; }
        if (pa < 0 && pb < 0) break;
    }

    // Equal numeric parts: release (no suffix) > pre-release (has suffix)
    if (result == 0) {
        if (da >= 0 && db < 0) return -1;
        if (da < 0 && db >= 0) return  1;
    }
    return result;
}

static bool fw_manifest_configured() {
    const char* url = FW_UPDATE_MANIFEST_URL;
    return (url != nullptr) && (strlen(url) > 0);
}

static void fw_set_status_locked(const String& status, const String& err = "") {
    g_fw_update.status = status;
    g_fw_update.last_error = err;
}

static void fw_ui_start_install() {
    ota_in_progress = true;
    g_fw_progress_last_log_ms = 0;
    g_fw_progress_last_percent = -1;

    if (ui_FWUpdate_screen != nullptr && lv_screen_active() != ui_FWUpdate_screen) {
        lv_disp_load_scr(ui_FWUpdate_screen);
    }
    if (ui_Label_FWUpdateInfo != nullptr) {
        lv_label_set_text(ui_Label_FWUpdateInfo, "Firmware Update in progress...");
    }
    update_ota_progress_screen(0);
}

static void fw_ui_progress_update(int current, int total) {
    if (total <= 0) return;

    int progress = (int)(((float)current / (float)total) * 100.0f);
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;

    if (progress != g_fw_progress_last_percent) {
        g_fw_progress_last_percent = progress;
        update_ota_progress_screen(progress);
    }

    const uint32_t now = millis();
    if ((uint32_t)(now - g_fw_progress_last_log_ms) >= 1000) {
        g_fw_progress_last_log_ms = now;
        logger.notice("[FW] Download progress: %d%% (%d / %d Bytes)", progress, current, total);
    }
}

static void fw_ui_finish_success() {
    update_ota_progress_screen(100);
    if (ui_Label_FWUpdateInfo != nullptr) {
        lv_label_set_text(ui_Label_FWUpdateInfo, "FWUpdate successful!\n\nperforming Reset");
    }
}

static void fw_ui_finish_error(const String& err) {
    if (ui_Label_FWUpdateInfo != nullptr) {
        String info = "FWUpdate failed:\n";
        info += err;
        lv_label_set_text(ui_Label_FWUpdateInfo, info.c_str());
    }
}

static void fw_update_check_manifest_now() {
    // Advance the retry timer upfront so any failure still delays the next attempt.
    if (fw_update_lock()) {
        g_fw_update.last_check_ms = millis();
        fw_update_unlock();
    }

    if (!fw_manifest_configured()) {
        if (fw_update_lock()) {
            fw_set_status_locked("disabled", "FW_UPDATE_MANIFEST_URL not configured");
            g_fw_update.checking = false;
            g_fw_update.update_available = false;
            fw_update_unlock();
        }
        return;
    }

    if (!WiFi.isConnected()) {
        if (fw_update_lock()) {
            fw_set_status_locked("offline", "WiFi not connected");
            g_fw_update.checking = false;
            fw_update_unlock();
        }
        return;
    }

    WiFiClientSecure client;
    client.setCACertBundle(x509_crt_bundle_start);

    HTTPClient http;
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, FW_UPDATE_MANIFEST_URL)) {
        if (fw_update_lock()) {
            fw_set_status_locked("error", "manifest begin failed");
            g_fw_update.checking = false;
            fw_update_unlock();
        }
        return;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        String err = String("manifest http ") + String(code);
        http.end();
        if (fw_update_lock()) {
            fw_set_status_locked("error", err);
            g_fw_update.checking = false;
            fw_update_unlock();
        }
        return;
    }

    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, http.getString());
    http.end();
    if (e) {
        if (fw_update_lock()) {
            fw_set_status_locked("error", String("manifest json: ") + String(e.c_str()));
            g_fw_update.checking = false;
            fw_update_unlock();
        }
        return;
    }

    const String latest = String((const char*)(doc["version"] | ""));
    const String url = String((const char*)(doc["url"] | ""));
    if (latest.isEmpty() || url.isEmpty()) {
        if (fw_update_lock()) {
            fw_set_status_locked("error", "manifest missing version/url");
            g_fw_update.checking = false;
            fw_update_unlock();
        }
        return;
    }

    const bool available = (fw_compare_versions(String(latest), String(APP_FIRMWARE_VERSION)) > 0);

    if (fw_update_lock()) {
        g_fw_update.latest_version = latest;
        g_fw_update.binary_url = url;
        g_fw_update.update_available = available;
        g_fw_update.last_check_ms = millis();
        fw_set_status_locked(available ? "update_available" : "up_to_date");
        g_fw_update.checking = false;
        fw_update_unlock();
    }
}

static void fw_update_install_now() {
    String url;
    if (fw_update_lock()) {
        url = g_fw_update.binary_url;
        g_fw_update.installing = true;
        fw_set_status_locked("installing");
        fw_update_unlock();
    }

    if (url.isEmpty()) {
        if (fw_update_lock()) {
            g_fw_update.installing = false;
            fw_set_status_locked("error", "no binary url");
            fw_update_unlock();
        }
        return;
    }

    if (!WiFi.isConnected()) {
        if (fw_update_lock()) {
            g_fw_update.installing = false;
            fw_set_status_locked("error", "WiFi not connected");
            fw_update_unlock();
        }
        return;
    }

    logger.notice("[FW] Installing update from: %s", url.c_str());
    fw_ui_start_install();

    WiFiClientSecure client;
    client.setCACertBundle(x509_crt_bundle_start);

    httpUpdate.onStart([]() {
        logger.notice("[FW] HTTP update stream started");
    });
    httpUpdate.onProgress([](int current, int total) {
        fw_ui_progress_update(current, total);
    });
    httpUpdate.onEnd([]() {
        logger.notice("[FW] HTTP update stream finished");
    });
    httpUpdate.onError([](int err) {
        logger.warning("[FW] HTTP update callback error=%d", err);
    });
    httpUpdate.rebootOnUpdate(false);
    t_httpUpdate_return ret = httpUpdate.update(client, url, String(APP_FIRMWARE_VERSION));

    if (ret == HTTP_UPDATE_OK) {
        if (fw_update_lock()) {
            g_fw_update.installing = false;
            g_fw_update.update_available = false;
            fw_set_status_locked("updated");
            fw_update_unlock();
        }
        fw_ui_finish_success();
        logger.notice("[FW] Update successful, restarting...");
        delay(500);
        ESP.restart();
        return;
    }

    String err;
    if (ret == HTTP_UPDATE_NO_UPDATES) err = "no update";
    else err = String("install failed: ") + httpUpdate.getLastErrorString();

    if (fw_update_lock()) {
        g_fw_update.installing = false;
        fw_set_status_locked("error", err);
        fw_update_unlock();
    }
    ota_in_progress = false;
    fw_ui_finish_error(err);
    logger.warning("[FW] %s", err.c_str());
}

static void fw_update_process_tick() {
    if (ota_in_progress) return;

    bool do_install = false;
    bool do_check = false;
    uint32_t last_check = 0;

    if (fw_update_lock()) {
        do_install = g_fw_update.install_requested && !g_fw_update.installing;
        if (do_install) {
            g_fw_update.install_requested = false;
        } else {
            do_check = g_fw_update.check_requested && !g_fw_update.checking && !g_fw_update.installing;
            if (do_check) {
                g_fw_update.check_requested = false;
                g_fw_update.checking = true;
                fw_set_status_locked("checking");
            } else {
                last_check = g_fw_update.last_check_ms;
            }
        }
        fw_update_unlock();
    }

    if (!do_install && !do_check) {
        const uint32_t now = millis();
        if ((uint32_t)(now - last_check) >= (uint32_t)FW_UPDATE_CHECK_INTERVAL_MS && fw_manifest_configured()) {
            if (fw_update_lock()) {
                if (!g_fw_update.checking && !g_fw_update.installing) {
                    g_fw_update.checking = true;
                    g_fw_update.check_requested = false;
                    fw_set_status_locked("checking");
                    do_check = true;
                }
                fw_update_unlock();
            }
        }
    }

    if (do_install) fw_update_install_now();
    if (do_check) fw_update_check_manifest_now();
}

void fw_update_init() {
    if (g_fw_update_mutex == nullptr) {
        g_fw_update_mutex = xSemaphoreCreateMutex();
    }
}

void fw_update_start_task(BaseType_t core_id, UBaseType_t priority) {
    (void)core_id;
    (void)priority;
    fw_update_init();
    // No-op: FW updates are processed by fw_update_poll() in FSM context.
}

void fw_update_poll() {
    static bool initialized = false;
    if (!initialized) {
        fw_update_init();
        initialized = true;
        logger.notice("[FW] updater poll active (current=%s)", APP_FIRMWARE_VERSION);
    }

    const uint32_t now = millis();
    if ((uint32_t)(now - g_fw_poll_last_ms) < 1000U) return;
    g_fw_poll_last_ms = now;

    fw_update_process_tick();
}

String fw_update_get_status_json() {
    JsonDocument d;
    if (fw_update_lock()) {
        d["current_version"] = APP_FIRMWARE_VERSION;
        d["latest_version"] = g_fw_update.latest_version;
        d["update_available"] = g_fw_update.update_available;
        d["checking"] = g_fw_update.checking;
        d["installing"] = g_fw_update.installing;
        d["last_check_ms"] = g_fw_update.last_check_ms;
        d["status"] = g_fw_update.status;
        d["last_error"] = g_fw_update.last_error;
        d["manifest_configured"] = fw_manifest_configured();
        fw_update_unlock();
    } else {
        d["current_version"] = APP_FIRMWARE_VERSION;
        d["status"] = "lock_timeout";
        d["manifest_configured"] = fw_manifest_configured();
    }

    String out;
    serializeJson(d, out);
    return out;
}

void fw_update_request_check_now() {
    if (!fw_update_lock()) return;
    g_fw_update.check_requested = true;
    fw_update_unlock();
}

bool fw_update_request_install(String& message) {
    if (!fw_update_lock()) {
        message = "lock timeout";
        return false;
    }

    if (g_fw_update.installing) {
        message = "install already running";
        fw_update_unlock();
        return false;
    }
    if (!g_fw_update.update_available || g_fw_update.binary_url.isEmpty()) {
        message = "no update available";
        fw_update_unlock();
        return false;
    }

    g_fw_update.install_requested = true;
    message = "install scheduled";
    fw_update_unlock();
    return true;
}
