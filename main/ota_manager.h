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

/**
 * @brief Register a callback fired right before the post-OTA esp_restart().
 *
 * ota_manager never touches the modem/PPP transport directly (see the file
 * header comment in ota_manager.cpp) -- this hook is the extension point for
 * whatever transport layer is in use to do so. On this project it's used to
 * return the modem to COMMAND mode before the ESP32 reboots: the modem has
 * its own independent power supply and does NOT reset alongside the ESP32,
 * so if it's left in DATA mode (still PPP-dialed) at reboot, the freshly
 * booted esp_modem session's first AT probe can be swallowed/misread,
 * costing a full retry cycle before the modem is usable again.
 *
 * Optional: if never set, the pre-restart hook is simply skipped.
 */
typedef void (*ota_pre_reboot_hook_t)(void);
void ota_manager_set_pre_reboot_hook(ota_pre_reboot_hook_t hook);

#ifdef __cplusplus
}
#endif
