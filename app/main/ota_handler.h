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

uint8_t update_ota_progress_screen(int progress);
void ota_ui_request_start();
void ota_ui_request_progress(int progress);
void ota_ui_request_info(const char *text);
void ota_ui_request_finish(bool success);
void ota_ui_poll();
void ota_ui_render_now();
void onOTAStart();
void onOTAProgress(size_t current, size_t final);
void onOTAEnd(bool success);
void setup_OTA(bool mode);
