#pragma once

#include <Arduino.h>
#include <PubSubClient.h>

void mqtt_publish();
void update_mqtt_publish();
void mqtt_callback(char *topic, byte *payload, unsigned int length);
bool setup_mqtt();
