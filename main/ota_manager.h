#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_RUNNING,
    OTA_STATE_DONE,
    OTA_STATE_FAILED,
} ota_state_t;

/**
 * @brief Initialise the OTA manager (call once after NVS init).
 */
void ota_manager_init(void);

/**
 * @brief Start an OTA download+flash from @p url.
 *
 * If url ends with ".patch" -> delta OTA via esp_delta_ota.
 * Otherwise              -> full binary OTA via esp_https_ota.
 *
 * The operation runs in a background FreeRTOS task.
 * Progress and log messages are sent over SSE (web_server).
 *
 * @return ESP_OK if task was started, ESP_ERR_INVALID_STATE if already running.
 */
esp_err_t ota_manager_start(const char *url);

/**
 * @brief Return current OTA state.
 */
ota_state_t ota_manager_get_state(void);

#ifdef __cplusplus
}
#endif
