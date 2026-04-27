#ifndef HTTP_UPDATE_MODULE_H
#define HTTP_UPDATE_MODULE_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

/**
 * @brief Initialize firmware update module internals.
 */
void fw_update_init();

/**
 * @brief Legacy API from task-based mode (kept for compatibility).
 *
 * FW updates are now processed via fw_update_poll().
 */
void fw_update_start_task(BaseType_t core_id = 1, UBaseType_t priority = 1);

/**
 * @brief Processes one scheduler tick of firmware updates.
 *
 * Call regularly from an existing loop/FSM context.
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
 * @brief Returns the current status string (e.g. "idle", "up_to_date", "error").
 */
const char* fw_update_get_status();

/**
 * @brief Returns the latest available firmware version string from the manifest.
 */
const char* fw_update_get_latest_version();

/**
 * @brief Returns the current firmware version string (compile-time constant).
 */
const char* fw_update_get_current_version();

/**
 * @brief Returns true if a firmware update is available.
 */
bool fw_update_is_update_available();

/**
 * @brief Returns the pending operation type for FSM integration.
 * @return 0=none, 1=check due, 2=install due
 */
int fw_update_op_pending();

#endif // HTTP_UPDATE_MODULE_H
