/**
 * @file mqtt.h
 * @brief MQTT class for managing MQTT connections and data handling.
 *
 * This class handles the MQTT connection setup, authentication,
 * and data transmission using the PubSubClient library.
 */

#ifndef MQTT_H
#define MQTT_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <StreamUtils.h>

/**
 * @def VERSION_MQTT_LIB
 * @brief Defines the version of the MQTT library.
 */
#define VERSION_MQTT_LIB 1.0

/**
 * @def DBGprint_MQTT
 * @brief Debug print macro for MQTT module.
 */
#define DBGprint_MQTT Serial.printf("[%08d][%s][%s] ", millis(), __FILE__, __func__);

/**
 * @def MQTT_DEBUG
 * @brief Enable or disable MQTT debugging.
 */
#define MQTT_DEBUG 0

/**
 * @class MQTT
 * @brief Manages MQTT communication, including connection and message handling.
 */
class MQTT {
private:
    
public:
    uint8_t debug_level = 1;  ///< Debugging level (default: 1)
    uint8_t mqtt_enable = 1;  ///< Enables or disables MQTT functionality

    /**
     * @brief MQTT server settings.
     */
    const char* mqtt_server = "192.168.0.202"; ///< MQTT broker IP address
    uint16_t mqtt_port = 1883; ///< MQTT broker port
    const char* mqtt_user = "mqtt"; ///< MQTT username
    const char* mqtt_password = "proxmox_mqtt"; ///< MQTT password

    /**
     * @brief MQTT buffer and topics.
     */
    char mqtt_buffer[255]; ///< Buffer for MQTT messages
    String mqtt_base = "librelinkup"; ///< Base MQTT topic
    String mqtt_client_data = "/data"; ///< Topic for client data
    String mqtt_client_name = ""; ///< Client name
    String mqtt_subscibe_toppic = "/cmd"; ///< Subscription topic for commands
    String mqtt_subscibe_rec_toppic = "/cmd_rec"; ///< Subscription topic for received commands
    String mqtt_client_network = "/network"; ///< Topic for network information
    String mqtt_incomming_cmd = ""; ///< Incoming MQTT command buffer

    bool configured = false; ///< Indicates if the MQTT client is configured

    /**
     * @brief Initializes the MQTT client.
     * @return Status of initialization (0 = failure, 1 = success)
     */
    uint8_t begin();

    /**
     * @brief Checks the status of the MQTT client connection.
     * @return Connection status (0 = disconnected, 1 = connected)
     */
    uint8_t check_client();
};

#endif // MQTT_H