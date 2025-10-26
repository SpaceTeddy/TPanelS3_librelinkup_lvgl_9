/**
 * @file webpage.cpp
 * @brief Route registration and HTTP request handlers for the AsyncWebServer.
 *
 * This module exposes a single public function:
 *   - register_webpage_routes(AsyncWebServer&): registers all HTTP routes.
 *
 * The handlers in this file operate on global project state (e.g., SETTINGS, logger),
 * but are kept here to keep main.cpp slim and to group all web-related code.
 *
 * @note This file intentionally does not change runtime behavior; it only organizes
 *       handlers and documents them using Doxygen-compatible comments.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <ESPAsyncWebServer.h>

#include <string>                   ///< String operations
#include <vector>                   ///< Vector container
#include <uuid/common.h>            ///< UUID common utilities
#include <uuid/console.h>           ///< Console interaction
#include <uuid/telnet.h>            ///< Telnet server
#include <uuid/log.h>               ///< Logging system

#include "webpage.h"                ///< Embedded HTML (e.g., index_html)
#include "settings.h"               ///< SETTINGS (persistent configuration)
#include "tpanels3.h"               ///< TPanelS3 display management

//------------------------[ uuid logger ]-----------------------------------
/**
 * @brief Local logger for this translation unit.
 * @note If you already expose a global logger from main.cpp, prefer `extern` and
 *       remove this local static to avoid duplicate loggers.
 */
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//-------------------------------------------------------------------------

// ---- External symbols from the project (provided elsewhere) -------------
extern SETTINGS settings;           ///< Global configuration storage
extern TPanelS3 tpanels3;           ///< TPanelS3 hardware interface instance
extern String availableNetworks;    ///< JSON produced by Wi-Fi scan task


// ---- Local state (only used in this file) --------------------------------
static String username;
static String password;
static String wifi_bssid;
static String wifi_password;

/**
 * @brief Pointer to the active AsyncWebServer used by handlers that need server access.
 */
static AsyncWebServer* g_server = nullptr;

// -------------------------------------------------------------------------
// Handlers
// -------------------------------------------------------------------------

/**
 * @brief Serves the main HTML page.
 * @param request The incoming HTTP request.
 */
static void handleRoot(AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html);
}

/**
 * @brief Processes login form submission and persists credentials.
 * @param request The incoming HTTP POST request with "username" and "password".
 */
static void handleLogin(AsyncWebServerRequest *request) {
    if (request->hasParam("username", true)) {
        username = request->getParam("username", true)->value();
    }
    if (request->hasParam("password", true)) {
        password = request->getParam("password", true)->value();
    }

    settings.config.login_email    = username;
    settings.config.login_password = password;
    settings.saveConfiguration(settings.config_filename, settings.config);

    // Intentionally kept German response text (UI string), only comments changed to English.
    request->send(200, "text/html", "Login erfolgreich!<br><a href='/'>Zurueck</a>");
}

/**
 * @brief Returns last scanned Wi-Fi networks as JSON.
 * @param request The incoming HTTP GET request.
 * @note The JSON content is generated asynchronously by the background scan task.
 */
static void handleScan(AsyncWebServerRequest *request) {
    request->send(200, "application/json", availableNetworks);
}

/**
 * @brief Persists selected Wi-Fi credentials and reboots to apply.
 * @param request The incoming HTTP POST request with "networks" and "wifiPassword".
 */
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

/**
 * @brief Returns a small device status JSON snapshot (OTA, WG, MQTT, brightness).
 * @param request The incoming HTTP GET request.
 */
static void handleStatus(AsyncWebServerRequest *request) {
    // Ensure we load the latest persisted configuration
    settings.loadConfiguration("/config.json", settings.config);

    // 256 bytes were tight in practice; 512 gives safe headroom for this payload size.
    DynamicJsonDocument json_config(512);
    json_config["ota_update"] = settings.config.ota_update;
    json_config["wg_mode"]    = settings.config.wg_mode;
    json_config["mqtt_mode"]  = settings.config.mqtt_mode;
    json_config["brightness"] = settings.config.brightness;

    String jsonResponse;
    serializeJson(json_config, jsonResponse);
    request->send(200, "application/json", jsonResponse);
}

/**
 * @brief Toggles features via query parameters (ota_update, wg_mode, mqtt_mode).
 *
 * @param request The incoming HTTP POST request with:
 *   - feature: one of "ota_update", "wg_mode", "mqtt_mode"
 *   - status : integer value (0 or 1)
 *
 * @note For "ota_update", this will (re)start or stop the HTTP server for OTA.
 *       Keep `g_server` valid by calling register_webpage_routes() before toggling.
 */
static void handleToggleFeature(AsyncWebServerRequest *request) {
    if (request->hasParam("feature") && request->hasParam("status")) {
        String feature = request->getParam("feature")->value();
        int status = request->getParam("status")->value().toInt();

        Serial.printf("Feature: %s, Status: %d\n", feature.c_str(), status);

        if (feature == "ota_update") {
            settings.config.ota_update = status;
            logger.notice("OTA_Update: %d", settings.config.ota_update);

            if (!g_server) {
                request->send(500, "text/plain", "Server not initialized");
                return;
            }

            if (status == 1) {
                // Simple root info page while OTA is enabled
                g_server->on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
                    req->send(200, "text/plain", "ESP32 LibreLinkup Client");
                });
                ElegantOTA.begin(g_server);
                g_server->begin();
            } else if (status == 0) {
                g_server->end();
            }
        } else if (feature == "wg_mode") {
            settings.config.wg_mode = status;
            logger.notice("wg_mode: %d", settings.config.wg_mode);
            setup_wg(settings.config.wg_mode);
        } else if (feature == "mqtt_mode") {
            settings.config.mqtt_mode = status;
            logger.notice("mqtt_mode: %d", settings.config.mqtt_mode);
        } else {
            request->send(400, "application/json", "{\"error\": \"Unknown feature\"}");
            return;
        }

        request->send(200, "application/json", "{\"status\": \"updated\"}");
    } else {
        request->send(400, "application/json", "{\"error\": \"Missing parameters\"}");
    }
}

/**
 * @brief Sets backlight brightness via HTTP.
 * @param request The incoming HTTP POST request with "value" parameter.
 */
static void handleSetBrightness(AsyncWebServerRequest *request) {
    if (request->hasParam("value")) {
        int brightness = request->getParam("value")->value().toInt();
        Serial.printf("WebPage brightness slider feedback: %d\n", brightness);

        settings.config.brightness = brightness;
        tpanels3.set_backlight_brightness(brightness);

        request->send(200, "application/json", "{\"brightness\": " + String(brightness) + "}");
    } else {
        request->send(400, "application/json", "{\"error\": \"Invalid parameters\"}");
    }
}

/**
 * @brief Configures WireGuard parameters from an HTTP POST request and persists them.
 *
 * @param request The incoming HTTP POST request with form fields:
 *   - privateKey, publicKey, presharedKey, ipAddress, endpoint, endpointPort, allowedIPs
 */
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

    if (ok) {
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
    } else {
        logger.notice("Missing WireGuard parameters in request");
        request->send(400, "application/json", "{\"error\": \"Missing parameters\"}");
    }
}

/**
 * @brief Configures MQTT connection settings via HTTP POST and persists them.
 *
 * @param request The incoming HTTP POST request with form fields:
 *   - server (hostname/IP), port (int), username, password
 */
static void handleConfigureMQTT(AsyncWebServerRequest *request) {
    String serverName, user, pass;
    int port = 0;

    if (request->hasParam("server", true))   serverName = request->getParam("server", true)->value();
    if (request->hasParam("port", true))     port       = request->getParam("port", true)->value().toInt();
    if (request->hasParam("username", true)) user       = request->getParam("username", true)->value();
    if (request->hasParam("password", true)) pass       = request->getParam("password", true)->value();

    if (!serverName.isEmpty() && port > 0) {
        settings.config.mqttServer   = serverName;
        settings.config.mqtt_port    = port;
        settings.config.mqttUsername = user;
        settings.config.mqttPassword = pass;

        logger.notice("MQTT configuration parsed and saved");
        settings.saveConfiguration(settings.config_filename, settings.config);

        request->send(200, "application/json", "{\"status\": \"MQTT configuration saved\"}");
    } else {
        logger.notice("Missing 'server' or 'port' parameters in request");
        request->send(400, "application/json", "{\"error\": \"Missing server or port parameters\"}");
    }
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

/**
 * @brief Registers all webpage routes and stores the server pointer for internal use.
 * @param server Reference to the AsyncWebServer instance to register routes on.
 *
 * @note Must be called before toggling OTA via /toggle (so that g_server is valid).
 */
void register_webpage_routes(AsyncWebServer& server) {
    g_server = &server;   // used by handlers that need to (re)start/stop the server (OTA)

    server.on("/",                   HTTP_GET,  handleRoot);
    server.on("/scan",               HTTP_GET,  handleScan);
    server.on("/login",              HTTP_POST, handleLogin);
    server.on("/connect",            HTTP_POST, handleConnect);
    server.on("/status",             HTTP_GET,  handleStatus);
    server.on("/toggle",             HTTP_POST, handleToggleFeature);
    server.on("/setBrightness",      HTTP_POST, handleSetBrightness);
    server.on("/configureWireGuard", HTTP_POST, handleConfigureWireGuard);
    server.on("/configureMQTT",      HTTP_POST, handleConfigureMQTT);
}