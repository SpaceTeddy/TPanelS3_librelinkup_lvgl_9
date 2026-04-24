/**
 * @file mqtt_handler.h
 * @brief MQTT client setup, publishing, and command handling.
 *
 * Declares functions for connecting to the MQTT broker, subscribing to
 * command and raw-data topics, publishing device state, and processing
 * incoming commands (brightness, OTA mode, WireGuard, resets).
 *
 * @author Chris
 * @license GPL 3.0
 */

#pragma once

#include <Arduino.h>
#include <PubSubClient.h>

void mqtt_publish();
void update_mqtt_publish();
void mqtt_callback(char *topic, byte *payload, unsigned int length);
bool setup_mqtt();
