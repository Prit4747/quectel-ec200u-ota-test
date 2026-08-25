#pragma once

/**
 * @file ppp_manager.h
 * @brief PPP IP / link event tracking for the esp_modem UART DCE
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ppp_manager_init_events(void);

/**
 * @brief Wait for IP_EVENT_PPP_GOT_IP after modem_ppp_start() has
 *        switched the DCE into DATA mode.
 *
 * Caller must call modem_ppp_start() first -- esp_modem never dials on
 * its own the way iot_usbh_modem's auto-connect did.
 */
esp_err_t ppp_manager_connect(void);

esp_err_t ppp_manager_disconnect(void);

bool ppp_manager_is_connected(void);

esp_err_t ppp_manager_verify_connectivity(const char *host, int port);

#ifdef __cplusplus
}
#endif
