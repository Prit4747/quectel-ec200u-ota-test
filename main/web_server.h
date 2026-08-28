#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the Wi-Fi softAP and HTTP server.
 *
 * Opens AP with SSID "IOT-HUB-AP" on 192.168.4.1.
 * Serves the web UI and REST endpoints (/status, /events, /ota/start, /modem/retry).
 */
esp_err_t web_server_start(void);

/**
 * @brief Push a JSON event string to all connected SSE clients.
 *
 * Safe to call from any task.
 * @param json_event  Null-terminated JSON string, e.g. {"type":"log","msg":"hello"}
 */
void web_server_push_event(const char *json_event);

/**
 * @brief Returns true (once) if the web UI requested a modem retry.
 */
bool web_server_retry_requested(void);

/**
 * @brief Returns true (once) if the web UI requested the one-shot UART
 *        baud-persist migration (see modem_set_uart_baud_persist()).
 */
bool web_server_baud_migrate_requested(void);

/**
 * @brief Update the status fields shown on the web UI.
 */
void web_server_set_status(const char *fw_version,
                           const char *ssid,
                           int  modem_state,   /* 0=offline,1=connecting,2=online */
                           const char *ip4,
                           const char *operator_name,
                           int  rssi);

#ifdef __cplusplus
}
#endif
