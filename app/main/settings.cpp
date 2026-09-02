/**
 * @file Settings.cpp
 * @brief Implementation of functions for loading and saving configuration on the ESP32.
 */

#include <Arduino.h>

#include "settings.h"
#include <FS.h>
#include <LittleFS.h>
#include <string.h>
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include <lvgl.h>   // only for the post-write repaint below

#include "commands.h"
#include "main.h"

//------------------------[uuid logger]-----------------------------------
static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};
//------------------------------------------------------------------------

/**
 * @brief Loads the configuration from a JSON file and stores the values in a Config object.
 * 
 * This function opens the JSON file, deserializes the data, and transfers it to the 
 * appropriate structure for further use. In case of failure, default values are used.
 * 
 * @param filename The name of the file from which the configuration is loaded.
 * @param config Reference to the Config object where the loaded configuration values are stored.
 */
void SETTINGS::loadConfiguration(const char* filename, Config &config) {
    JsonDocument doc;

    // Open file for reading
    File file = LittleFS.open(filename, FILE_READ);

    // Deserialize the JSON document
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
        Serial.println(F("Failed to read file, using default configuration"));
    }

    // Copy values from the JSON document to the Config object
    config.login_email      = doc["login_email"].as<String>();
    config.login_password   = doc["login_password"].as<String>();
    config.wifi_bssid       = doc["wifi_bssid"].as<String>();
    config.wifi_password    = doc["wifi_password"].as<String>();
    config.timezone         = doc["timezone"];
    config.ota_update       = doc["ota_update"];
    config.wg_mode          = doc["wg_mode"];
    config.mqtt_mode        = doc["mqtt_mode"];
    config.mqtt_master_mode = doc["mqtt_master_mode"];
    config.brightness       = doc["brightness"];
    config.telnet_port      = doc["telnet_port"];
    config.mqttServer       = doc["mqttServer"].as<String>();
    // "| existing value" keeps the struct default when the key is missing or not
    // convertible, instead of silently zeroing it. Without this, a /config.json
    // without "mqtt_port" left the port at 0: MQTT itself still worked (it uses
    // mqtt.mqtt_port), but the FSM's broker probe connected to port 0, always
    // failed, and declared the WireGuard tunnel sick until it rebooted the
    // device every 5 minutes. Every other field here has the same weakness.
    config.mqtt_port        = doc["mqtt_port"] | config.mqtt_port;
    config.mqttUsername     = doc["mqttUsername"].as<String>();
    config.mqttPassword     = doc["mqttPassword"].as<String>();
    config.wgPrivateKey     = doc["wgPrivateKey"].as<String>();
    config.wgPublicKey      = doc["wgPublicKey"].as<String>();
    config.wgPresharedKey   = doc["wgPresharedKey"].as<String>();
    config.wgIpAddress      = doc["wgIpAddress"].as<String>();
    config.wgEndpoint       = doc["wgEndpoint"].as<String>();
    config.wgEndpointPort   = doc["wgEndpointPort"];
    config.wgAllowedIPs     = doc["wgAllowedIPs"].as<String>();
    config.ha_discovery           = doc["ha_discovery"]           | (uint8_t)1;
    config.ota_staging            = doc["ota_staging"]            | (uint8_t)0;
    config.ota_force              = doc["ota_force"]              | (uint8_t)0;
    config.display_dim_timeout_s  = doc["display_dim_timeout_s"]  | (uint32_t)300;
    config.auto_brightness        = doc["auto_brightness"]        | (uint8_t)0;

    // Load wifi_networks array; migrate legacy fields if missing
    config.wifi_networks.clear();
    if (doc["wifi_networks"].is<JsonArray>()) {
        for (JsonObject net : doc["wifi_networks"].as<JsonArray>()) {
            if (config.wifi_networks.size() >= WIFI_NETWORKS_MAX) break;
            SETTINGS::WifiNetwork n;
            n.ssid     = net["ssid"].as<String>();
            n.password = net["password"].as<String>();
            if (n.ssid.length() > 0)
                config.wifi_networks.push_back(n);
        }
    }
    // Migration: if no wifi_networks entry yet, carry over legacy fields
    if (config.wifi_networks.empty() && config.wifi_bssid.length() > 0) {
        SETTINGS::WifiNetwork n;
        n.ssid     = config.wifi_bssid;
        n.password = config.wifi_password;
        config.wifi_networks.push_back(n);
    }

    file.close();
    doc.clear();
    
}

/**
 * @brief Saves the current configuration to a JSON file.
 * 
 * This function serializes the data from the Config object into a JSON file. 
 * If an error occurs, an appropriate error message is displayed.
 * 
 * @param filename The name of the file where the configuration is saved.
 * @param config The Config object containing the configuration data to be saved.
 */
void SETTINGS::saveConfiguration(const char *filename, Config &config) {
    JsonDocument doc;
    
    // Delete existing file to prevent appending
    LittleFS.remove(filename);

    // Open file for writing
    File file = LittleFS.open(filename, FILE_WRITE);
    if (!file) {
        Serial.println(F("Failed to create file"));
        return;
    }

    // Set values in the document
    doc["login_email"]      = config.login_email.c_str();
    doc["login_password"]   = config.login_password.c_str();
    doc["wifi_bssid"]       = config.wifi_bssid.c_str();
    doc["wifi_password"]    = config.wifi_password.c_str();
    doc["timezone"]         = config.timezone;
    doc["ota_update"]       = config.ota_update;
    doc["wg_mode"]          = config.wg_mode;
    doc["mqtt_mode"]        = config.mqtt_mode;
    doc["mqtt_master_mode"] = config.mqtt_master_mode;
    doc["brightness"]       = config.brightness;
    doc["telnet_port"]      = config.telnet_port;
    doc["mqttServer"]       = config.mqttServer.c_str();
    doc["mqtt_port"]        = config.mqtt_port;
    doc["mqttUsername"]     = config.mqttUsername.c_str();
    doc["mqttPassword"]     = config.mqttPassword.c_str();
    doc["wgPrivateKey"]     = config.wgPrivateKey.c_str();
    doc["wgPublicKey"]      = config.wgPublicKey.c_str();
    doc["wgPresharedKey"]   = config.wgPresharedKey.c_str();
    doc["wgIpAddress"]      = config.wgIpAddress.c_str();
    doc["wgEndpoint"]       = config.wgEndpoint.c_str();
    doc["wgEndpointPort"]   = config.wgEndpointPort;
    doc["wgAllowedIPs"]     = config.wgAllowedIPs.c_str();
    doc["ha_discovery"]          = config.ha_discovery;
    doc["ota_staging"]           = config.ota_staging;
    doc["ota_force"]             = config.ota_force;
    doc["display_dim_timeout_s"] = config.display_dim_timeout_s;
    doc["auto_brightness"]       = config.auto_brightness;

    // Save wifi_networks array; keep legacy fields in sync with first entry
    JsonArray nets = doc["wifi_networks"].to<JsonArray>();
    for (const auto& n : config.wifi_networks) {
        JsonObject o = nets.add<JsonObject>();
        o["ssid"]     = n.ssid.c_str();
        o["password"] = n.password.c_str();
    }
    if (!config.wifi_networks.empty()) {
        doc["wifi_bssid"]    = config.wifi_networks[0].ssid.c_str();
        doc["wifi_password"] = config.wifi_networks[0].password.c_str();
    }

    // Serialize JSON to file
    if (serializeJson(doc, file) == 0) {
        Serial.println(F("Failed to write to file"));
    }

    file.close();
    doc.clear();

    // Writing to LittleFS disables the flash cache, which starves the RGB
    // panel's DMA and leaves stripes on screen until something repaints. Doing
    // it here rather than at the ~10 call sites keeps every config write
    // covered, including the ones reached from the web API and MQTT.
    lv_obj_t *active_screen = lv_screen_active();
    if (active_screen != NULL)
        lv_obj_invalidate(active_screen);
}
