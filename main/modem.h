#pragma once

/**
 * @file modem.h
 * @brief Wrapper around Espressif esp_modem (UART PPPoS) for the Quectel
 *        EC200U-CN
 *
 * Maps to official esp_modem C API (esp_modem_api.h):
 *   esp_modem_new() -- generic 3GPP DCE (EC200U-CN isn't in esp_modem's
 *                       named-device enum, so we use the generic profile,
 *                       which the datasheet's own AT-compliance claim
 *                       ("3GPP TS 27.007/27.005 + Quectel enhanced AT")
 *                       supports)
 *   esp_modem_set_mode(..., ESP_MODEM_MODE_DATA / ESP_MODEM_MODE_COMMAND)
 *   esp_modem_get_signal_quality() / esp_modem_get_operator_name()
 *   esp_modem_destroy()
 */

#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the UART DTE + generic DCE and the PPP esp_netif.
 * @param apn Carrier APN (e.g. airtelgprs.com). Applied via PDP config when non-empty.
 */
esp_err_t modem_install(const char *apn);

/** Destroy the DCE + PPP esp_netif. */
esp_err_t modem_uninstall(void);

/** True after successful modem_install(). */
bool modem_is_installed(void);

esp_netif_t *modem_get_netif(void);

/** Switch the DCE into DATA mode -- dials PPP (ATD*99#-style) and starts
 *  the PPP negotiation. Do not call while already in DATA mode. */
esp_err_t modem_ppp_start(void);

/** Switch the DCE back into COMMAND mode (hangs up PPP, restores AT access). */
esp_err_t modem_ppp_stop(void);

/** Best-effort operator / RSSI via AT+COPS?/AT+CSQ. Only valid while the
 *  DCE is in COMMAND mode. */
void modem_read_status(char *op_buf, size_t op_len, int *rssi_out);

/** Best-effort AT+CFUN=1,1 reboot of the modem's baseband. Deliberately
 *  sent as a raw AT command (esp_modem_at()) rather than esp_modem's own
 *  esp_modem_reset() -- that generic command sends SIMCom-specific
 *  AT+CRESET and waits for a SIMCom-only "PB DONE" URC, which a Quectel
 *  module will never emit. Call only while in COMMAND mode. */
esp_err_t modem_force_reset(void);

/** Hardware power-cycle via the PWRKEY line (requires GPIO4 wired to the
 *  carrier board's PWRKEY pin). Unlike modem_force_reset(), this works
 *  even when the modem's UART/AT stack itself is unresponsive, since
 *  PWRKEY is serviced by the module's power-management IC, not its
 *  baseband firmware. Blocks for several seconds (datasheet timing). */
esp_err_t modem_hard_power_cycle(void);

/** Hard reset via the RST line (requires GPIO5 wired to the carrier
 *  board's RST pin). This is NOT a graceful shutdown -- no network
 *  deregistration happens, unlike PWRKEY power-off or AT+QPOWD. Reserve
 *  it for a modem that does not respond to modem_hard_power_cycle()
 *  either. */
esp_err_t modem_hard_reset(void);

#ifdef __cplusplus
}
#endif
