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
#include <freertos/portmacro.h>

#include "lvgl.h"
#include "ui.h"
#include "webpage.h"
#include <uuid/log.h>

extern volatile bool     ota_in_progress;
extern bool              g_ap_mode;
extern AsyncWebServer    server;

static uuid::log::Logger logger{F(__FILE__), uuid::log::Facility::CONSOLE};

static uint32_t ota_progress_millis = 0;

// OTA callbacks run on the AsyncTCP task. They only queue state here; LVGL
// is touched later by ota_ui_poll() from the Arduino loop task.
static portMUX_TYPE ota_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static bool ota_ui_start_pending = false;
static int ota_ui_progress_pending = -1;
static bool ota_ui_info_pending = false;
static bool ota_ui_finish_pending = false;
static bool ota_ui_finish_success = false;
static char ota_ui_info_text[128] = {};

void ota_ui_request_start()
{
    portENTER_CRITICAL(&ota_ui_mux);
    ota_ui_start_pending = true;
    ota_ui_progress_pending = 0;
    portEXIT_CRITICAL(&ota_ui_mux);
}

void ota_ui_request_progress(int progress)
{
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;

    portENTER_CRITICAL(&ota_ui_mux);
    ota_ui_progress_pending = progress;
    portEXIT_CRITICAL(&ota_ui_mux);
}

void ota_ui_request_info(const char *text)
{
    portENTER_CRITICAL(&ota_ui_mux);
    strlcpy(ota_ui_info_text, text ? text : "", sizeof(ota_ui_info_text));
    ota_ui_info_pending = true;
    portEXIT_CRITICAL(&ota_ui_mux);
}

void ota_ui_request_finish(bool success)
{
    portENTER_CRITICAL(&ota_ui_mux);
    ota_ui_finish_success = success;
    ota_ui_finish_pending = true;
    portEXIT_CRITICAL(&ota_ui_mux);
}

void ota_ui_poll()
{
    bool start_pending;
    int progress_pending;
    bool info_pending;
    bool finish_pending;
    bool finish_success;
    char info_text[sizeof(ota_ui_info_text)];

    portENTER_CRITICAL(&ota_ui_mux);
    start_pending = ota_ui_start_pending;
    progress_pending = ota_ui_progress_pending;
    info_pending = ota_ui_info_pending;
    finish_pending = ota_ui_finish_pending;
    finish_success = ota_ui_finish_success;
    strlcpy(info_text, ota_ui_info_text, sizeof(info_text));
    ota_ui_start_pending = false;
    ota_ui_progress_pending = -1;
    ota_ui_info_pending = false;
    ota_ui_finish_pending = false;
    portEXIT_CRITICAL(&ota_ui_mux);

    if (start_pending)
    {
        lv_disp_load_scr(ui_FWUpdate_screen);
        lv_label_set_text(ui_Label_FWUpdateInfo, "Firmware Update in progress...");
        lv_label_set_text(ui_Label_FWUpdateProgress_percent, "0%");
    }

    if (info_pending)
        lv_label_set_text(ui_Label_FWUpdateInfo, info_text);

    if (progress_pending >= 0)
    {
        char progress_text[8];
        snprintf(progress_text, sizeof(progress_text), "%d%%", progress_pending);
        lv_label_set_text(ui_Label_FWUpdateProgress_percent, progress_text);
    }

    if (finish_pending && !finish_success)
        lv_disp_load_scr(ui_Main_screen);
}

// Used by the main-task HTTPUpdate path before it blocks for the download.
void ota_ui_render_now()
{
    ota_ui_poll();
    lv_timer_handler();
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
    ota_ui_request_progress(progress);
    return 1;
}

void onOTAStart()
{
    Serial.println("OTA update started!");
    logger.notice("OTA Update Progress has started");

    ota_in_progress = true;
    ota_ui_request_start();
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
    ota_ui_request_finish(success);
    if (success)
    {
        ota_ui_request_progress(100);
        ota_ui_request_info("FWUpdate successful!\n\nperforming Reset");
    }
    ota_in_progress = false;
}
