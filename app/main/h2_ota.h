#pragma once
#include <Arduino.h>

/**
 * @brief Flash H2 firmware from a buffer (e.g. received via web upload).
 *
 * Takes ownership of @p data (must be heap_caps_malloc'd from PSRAM).
 * Frees it when the transfer completes. Streams the binary chunk by chunk
 * to the H2 via the UART JSON protocol (ota_start / ota_data / ota_end).
 *
 * @param data  PSRAM buffer containing the raw .bin image
 * @param size  Size of the image in bytes
 * @return true if the background task was started successfully
 */
bool h2_ota_start_from_buffer(uint8_t* data, size_t size);

/**
 * @brief Flash H2 firmware downloaded from a URL (HTTP or HTTPS).
 * @return true if the background task was started successfully
 */
bool h2_ota_start(const char* url);

/**
 * @brief Returns true while an H2 OTA transfer is running.
 */
bool h2_ota_in_progress();

/**
 * @brief Force-clears the in-progress flag (emergency recovery via Telnet).
 */
void h2_ota_force_clear();

/** @brief Bytes successfully written to H2 so far. */
size_t h2_ota_written();

/** @brief Total firmware size being transferred. */
size_t h2_ota_total();
