#ifndef HTTP_UPDATE_MODULE_H
#define HTTP_UPDATE_MODULE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

/**
 * @brief Initialize firmware update module internals.
 */
void fw_update_init();

/**
 * @brief Legacy API for old dedicated FW task mode.
 *
 * FW update processing now runs via fw_update_poll() from LoopTask.
 */
void fw_update_start_task(BaseType_t core_id = 1, UBaseType_t priority = 1);

/**
 * @brief Process one firmware update scheduler tick (non-blocking unless check/install runs).
 *
 * Call this regularly from an existing background loop.
 */
void fw_update_poll();

/**
 * @brief Returns firmware update status as JSON.
 */
String fw_update_get_status_json();

/**
 * @brief Requests an immediate firmware update check.
 */
void fw_update_request_check_now();

/**
 * @brief Requests firmware install of the latest available update.
 *
 * @param[out] message Status text for caller/UI
 * @return true if install request was accepted
 */
bool fw_update_request_install(String& message);

/**
 * @brief Marks that the first valid glucose reading has been rendered on LVGL.
 *
 * This unlocks the automatic firmware manifest check once.
 */
void fw_update_mark_first_glucose_rendered();

#endif // HTTP_UPDATE_MODULE_H
