#pragma once

#include <Arduino.h>

extern String availableNetworks;

void scanWiFiTask(void *parameter);
uint8_t update_ota_progress_screen(int progress);
void onOTAStart();
void onOTAProgress(size_t current, size_t final);
void onOTAEnd(bool success);
void setup_OTA(bool mode);
