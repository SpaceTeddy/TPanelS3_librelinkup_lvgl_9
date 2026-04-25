/**
 * @file ota_handler.cpp
 * @brief OTA (Over-the-Air) firmware update support.
 *
 * Implements the ElegantOTA lifecycle callbacks (start, progress, end),
 * a FreeRTOS background task for periodic WiFi network scanning, and
 * the LVGL-based progress screen update helper.  The main entry point
 * setup_OTA() registers webpage routes, initialises ElegantOTA, and
 * starts the async web server.
 *
 * @author Chris
 * @license GPL 3.0
 */

#include "ota_handler.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ElegantOTA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lvgl.h"
#include "ui.h"
#include "webpage.h"
#include <uuid/log.h>

extern bool              ota_in_progress;
extern bool              g_ap_mode;
extern TaskHandle_t      LvglTaskHandle;
extern AsyncWebServer    server;

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

static volatile bool g_scan_in_progress = false;
static uint32_t      ota_progress_millis = 0;

String wifi_scan_now()
{
    if (ota_in_progress) return "[]";

    g_scan_in_progress = true;
    int n = WiFi.scanNetworks(false, false);

    String json;
    json.reserve(256);
    json = "[";
    bool first = true;
    for (int i = 0; i < n; ++i)
    {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"ssid\":\"";
        json += ssid;
        json += "\",\"rssi\":";
        json += String(WiFi.RSSI(i));
        json += "}";
    }
    json += "]";
    WiFi.scanDelete();
    g_scan_in_progress = false;
    return json;
}

void setup_OTA(bool mode)
{
    if (!mode) return;

    register_webpage_routes(server);
    ElegantOTA.begin(&server);
    ElegantOTA.onStart(onOTAStart);
    ElegantOTA.onProgress(onOTAProgress);
    ElegantOTA.onEnd(onOTAEnd);
    server.begin();
    Serial.println("HTTP server started");
}

uint8_t update_ota_progress_screen(int progress)
{
    char progress_text[10];
    snprintf(progress_text, sizeof(progress_text), "%d%%", progress);
    lv_label_set_text(ui_Label_FWUpdateProgress_percent, progress_text);
    lv_timer_handler();
    delay(5);
    return 1;
}

void onOTAStart()
{
    Serial.println("OTA update started!");
    logger.notice("OTA Update Progress has started");

    const uint32_t t0 = millis();
    while (g_scan_in_progress && (millis() - t0) < 2000)
        vTaskDelay(pdMS_TO_TICKS(50));

    ota_in_progress = 1;

    if (lv_screen_active() != ui_FWUpdate_screen)
    {
        lv_disp_load_scr(ui_FWUpdate_screen);
        lv_label_set_text(ui_Label_FWUpdateInfo, "Firmware Update in progress...");
        lv_timer_handler();
    }
}

void onOTAProgress(size_t current, size_t final)
{
    if (millis() - ota_progress_millis > 1000)
    {
        ota_progress_millis = millis();
        const float progress = ((float)current / (float)final) * 100.0f;
        if (ota_in_progress == 1)
        {
            update_ota_progress_screen((int)progress);
            logger.notice("FWUpdate Progress: %.2f%% (%d / %d Bytes)", progress, current, final);
        }
    }
}

void onOTAEnd(bool success)
{
    vTaskResume(LvglTaskHandle);
    if (!success)
    {
        lv_disp_load_scr(ui_Main_screen);
        lv_timer_handler();
        delay(5);
        ota_in_progress = 0;
    }
    else
    {
        ota_in_progress = 0;
        lv_label_set_text(ui_Label_FWUpdateProgress_percent, "100%");
        lv_label_set_text(ui_Label_FWUpdateInfo, "FWUpdate successful!\n\nperforming Reset");
        lv_task_handler();
        delay(255);
    }
}