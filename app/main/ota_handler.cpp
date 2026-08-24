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
extern size_t g_internal_min_runtime; ///< see main.cpp
extern bool              g_ap_mode;
extern TaskHandle_t      LvglTaskHandle;
extern AsyncWebServer    server;
extern void              ui_blank_screen_for_reset();

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

static uint32_t ota_progress_millis = 0;

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
    if (ui_Arc_FWUpdate != NULL)
        lv_arc_set_value(ui_Arc_FWUpdate, progress);
    lv_timer_handler();
    delay(5);
    return 1;
}

void onOTAStart()
{
    Serial.println("OTA update started!");
    logger.notice("OTA Update Progress has started");

    ota_in_progress = 1;

    if (lv_screen_active() != ui_FWUpdate_screen)
    {
        lv_disp_load_scr(ui_FWUpdate_screen);
        // No "in progress" text: the ring and the percentage already say that.
        lv_label_set_text(ui_Label_FWUpdateInfo, "");
        lv_label_set_text(ui_Label_FWUpdateProgress_percent, "0%");
        if (ui_Arc_FWUpdate != NULL)
            lv_arc_set_value(ui_Arc_FWUpdate, 0);
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
            // Sample the internal heap here too. The low-water tracker in
            // main.cpp only runs per fetch, and there are no fetches during
            // OTA_MODE -- so it missed exactly the load case it exists for:
            // TLS download, web server, MQTT and display all at once, on top
            // of a continuous flash write.
            const size_t internal_free =
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (internal_free < g_internal_min_runtime)
                g_internal_min_runtime = internal_free;

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
        if (ui_Arc_FWUpdate != NULL)
            lv_arc_set_value(ui_Arc_FWUpdate, 100);
        if (ui_Label_FWUpdateTitle != NULL)
            lv_label_set_text(ui_Label_FWUpdateTitle, "Update finished");
        lv_task_handler();
        delay(255);
        // ElegantOTA.loop() restarts ~2s after this callback returns (see
        // ElegantOTAClass::loop() in the library) -- blank the panel now so
        // nothing is left on screen when that reset happens.
        ui_blank_screen_for_reset();
    }
}