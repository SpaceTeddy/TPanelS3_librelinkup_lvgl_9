// webpage.cpp (MERGED: Dashboard + Config + Legacy Config Handlers)
// ------------------------------------------------------------
// - /                 -> Dashboard (dark mode, chart, etc.)
// - /configuration    -> Config UI (index_html) with BasicAuth (uses LLU login_email/login_password)
// - Legacy endpoints used by the Config page are preserved:
//     /scan, /login, /connect, /status, /toggle, /setBrightness,
//     /configureWireGuard, /configureMQTT
// - API endpoints for dashboard:
//     /api/glucose, /api/glucose/history
// - Optional config prefill API (if web_config_api.cpp provided):
//     /api/config
// ------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <time.h>

#ifdef ESP32
#include <esp_system.h>
#include <esp_heap_caps.h>
#endif

#include <librelinkup.h>

#include <string>
#include <vector>
#include <uuid/common.h>
#include <uuid/console.h>
#include <uuid/telnet.h>
#include <uuid/log.h>

#include "webpage.h"
#include "settings.h"
#include "tpanels3.h"
#include "main.h"
#include "http_update.h"
#include "mqtt_handler.h"
#include "h2_ota.h"

//------------------------[ uuid logger ]-----------------------------------
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//-------------------------------------------------------------------------

extern SETTINGS settings;
extern TPanelS3 tpanels3;

extern LIBRELINKUP librelinkup;
extern String g_h2_fw_version;
extern String g_h2_fw_build;
extern String g_h2_chip_model;
extern String g_h2_chip_rev;
extern String g_h2_chip_mac;
extern String g_h2_chip_cores;
extern String g_h2_chip_cpu_mhz;
extern String g_h2_chip_xtal_mhz;
extern String g_h2_chip_features;
extern String g_h2_last_type;
extern String g_h2_last_json;
extern uint32_t g_h2_last_seen_ms;


// int16_t fix (used by debug endpoint)
extern int16_t glucose_delta;
// For handlers needing server access (OTA toggle)
static AsyncWebServer* g_server = nullptr;

// Forward declarations (used by debug endpoint)
String web_get_glucose_latest_json();

// Local state (legacy)
static String username;
static String password;
static String wifi_bssid;
static String wifi_password;
static uint32_t g_h2_info_req_last_ms = 0;

static void maybe_request_h2_info()
{
    if (h2_ota_in_progress()) return;
    const uint32_t now = millis();
    if ((now - g_h2_info_req_last_ms) < 15000U)
        return;
    g_h2_info_req_last_ms = now;
    h2_send("{\"cmd\":\"version\"}");
    h2_send("{\"cmd\":\"chipinfo\"}");
}

static bool ensureConfigAuth(AsyncWebServerRequest *request) {
    const String& user = settings.config.login_email;
    const String& pass = settings.config.login_password;
    if (user.length() != 0 && pass.length() != 0) {
        if (!request->authenticate(user.c_str(), pass.c_str())) {
            request->requestAuthentication();
            return false;
        }
    }
    return true;
}

// -------------------- Debug handlers --------------------
static void handleDebugPage(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    AsyncWebServerResponse* resp = request->beginResponse(LittleFS, "/debug.html", "text/html; charset=utf-8");
    resp->addHeader("Cache-Control", "no-store");
    request->send(resp);
}

static void handleApiDebug(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    maybe_request_h2_info();

    JsonDocument doc;

    doc["millis"] = (uint32_t)millis();
    time_t now = time(nullptr);
    doc["time_epoch"] = (uint32_t)now;

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["connected"] = WiFi.isConnected();
    wifi["ssid"] = WiFi.SSID();
    wifi["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
    wifi["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String("");

    JsonObject heap = doc["heap"].to<JsonObject>();
    // Heap stats (ESP-IDF)
#ifdef ESP32
    heap["total_free"] = (uint32_t)esp_get_free_heap_size();
    heap["largest_free_block"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    heap["internal_dma"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    heap["psram"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    heap["min_free"] = (uint32_t)ESP.getMinFreeHeap();
#else
    heap["total_free"] = (uint32_t)ESP.getFreeHeap();
#endif


    // Embed the normal /api/glucose payload
    JsonDocument glu;
    DeserializationError e1 = deserializeJson(glu, web_get_glucose_latest_json());
    if (!e1) {
        doc["glucose"] = glu.as<JsonVariant>();
    } else {
        doc["glucose_parse_error"] = e1.c_str();
        doc["glucose_raw"] = web_get_glucose_latest_json();
    }

    // Provide delta as int16_t sanity check
    doc["glucose_delta_int16"] = (int)glucose_delta;

    // LibreLinkUp status + sensor snapshot
    JsonObject llu = doc["llu"].to<JsonObject>();

    JsonObject st = llu["status"].to<JsonObject>();
    st["timestamp_status"] = (uint8_t)librelinkup.status().timestamp_status;
    st["sensor_state"] = (uint8_t)librelinkup.status().sensor_state;
    st["last_timestamp_unixtime"] = (uint32_t)librelinkup.status().last_timestamp_unixtime;

    JsonObject se = llu["sensor"].to<JsonObject>();
    se["sensor_state"] = (uint8_t)librelinkup.sensor_data().sensor_state;
    se["sensor_sn_non_active"] = librelinkup.sensor_data().sensor_sn_non_active;
    se["sensor_id_non_active"] = librelinkup.sensor_data().sensor_id_non_active;
    se["sensor_non_activ_unixtime"] = (uint32_t)librelinkup.sensor_data().sensor_non_activ_unixtime;
    se["sensor_id"] = librelinkup.sensor_data().sensor_id;
    se["sensor_sn"] = librelinkup.sensor_data().sensor_sn;
    se["sensor_type_dtid"] = (uint16_t)librelinkup.sensor_data().sensor_type_dtid;
    {
        // Prefer SN-based detection; fall back to non-active SN before touching dtid
        const String &sn_a = librelinkup.sensor_data().sensor_sn;
        const String &sn_b = librelinkup.sensor_data().sensor_sn_non_active;
        const String &sn   = (sn_a.length() >= 5) ? sn_a : sn_b;
        se["sensor_type_name"]   = librelinkup.sensor_device_type_to_string(
                                       librelinkup.get_sensor_device_type_from_sn(sn));
        se["sensor_type_sn_src"] = sn.length() >= 5 ? sn : String("--");
    }
    se["sensor_runtime"] = (uint32_t)librelinkup.sensor_data().sensor_runtime;
    se["sensor_activation_time"] = (uint32_t)librelinkup.sensor_data().sensor_activation_time;

// LibreLinkUp login snapshot (no secrets in cleartext)
JsonObject lo = llu["login"].to<JsonObject>();
lo["email"] = librelinkup.login_data().email;
lo["user_id"] = librelinkup.login_data().user_id;
lo["account_id"] = librelinkup.login_data().account_id;
lo["user_country"] = librelinkup.login_data().user_country;
lo["connection_country"] = librelinkup.login_data().connection_country;
lo["connection_status"] = (int16_t)librelinkup.login_data().connection_status;
lo["user_token_expires"] = (uint32_t)librelinkup.login_data().user_token_expires;
lo["user_login_status"] = (uint8_t)librelinkup.login_data().user_login_status;

// Security helpers
lo["password_set"] = (librelinkup.login_data().password.length() > 0);
lo["token_present"] = (librelinkup.login_data().user_token.length() > 0);

// Short token preview (safe)
if (librelinkup.login_data().user_token.length() > 10) {
    lo["token_preview"] =
        librelinkup.login_data().user_token.substring(0, 6) + "..." +
        librelinkup.login_data().user_token.substring(librelinkup.login_data().user_token.length() - 4);
} else {
    lo["token_preview"] = String("");
}


    // Add a small config snapshot (no secrets)
    JsonObject cfg = doc["config"].to<JsonObject>();
    cfg["ota_update"]  = settings.config.ota_update;
    cfg["ota_staging"] = settings.config.ota_staging;
    cfg["ota_force"]   = settings.config.ota_force;
    cfg["wg_mode"] = settings.config.wg_mode;
    cfg["mqtt_mode"] = settings.config.mqtt_mode;
    cfg["mqtt_master_mode"] = settings.config.mqtt_master_mode;
    cfg["brightness"] = settings.config.brightness;
    cfg["display_dim_timeout_s"] = settings.config.display_dim_timeout_s;

    JsonObject h2 = doc["h2"].to<JsonObject>();
    h2["fw_version"] = g_h2_fw_version;
    h2["fw_build"] = g_h2_fw_build;
    h2["chip_model"] = g_h2_chip_model;
    h2["chip_revision"] = g_h2_chip_rev;
    h2["chip_mac"] = g_h2_chip_mac;
    h2["chip_cores"] = g_h2_chip_cores;
    h2["chip_cpu_mhz"] = g_h2_chip_cpu_mhz;
    h2["chip_xtal_mhz"] = g_h2_chip_xtal_mhz;
    h2["chip_features"] = g_h2_chip_features;
    h2["last_type"] = g_h2_last_type;
    h2["last_seen_ms_ago"] = (g_h2_last_seen_ms > 0) ? (uint32_t)(millis() - g_h2_last_seen_ms) : 0;
    h2["has_data"] = (g_h2_last_seen_ms > 0);
    h2["last_json"] = g_h2_last_json;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json; charset=utf-8", out);
}


// -------------------- Dashboard/API hooks (implemented elsewhere) --------------------
// Provide these in web_glucose_api.cpp (recommended). Weak fallbacks keep compilation working.
__attribute__((weak)) String web_get_glucose_latest_json() { return String("{\"mgdl\":null,\"low\":null,\"high\":null,\"delta\":null,\"trend\":\"--\",\"ts_ok\":false}"); }
__attribute__((weak)) String web_get_glucose_history_json() { return String("{\"low\":null,\"high\":null,\"values\":[]}"); }

// Optional: config prefill API (provide web_config_api.cpp). Weak fallback keeps compilation working.
__attribute__((weak)) String web_get_config_json() { return String("{\"ok\":false}"); }

// -------------------- Handlers: Dashboard + Config --------------------
static void handleDashboard(AsyncWebServerRequest *request) {
    request->send(LittleFS, "/dashboard.html", "text/html; charset=utf-8");
}

static void handleConfiguration(AsyncWebServerRequest *request) {
    const String& user = settings.config.login_email;
    const String& pass = settings.config.login_password;

    // If no credentials configured, leave open
    if (user.length() == 0 || pass.length() == 0) {
        request->send(LittleFS, "/index.html", "text/html; charset=utf-8");
        return;
    }

    if (!request->authenticate(user.c_str(), pass.c_str())) {
        return request->requestAuthentication();
    }

    request->send(LittleFS, "/index.html", "text/html; charset=utf-8");
}

static void handleConfigRedirect(AsyncWebServerRequest *request) {
    request->redirect("/configuration");
}

static void handleApiGlucose(AsyncWebServerRequest *request) {
    request->send(200, "application/json; charset=utf-8", web_get_glucose_latest_json());
}

static void handleApiGlucoseHistory(AsyncWebServerRequest *request) {
    request->send(200, "application/json; charset=utf-8", web_get_glucose_history_json());
}

static void handleApiConfig(AsyncWebServerRequest *request) {
    // Prefill endpoint used by config page JS. Protect it with the same BasicAuth as /configuration.
    if (!ensureConfigAuth(request)) return;

    request->send(200, "application/json", web_get_config_json());
}

static void handleApiFwStatus(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    maybe_request_h2_info();

    JsonDocument doc;
    const String fw = fw_update_get_status_json();
    if (deserializeJson(doc, fw) != DeserializationError::Ok)
    {
        request->send(200, "application/json; charset=utf-8", fw);
        return;
    }

    JsonObject h2 = doc["h2"].to<JsonObject>();
    h2["fw_version"] = g_h2_fw_version;
    h2["fw_build"] = g_h2_fw_build;
    h2["chip_model"] = g_h2_chip_model;
    h2["chip_revision"] = g_h2_chip_rev;
    h2["chip_mac"] = g_h2_chip_mac;
    h2["chip_cores"] = g_h2_chip_cores;
    h2["chip_cpu_mhz"] = g_h2_chip_cpu_mhz;
    h2["chip_xtal_mhz"] = g_h2_chip_xtal_mhz;
    h2["chip_features"] = g_h2_chip_features;
    h2["last_seen_ms_ago"] = (g_h2_last_seen_ms > 0) ? (uint32_t)(millis() - g_h2_last_seen_ms) : 0;
    h2["has_data"] = (g_h2_last_seen_ms > 0);

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json; charset=utf-8", out);
}

static void handleApiFwCheck(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    fw_update_request_check_now();
    request->send(202, "application/json; charset=utf-8", "{\"status\":\"scheduled\"}");
}

static void handleApiFwInstall(AsyncWebServerRequest *request) {
    if (!ensureConfigAuth(request)) return;
    String msg;
    if (!fw_update_request_install(msg)) {
        request->send(409, "application/json; charset=utf-8",
                      String("{\"status\":\"rejected\",\"message\":\"") + msg + "\"}");
        return;
    }
    request->send(202, "application/json; charset=utf-8",
                  String("{\"status\":\"accepted\",\"message\":\"") + msg + "\"}");
}

// -------------------- Legacy handlers used by index_html --------------------
static void handleLogin(AsyncWebServerRequest *request) {
    if (request->hasParam("username", true)) {
        username = request->getParam("username", true)->value();
    }
    if (request->hasParam("password", true)) {
        password = request->getParam("password", true)->value();
    }

    settings.config.login_email    = username;
    settings.config.login_password = password;
    librelinkup.set_credentials(settings.config.login_email, settings.config.login_password);
    settings.saveConfiguration(settings.config_filename, settings.config);

    request->send(200, "text/html", "Login successful!<br><a href='/configuration'>Back</a>");
}

static void handleScan(AsyncWebServerRequest *request) {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        request->send(200, "application/json", "{\"scanning\":true}");
        return;
    }
    if (n < 0) {
        // No scan running — start one async and tell client to retry
        WiFi.scanNetworks(true);
        request->send(200, "application/json", "{\"scanning\":true}");
        return;
    }
    // Scan complete — return results
    String json = "[";
    bool first = true;
    for (int i = 0; i < n; ++i) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    request->send(200, "application/json", json);
}

static void handleConnect(AsyncWebServerRequest *request) {
    if (request->hasParam("networks", true)) {
        wifi_bssid = request->getParam("networks", true)->value();
        settings.config.wifi_bssid = wifi_bssid;
    }
    if (request->hasParam("wifiPassword", true)) {
        wifi_password = request->getParam("wifiPassword", true)->value();
        settings.config.wifi_password = wifi_password;
    }

    settings.saveConfiguration(settings.config_filename, settings.config);
    ESP.restart();
}

static void handleStatus(AsyncWebServerRequest *request) {
    settings.loadConfiguration(settings.config_filename, settings.config);

    JsonDocument json_config;
    json_config["ota_update"]        = settings.config.ota_update;
    json_config["ota_staging"]       = settings.config.ota_staging;
    json_config["ota_force"]         = settings.config.ota_force;
    json_config["wg_mode"]           = settings.config.wg_mode;
    json_config["mqtt_mode"]         = settings.config.mqtt_mode;
    json_config["mqtt_master_mode"]  = settings.config.mqtt_master_mode;
    json_config["brightness"]        = settings.config.brightness;

    String jsonResponse;
    serializeJson(json_config, jsonResponse);
    request->send(200, "application/json", jsonResponse);
}

static void handleToggleFeature(AsyncWebServerRequest *request) {
    if (!(request->hasParam("feature") && request->hasParam("status"))) {
        request->send(400, "application/json", "{\"error\": \"Missing parameters\"}");
        return;
    }

    String feature = request->getParam("feature")->value();
    int status = request->getParam("status")->value().toInt();

    if (feature == "ota_update") {
        settings.config.ota_update = status;
        logger.notice("OTA_Update: %d", settings.config.ota_update);
        settings.saveConfiguration(settings.config_filename, settings.config);

    } else if (feature == "ota_staging") {
        settings.config.ota_staging = status;
        logger.notice("OTA_Staging: %d", settings.config.ota_staging);
        fw_update_request_check_now();
        settings.saveConfiguration(settings.config_filename, settings.config);

    } else if (feature == "ota_force") {
        settings.config.ota_force = status;
        logger.notice("OTA_Force: %d", settings.config.ota_force);
        fw_update_request_check_now();
        settings.saveConfiguration(settings.config_filename, settings.config);

    } else if (feature == "wg_mode") {
        settings.config.wg_mode = status;
        logger.notice("wg_mode: %d", settings.config.wg_mode);
        setup_wg(settings.config.wg_mode);
    } else if (feature == "mqtt_mode") {
        settings.config.mqtt_mode = status;
        logger.notice("mqtt_mode: %d", settings.config.mqtt_mode);
    } else if (feature == "mqtt_master_mode") {
        settings.config.mqtt_master_mode = status;
        logger.notice("mqtt_master_mode: %d", settings.config.mqtt_master_mode);
        mqtt_publish_ha_discovery();
    } else if (feature == "ha_discovery") {
        settings.config.ha_discovery = status;
        logger.notice("ha_discovery: %d", settings.config.ha_discovery);
        mqtt_publish_ha_discovery();
    } else {
        request->send(400, "application/json", "{\"error\": \"Unknown feature\"}");
        return;
    }

    settings.saveConfiguration(settings.config_filename, settings.config);
    request->send(200, "application/json", "{\"status\": \"updated\"}");
}

static void handleSetDimTimeout(AsyncWebServerRequest *request) {
    if (!request->hasParam("value", true)) {
        request->send(400, "application/json", "{\"error\": \"Missing value\"}");
        return;
    }
    long secs = request->getParam("value", true)->value().toInt();
    if (secs < 0) secs = 0;
    if (secs > 86400) secs = 86400;
    settings.config.display_dim_timeout_s = (uint32_t)secs;
    settings.saveConfiguration(settings.config_filename, settings.config);
    request->send(200, "application/json",
                  "{\"display_dim_timeout_s\": " + String((unsigned long)secs) + "}");
}

static void handleSetBrightness(AsyncWebServerRequest *request) {
    if (!request->hasParam("value")) {
        request->send(400, "application/json", "{\"error\": \"Invalid parameters\"}");
        return;
    }

    int brightness = request->getParam("value")->value().toInt();
    if (brightness < 0) brightness = 0;
    if (brightness > 255) brightness = 255;

    settings.config.brightness = brightness;
    tpanels3.set_backlight_brightness(brightness);

    request->send(200, "application/json", "{\"brightness\": " + String(brightness) + "}");
}

static void handleConfigureWireGuard(AsyncWebServerRequest *request) {
    String privateKey, publicKey, presharedKey, ipAddress, endpoint, allowedIPs;
    int endpointPort = 0;

    if (request->hasParam("privateKey", true))   privateKey   = request->getParam("privateKey", true)->value();
    if (request->hasParam("publicKey", true))    publicKey    = request->getParam("publicKey", true)->value();
    if (request->hasParam("presharedKey", true)) presharedKey = request->getParam("presharedKey", true)->value();
    if (request->hasParam("ipAddress", true))    ipAddress    = request->getParam("ipAddress", true)->value();
    if (request->hasParam("endpoint", true))     endpoint     = request->getParam("endpoint", true)->value();
    if (request->hasParam("endpointPort", true)) endpointPort = request->getParam("endpointPort", true)->value().toInt();
    if (request->hasParam("allowedIPs", true))   allowedIPs   = request->getParam("allowedIPs", true)->value();

    const bool ok = !privateKey.isEmpty() && !publicKey.isEmpty() && !presharedKey.isEmpty() &&
                    !ipAddress.isEmpty() && !endpoint.isEmpty() && endpointPort > 0 && !allowedIPs.isEmpty();

    if (!ok) {
        logger.notice("Missing WireGuard parameters in request");
        request->send(400, "application/json", "{\"error\": \"Missing parameters\"}");
        return;
    }

    settings.config.wgPrivateKey   = privateKey;
    settings.config.wgPublicKey    = publicKey;
    settings.config.wgPresharedKey = presharedKey;
    settings.config.wgIpAddress    = ipAddress;
    settings.config.wgEndpoint     = endpoint;
    settings.config.wgEndpointPort = endpointPort;
    settings.config.wgAllowedIPs   = allowedIPs;

    logger.notice("WireGuard configuration parsed and saved");
    settings.saveConfiguration(settings.config_filename, settings.config);

    request->send(200, "application/json", "{\"status\": \"WireGuard configuration saved\"}");
}

static String g_wifi_networks_body;

static void handleConfigureWiFiNetworksBody(AsyncWebServerRequest *request,
                                             uint8_t *data, size_t len,
                                             size_t index, size_t total)
{
    if (index == 0) { g_wifi_networks_body = ""; g_wifi_networks_body.reserve(total); }
    for (size_t i = 0; i < len; i++) g_wifi_networks_body += (char)data[i];
    if (index + len < total) return;

    JsonDocument doc;
    if (deserializeJson(doc, g_wifi_networks_body) || !doc["networks"].is<JsonArray>()) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        g_wifi_networks_body = "";
        return;
    }

    settings.config.wifi_networks.clear();
    for (JsonObject net : doc["networks"].as<JsonArray>()) {
        if (settings.config.wifi_networks.size() >= WIFI_NETWORKS_MAX) break;
        SETTINGS::WifiNetwork n;
        n.ssid     = net["ssid"].as<String>();
        n.password = net["password"].as<String>();
        if (n.ssid.length() > 0)
            settings.config.wifi_networks.push_back(n);
    }
    if (!settings.config.wifi_networks.empty()) {
        settings.config.wifi_bssid    = settings.config.wifi_networks[0].ssid;
        settings.config.wifi_password = settings.config.wifi_networks[0].password;
    }

    settings.saveConfiguration(settings.config_filename, settings.config);
    g_wifi_networks_body = "";

    request->send(200, "application/json", "{\"status\":\"saved\",\"count\":" +
                  String(settings.config.wifi_networks.size()) + "}");
    ESP.restart();
}

static void handleConfigureMQTT(AsyncWebServerRequest *request) {
    String serverName, user, pass;
    int port = 0;

    if (request->hasParam("server", true))   serverName = request->getParam("server", true)->value();
    if (request->hasParam("port", true))     port       = request->getParam("port", true)->value().toInt();
    if (request->hasParam("username", true)) user       = request->getParam("username", true)->value();
    if (request->hasParam("password", true)) pass       = request->getParam("password", true)->value();

    if (serverName.isEmpty() || port <= 0) {
        logger.notice("Missing 'server' or 'port' parameters in request");
        request->send(400, "application/json", "{\"error\": \"Missing server or port parameters\"}");
        return;
    }

    settings.config.mqttServer   = serverName;
    settings.config.mqtt_port    = port;
    settings.config.mqttUsername = user;
    settings.config.mqttPassword = pass;

    logger.notice("MQTT configuration parsed and saved");
    settings.saveConfiguration(settings.config_filename, settings.config);

    request->send(200, "application/json", "{\"status\": \"MQTT configuration saved\"}");
}


// -------------------- Telnet WebSocket bridge --------------------
// Browser can't do raw TCP; this bridges WebSocket <-> Telnet TCP (LAN use).
#include <Ticker.h>
#include <map>

static AsyncWebSocket g_ws_telnet("/ws/telnet");
static Ticker g_telnet_ticker;

struct TelnetSession {
    AsyncWebSocketClient* ws = nullptr;
    WiFiClient tcp;
    bool tcp_connected = false;
    String host;
    uint16_t port = 23;
};

static std::map<uint32_t, TelnetSession> g_telnet_sessions;

// Protocol: status messages are prefixed with '\x01' (SOH); everything else
// is raw terminal data forwarded as-is.  No JSON — this avoids parse failures
// when the terminal output itself contains JSON-like characters, and ensures
// the browser sees actual CR/LF and ANSI bytes rather than JSON escape sequences.
static void telnet_send_status(AsyncWebSocketClient* c, const String& msg) {
    if (!c) return;
    String out("\x01");
    out += msg;
    c->text(out);
}

static void telnet_send_data(AsyncWebSocketClient* c, const String& data) {
    if (!c) return;
    c->text(data);   // raw terminal bytes — no encoding
}

static void telnet_service() {
    for (auto it = g_telnet_sessions.begin(); it != g_telnet_sessions.end(); ) {
        auto& s = it->second;
        if (!s.ws || s.ws->status() != WS_CONNECTED) {
            if (s.tcp_connected) s.tcp.stop();
            it = g_telnet_sessions.erase(it);
            continue;
        }

        if (s.tcp_connected && s.tcp.connected()) {
            // Collect all available TCP bytes into ONE message per tick so the
            // browser receives a single WebSocket frame (AsyncWebSocket may coalesce
            // rapid c->text() calls into one frame, breaking JSON.parse).
            String all_data;
            all_data.reserve(512);
            while (s.tcp.available() && all_data.length() < 4096) {
                uint8_t b = (uint8_t)s.tcp.read();
                // Minimal TELNET negotiation handling (IAC)
                if (b == 255) { // IAC
                    if (s.tcp.available() < 2) continue; // partial — skip
                    uint8_t cmd = (uint8_t)s.tcp.read();
                    uint8_t opt = (uint8_t)s.tcp.read();
                    // Respond by refusing options (DO->WONT, WILL->DONT)
                    if (cmd == 253) { uint8_t resp[3] = {255, 252, opt}; s.tcp.write(resp, 3); }
                    else if (cmd == 251) { uint8_t resp[3] = {255, 254, opt}; s.tcp.write(resp, 3); }
                    // Drop negotiation bytes from output
                    continue;
                }
                all_data += (char)b;
            }
            if (all_data.length()) telnet_send_data(s.ws, all_data);
        } else {
            s.tcp_connected = false;
        }
        ++it;
    }

    // Stop ticker if no sessions
    if (g_telnet_sessions.empty()) {
        g_telnet_ticker.detach();
    }
}

static std::map<uint32_t, bool> g_ws_auth_ok;

static inline void ws_set_authorized(AsyncWebSocketClient* c, bool ok) {
    if (!c) return;
    if (ok) {
        g_ws_auth_ok[c->id()] = true;
    } else {
        g_ws_auth_ok.erase(c->id());
    }
}

static inline bool ws_is_authorized(AsyncWebSocketClient* c) {
    if (!c) return false;
    auto it = g_ws_auth_ok.find(c->id());
    return (it != g_ws_auth_ok.end()) && it->second;
}

static void ws_telnet_on_event(AsyncWebSocket *server, AsyncWebSocketClient *client,
                              AwsEventType type, void *arg, uint8_t *data, size_t len) {
    (void)server;

    if (type == WS_EVT_CONNECT) {
        // WS auth: some ESPAsyncWebServer builds do not provide reliable access
        // to the originating HTTP request (and BasicAuth headers) here.
        // The debug page itself is already access-controlled in your setup,
        // so we allow the WS connection and keep the terminal usable.
        ws_set_authorized(client, true);

        TelnetSession sess;
        sess.ws = client;
        g_telnet_sessions[client->id()] = sess;

        telnet_send_status(client, "ready");
        if (!g_telnet_ticker.active()) {
            g_telnet_ticker.attach_ms(20, telnet_service);
        }
        return;
    }


    if (type == WS_EVT_DISCONNECT) {
        ws_set_authorized(client, false);
        auto it = g_telnet_sessions.find(client->id());
        if (it != g_telnet_sessions.end()) {
            if (it->second.tcp_connected) it->second.tcp.stop();
            g_telnet_sessions.erase(it);
        }
        return;
    }

    if (type == WS_EVT_DATA) {
        if (!ws_is_authorized(client)) { client->close(); return; }
        auto it = g_telnet_sessions.find(client->id());
        if (it == g_telnet_sessions.end()) return;
        TelnetSession& sess = it->second;

        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (!info || info->final != 1 || info->index != 0) return;
        if (info->opcode != WS_TEXT) return;

        String msg;
        msg.reserve(len + 1);
        for (size_t i=0;i<len;i++) msg += (char)data[i];

        JsonDocument d;
        DeserializationError e = deserializeJson(d, msg);
        if (e) {
            telnet_send_status(client, "bad json");
            return;
        }

        const char* cmd = d["cmd"] | "";
        if (strcmp(cmd, "connect") == 0) {
            const char* host = d["host"] | "";
            uint16_t port = (uint16_t)(d["port"] | 23);
            if (!host || strlen(host)==0) { telnet_send_status(client, "host missing"); return; }

            if (sess.tcp_connected) sess.tcp.stop();
            sess.host = host;
            sess.port = port;
            sess.tcp.setTimeout(2000);

            telnet_send_status(client, "connecting...");
            bool ok = sess.tcp.connect(sess.host.c_str(), sess.port);
            sess.tcp_connected = ok;
            telnet_send_status(client, ok ? "connected" : "connect failed");
            return;
        }

        if (strcmp(cmd, "disconnect") == 0) {
            if (sess.tcp_connected) sess.tcp.stop();
            sess.tcp_connected = false;
            telnet_send_status(client, "disconnected");
            return;
        }

        if (strcmp(cmd, "send") == 0) {
            const char* payload = d["data"] | "";
            if (!sess.tcp_connected || !sess.tcp.connected()) {
                telnet_send_status(client, "tcp not connected");
                return;
            }
            if (payload && strlen(payload)) {
                sess.tcp.write((const uint8_t*)payload, strlen(payload));
            }
            return;
        }
    }
}

static uint8_t* g_h2_upload_buf = nullptr;
static size_t   g_h2_upload_pos = 0;
static bool     g_h2_upload_ok  = false;

static void handleH2OtaStatus(AsyncWebServerRequest *request)
{
    char buf[96];
    size_t w = h2_ota_written();
    size_t t = h2_ota_total();
    snprintf(buf, sizeof(buf),
             "{\"active\":%s,\"written\":%u,\"total\":%u}",
             h2_ota_in_progress() ? "true" : "false",
             (unsigned)w, (unsigned)t);
    request->send(200, "application/json", buf);
}

static void handleH2OtaPage(AsyncWebServerRequest *request)
{
    request->send(LittleFS, "/ota_h2.html", "text/html; charset=utf-8");
}

static void handleH2OtaUploadDone(AsyncWebServerRequest *request)
{
    if (g_h2_upload_ok) {
        request->send(202, "application/json", "{\"status\":\"started\"}");
    } else {
        request->send(500, "application/json", "{\"error\":\"upload or ota start failed\"}");
    }
    g_h2_upload_ok = false;
}

static void handleH2OtaUploadBody(AsyncWebServerRequest *request,
                                   const String& /*filename*/,
                                   size_t index, uint8_t *data, size_t len, bool final)
{
    static constexpr size_t MAX_FW = 1536 * 1024; // 1.5 MB — well above H2 OTA partition

    if (index == 0) {
        // Abort any stale buffer
        if (g_h2_upload_buf) { heap_caps_free(g_h2_upload_buf); g_h2_upload_buf = nullptr; }
        g_h2_upload_pos = 0;
        g_h2_upload_ok  = false;
        g_h2_upload_buf = (uint8_t*)heap_caps_malloc(MAX_FW, MALLOC_CAP_SPIRAM);
        if (!g_h2_upload_buf) {
            logger.warning("[H2-OTA] PSRAM alloc failed");
            return;
        }
    }

    if (!g_h2_upload_buf) return; // alloc failed earlier

    if (g_h2_upload_pos + len > MAX_FW) {
        logger.warning("[H2-OTA] firmware too large");
        heap_caps_free(g_h2_upload_buf); g_h2_upload_buf = nullptr;
        return;
    }

    memcpy(g_h2_upload_buf + g_h2_upload_pos, data, len);
    g_h2_upload_pos += len;

    if (final) {
        size_t fw_size = g_h2_upload_pos;
        uint8_t* buf   = g_h2_upload_buf;
        g_h2_upload_buf = nullptr; // ownership passes to h2_ota
        g_h2_upload_pos = 0;
        g_h2_upload_ok  = h2_ota_start_from_buffer(buf, fw_size);
        if (!g_h2_upload_ok) heap_caps_free(buf);
        logger.notice("[H2-OTA] upload done: %u bytes, started=%d", (unsigned)fw_size, g_h2_upload_ok);
    }
}

void register_webpage_routes(AsyncWebServer& server) {
    g_server = &server;

    // Register OTA endpoints once. Do NOT stop the webserver at runtime.
    ElegantOTA.begin(g_server);

// Telnet terminal (WebSocket bridge)
g_ws_telnet.onEvent(ws_telnet_on_event);
server.addHandler(&g_ws_telnet);

    // Dashboard + config page split
    server.on("/",              HTTP_GET,  handleDashboard);
    server.on("/configuration", HTTP_GET,  handleConfiguration);
    server.on("/config",        HTTP_GET,  handleConfigRedirect);


    // Debug page
    server.on("/debug",     HTTP_GET, handleDebugPage);
    server.on("/api/debug", HTTP_GET, handleApiDebug);

    // Dashboard APIs (order matters: longer first)
    server.on("/api/glucose/history", HTTP_GET, handleApiGlucoseHistory);
    server.on("/api/glucose",         HTTP_GET, handleApiGlucose);
    server.on("/api/config",          HTTP_GET, handleApiConfig);
    server.on("/api/fw/status",       HTTP_GET,  handleApiFwStatus);
    server.on("/api/fw/check",        HTTP_POST, handleApiFwCheck);
    server.on("/api/fw/install",      HTTP_POST, handleApiFwInstall);
    server.on("/h2ota",               HTTP_GET,  handleH2OtaPage);
    server.on("/api/h2/ota/status",   HTTP_GET,  handleH2OtaStatus);
    server.on("/api/h2/ota/upload",   HTTP_POST, handleH2OtaUploadDone, handleH2OtaUploadBody);

    // Legacy config endpoints (used by index_html JS)
    server.on("/scan",               HTTP_GET,  handleScan);
    server.on("/login",              HTTP_POST, handleLogin);
    server.on("/connect",            HTTP_POST, handleConnect);
    server.on("/status",             HTTP_GET,  handleStatus);
    server.on("/toggle",             HTTP_POST, handleToggleFeature);
    server.on("/setBrightness",      HTTP_POST, handleSetBrightness);
    server.on("/setDimTimeout",      HTTP_POST, handleSetDimTimeout);
    server.on("/configureWireGuard",   HTTP_POST, handleConfigureWireGuard);
    server.on("/configureMQTT",        HTTP_POST, handleConfigureMQTT);
    server.on("/configureWiFiNetworks", HTTP_POST,
              [](AsyncWebServerRequest *request) {},
              NULL,
              handleConfigureWiFiNetworksBody);
}
