#ifndef HTTP_UPDATE_MODULE_H
#define HTTP_UPDATE_MODULE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

/**
 * @brief Initialize firmware update module internals.
 */
void fw_update_init();

/**
 * @brief Start firmware update worker task.
 *
 * @param core_id FreeRTOS core id (default: 1)
 * @param priority FreeRTOS task priority (default: 1)
 */
void fw_update_start_task(BaseType_t core_id = 1, UBaseType_t priority = 1);

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

#endif // HTTP_UPDATE_MODULE_H
