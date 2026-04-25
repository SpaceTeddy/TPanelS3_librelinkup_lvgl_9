/**
 * @file ota_handler.h
 * @brief OTA (Over-the-Air) firmware update support.
 *
 * Declares the ElegantOTA callback hooks, the LVGL progress screen
 * update helper, the background WiFi network scan task, and the
 * top-level setup function that registers all routes and starts the
 * async web server.
 *
 * @author Chris
 * @license GPL 3.0
 */

#pragma once

#include <Arduino.h>

String wifi_scan_now();
uint8_t update_ota_progress_screen(int progress);
void onOTAStart();
void onOTAProgress(size_t current, size_t final);
void onOTAEnd(bool success);
void setup_OTA(bool mode);
