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
    DynamicJsonDocument doc(1024);

    // Open file for reading
    File file = LittleFS.open(filename, FILE_READ);

    // Deserialize the JSON document
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
        Serial.println(F("Failed to read file, using default configuration"));
    }

    // Copy values from the JSON document to the Config object
    config.login_email    = doc["login_email"].as<String>();
    config.login_password = doc["login_password"].as<String>();
    config.wifi_bssid     = doc["wifi_bssid"].as<String>();
    config.wifi_password  = doc["wifi_password"].as<String>();
    config.timezone       = doc["timezone"];
    config.ota_update     = doc["ota_update"];
    config.wg_mode        = doc["wg_mode"];
    config.mqtt_mode      = doc["mqtt_mode"];
    config.brightness     = doc["brightness"];
    config.telnet_port    = doc["telnet_port"];
    config.mqttServer     = doc["mqttServer"].as<String>();
    config.mqtt_port      = doc["mqtt_port"];
    config.mqttUsername   = doc["mqttUsername"].as<String>();
    config.mqttPassword   = doc["mqttPassword"].as<String>();
    config.wgPrivateKey   = doc["wgPrivateKey"].as<String>();
    config.wgPublicKey    = doc["wgPublicKey"].as<String>();
    config.wgPresharedKey = doc["wgPresharedKey"].as<String>();
    config.wgIpAddress    = doc["wgIpAddress"].as<String>();
    config.wgEndpoint     = doc["wgEndpoint"].as<String>();
    config.wgEndpointPort = doc["wgEndpointPort"];
    config.wgAllowedIPs   = doc["wgAllowedIPs"].as<String>();
    config.sleep_timer    = doc["sleep_timer"];
    
    file.close();
    doc.clear();
    
    Serial.printf("load:{login_email:%s, login_password:%s, wifi_bssid:%s, wifi_password:%s, timezone:%d, ota_update:%d, wg_mode:%d, mqtt_mode:%d, brightness:%d, telnet_port:%d, mqttServer:%s, mqtt_port:%d, mqttUsername:%s, mqttPassword:%s, wgPrivateKey:%s, wgPublicKey:%s, wgPresharedKey:%s, wgIpAddress:%s, wgEndpoint:%s, wgEndpointPort:%d, wgAllowedIPs:%s, sleep_timer:%d}",
       config.login_email.c_str(),
       config.login_password.c_str(),
       config.wifi_bssid.c_str(),
       config.wifi_password.c_str(),
       config.timezone,
       config.ota_update,
       config.wg_mode,
       config.mqtt_mode,
       config.brightness,
       config.telnet_port,
       config.mqttServer.c_str(),
       config.mqtt_port,
       config.mqttUsername.c_str(),
       config.mqttPassword.c_str(),
       config.wgPrivateKey.c_str(),
       config.wgPublicKey.c_str(),
       config.wgPresharedKey.c_str(),
       config.wgIpAddress.c_str(),
       config.wgEndpoint.c_str(),
       config.wgEndpointPort,
       config.wgAllowedIPs.c_str(),
       config.sleep_timer);
    /*  
    logger.notice("load:{login_email:%s, login_password:%s, wifi_bssid:%s, wifi_password:%s, timezone:%d, ota_update:%d, wg_mode:%d, mqtt_mode:%d, brightness:%d, telnet_port:%d, mqttServer:%s, mqtt_port:%d, mqttUsername:%s, mqttPassword:%s, wgPrivateKey:%s, wgPublicKey:%s, wgPresharedKey:%s, wgIpAddress:%s, wgEndpoint:%s, wgEndpointPort:%d, wgAllowedIPs:%s, sleep_timer:%d}",
       config.login_email.c_str(),
       config.login_password.c_str(),
       config.wifi_bssid.c_str(),
       config.wifi_password.c_str(),
       config.timezone,
       config.ota_update,
       config.wg_mode,
       config.mqtt_mode,
       config.brightness,
       config.telnet_port,
       config.mqttServer.c_str(),
       config.mqtt_port,
       config.mqttUsername.c_str(),
       config.mqttPassword.c_str(),
       config.wgPrivateKey.c_str(),
       config.wgPublicKey.c_str(),
       config.wgPresharedKey.c_str(),
       config.wgIpAddress.c_str(),
       config.wgEndpoint.c_str(),
       config.wgEndpointPort,
       config.wgAllowedIPs.c_str(),
       config.sleep_timer);
    */
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
    DynamicJsonDocument doc(1024);
    
    // Delete existing file to prevent appending
    LittleFS.remove(filename);

    // Open file for writing
    File file = LittleFS.open(filename, FILE_WRITE);
    if (!file) {
        Serial.println(F("Failed to create file"));
        return;
    }

    // Set values in the document
    doc["login_email"]    = config.login_email.c_str();
    doc["login_password"] = config.login_password.c_str();
    doc["wifi_bssid"]     = config.wifi_bssid.c_str();
    doc["wifi_password"]  = config.wifi_password.c_str();
    doc["timezone"]       = config.timezone;
    doc["ota_update"]     = config.ota_update;
    doc["wg_mode"]        = config.wg_mode;
    doc["mqtt_mode"]      = config.mqtt_mode;
    doc["brightness"]     = config.brightness;
    doc["telnet_port"]    = config.telnet_port;
    doc["mqttServer"]     = config.mqttServer.c_str();
    doc["mqtt_port"]      = config.mqtt_port;
    doc["mqttUsername"]   = config.mqttUsername.c_str();
    doc["mqttPassword"]   = config.mqttPassword.c_str();
    doc["wgPrivateKey"]   = config.wgPrivateKey.c_str();
    doc["wgPublicKey"]    = config.wgPublicKey.c_str();
    doc["wgPresharedKey"] = config.wgPresharedKey.c_str();
    doc["wgIpAddress"]    = config.wgIpAddress.c_str();
    doc["wgEndpoint"]     = config.wgEndpoint.c_str();
    doc["wgEndpointPort"] = config.wgEndpointPort;
    doc["wgAllowedIPs"]   = config.wgAllowedIPs.c_str();
    doc["sleep_timer"]    = config.sleep_timer;

    // Serialize JSON to file
    if (serializeJson(doc, file) == 0) {
        Serial.println(F("Failed to write to file"));
    }

    file.close();
    doc.clear();

    Serial.printf("save:{login_email:%s, login_password:%s, wifi_bssid:%s, wifi_password:%s, timezone:%d, ota_update:%d, wg_mode:%d, mqtt_mode:%d, brightness:%d, telnet_port:%d, mqttServer:%s, mqtt_port:%d, mqttUsername:%s, mqttPassword:%s, wgPrivateKey:%s, wgPublicKey:%s, wgPresharedKey:%s, wgIpAddress:%s, wgEndpoint:%s, wgEndpointPort:%d, wgAllowedIPs:%s, sleep_timer:%d}",
       config.login_email.c_str(),
       config.login_password.c_str(),
       config.wifi_bssid.c_str(),
       config.wifi_password.c_str(),
       config.timezone,
       config.ota_update,
       config.wg_mode,
       config.mqtt_mode,
       config.brightness,
       config.telnet_port,
       config.mqttServer.c_str(),
       config.mqtt_port,
       config.mqttUsername.c_str(),
       config.mqttPassword.c_str(),
       config.wgPrivateKey.c_str(),
       config.wgPublicKey.c_str(),
       config.wgPresharedKey.c_str(),
       config.wgIpAddress.c_str(),
       config.wgEndpoint.c_str(),
       config.wgEndpointPort,
       config.wgAllowedIPs.c_str(),
       config.sleep_timer);

    /*
    logger.notice("save:{login_email:%s, login_password:%s, wifi_bssid:%s, wifi_password:%s, timezone:%d, ota_update:%d, wg_mode:%d, mqtt_mode:%d, brightness:%d, telnet_port:%d, mqttServer:%s, mqtt_port:%d, mqttUsername:%s, mqttPassword:%s, wgPrivateKey:%s, wgPublicKey:%s, wgPresharedKey:%s, wgIpAddress:%s, wgEndpoint:%s, wgEndpointPort:%d, wgAllowedIPs:%s, sleep_timer:%d}",
       config.login_email.c_str(),
       config.login_password.c_str(),
       config.wifi_bssid.c_str(),
       config.wifi_password.c_str(),
       config.timezone,
       config.ota_update,
       config.wg_mode,
       config.mqtt_mode,
       config.brightness,
       config.telnet_port,
       config.mqttServer.c_str(),
       config.mqtt_port,
       config.mqttUsername.c_str(),
       config.mqttPassword.c_str(),
       config.wgPrivateKey.c_str(),
       config.wgPublicKey.c_str(),
       config.wgPresharedKey.c_str(),
       config.wgIpAddress.c_str(),
       config.wgEndpoint.c_str(),
       config.wgEndpointPort,
       config.wgAllowedIPs.c_str(),
       config.sleep_timer);
    */
}
