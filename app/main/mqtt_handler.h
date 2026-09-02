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

/** Publish Home Assistant MQTT auto-discovery payloads (retained).
 *  Skipped when settings.config.ha_discovery == 0. */
void mqtt_publish_ha_discovery();

/** Publish one Zigbee device's state (motion, battery) to its own topic.
 *  Called straight from the H2 message handler so motion is reported when it
 *  happens, not on the next minute-long publish cycle. */
void mqtt_publish_zigbee_device(uint16_t addr);

/** Announces Zigbee devices that have turned up since the last discovery
 *  run. Needed because discovery fires on MQTT connect, before the H2 has
 *  reported its device list. */
void mqtt_sync_zigbee_entities();

/** Remove a Zigbee device's Home Assistant entities (empty retained config).
 *  Without this, forgotten devices linger in HA forever. */
void mqtt_remove_zigbee_device(uint16_t addr);
