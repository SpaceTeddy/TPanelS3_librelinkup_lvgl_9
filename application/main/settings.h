/**
 * @file settings.h
 * @brief Declaration of the SETTINGS class and configuration structure for managing ESP32 configuration settings.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <uuid/common.h>
#include <uuid/console.h>
#include <string>
#include <vector>
#include "commands.h"

/**
 * @class SETTINGS
 * @brief Manages configuration settings for the ESP32 device.
 * 
 * The SETTINGS class provides methods for loading and saving configuration data from/to a JSON file.
 * It also defines default settings such as Wi-Fi credentials and communication parameters.
 */
class SETTINGS {
    public:
        /**
         * @brief Default configuration file name for storing settings.
         */
        const char* config_filename = "/config.json";
        
        /**
         * @brief Default SSID for the access point if no known Wi-Fi network is available.
         */
        const char* apSSID = "llu";
        
        /**
         * @brief Default password for the access point.
         */
        const char* apPassword = "12345678";

        /**
         * @struct Config
         * @brief Structure to hold configuration settings.
         * 
         * This structure contains user credentials, network settings, and device configuration parameters.
         */
        struct Config {
            String login_email = "";          /**< User's login email */
            String login_password = "";       /**< User's login password */
            String wifi_bssid = "";           /**< Wi-Fi BSSID (MAC address of the access point) */
            String wifi_password = "";        /**< Wi-Fi password */
            int timezone = +1;                /**< Timezone offset in hours */
            uint8_t ota_update = 1;           /**< Flag to enable/disable OTA updates (1 = enabled, 0 = disabled) */
            uint8_t wg_mode = 0;              /**< Flag to enable/disable WireGuard VPN (1 = enabled, 0 = disabled) */
            uint8_t mqtt_mode = 1;            /**< Flag to enable/disable MQTT (1 = enabled, 0 = disabled) */
            uint8_t brightness = 50;          /**< Display brightness level (0-255) */
            uint16_t telnet_port = 23;        /**< Telnet port number */
            
            // MQTT Configuration
            String mqttServer = "";           /**< MQTT server address */
            uint16_t mqtt_port = 1883;        /**< MQTT port number */
            String mqttUsername = "";         /**< MQTT username */
            String mqttPassword = "";         /**< MQTT password */

            // WireGuard Configuration
            String wgPrivateKey = "";         /**< WireGuard private key */
            String wgPublicKey = "";          /**< WireGuard public key */
            String wgPresharedKey = "";       /**< WireGuard preshared key */
            String wgIpAddress = "";          /**< WireGuard IP address */
            String wgEndpoint = "";           /**< WireGuard endpoint (server address) */
            uint32_t wgEndpointPort = 0;      /**< WireGuard endpoint port */
            String wgAllowedIPs = "";         /**< WireGuard allowed IPs */

            uint64_t sleep_timer = 3600000;   /**< Sleep timer in milliseconds (default: 1 hour) */
        };

        /**
         * @brief Holds the current configuration settings.
         * 
         * This structure stores the configuration data loaded from a JSON file.
         */
        struct Config config;

        /**
         * @brief Loads configuration settings from a JSON file.
         * 
         * This method reads the JSON configuration file and stores the retrieved values in the `config` structure.
         * If the file is missing or unreadable, default values are used.
         * 
         * @param filename The name of the file from which the configuration will be loaded.
         * @param config Reference to the Config structure where the loaded settings are stored.
         */
        void loadConfiguration(const char* filename, Config &config);

        /**
         * @brief Saves the current configuration settings to a JSON file.
         * 
         * This method writes the values from the `config` structure into a JSON file, creating or overwriting it as needed.
         * 
         * @param filename The name of the file to which the configuration will be saved.
         * @param config The Config structure containing the settings to be saved.
         */
        void saveConfiguration(const char* filename, Config &config);
};

#endif // SETTINGS_H