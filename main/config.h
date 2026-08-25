#pragma once

/**
 * @file config.h
 * @brief ESP32-S3 + Quectel EC200U-CN (Vanix TracX-1b carrier) via
 *        Espressif esp_modem (UART PPPoS)
 *
 * Wiring (see ../../docs/WIRING.md for the full walkthrough):
 *   TracX-1b Tx  -> ESP32-S3 UART Rx  (MODEM_UART_RX_GPIO)
 *   TracX-1b Rx  -> ESP32-S3 UART Tx  (MODEM_UART_TX_GPIO)
 *   TracX-1b GND -> ESP32-S3 GND
 *   No MAIN_RTS/MAIN_CTS wiring: the EC200U-CN chip supports HW flow
 *   control, but this carrier board's Communications Pins header does
 *   not expose those two pins (datasheet Table 3 lists only Tx/Rx/GND
 *   under UART Pins) -- flow control stays at None regardless of baud.
 *   Carrier board VBAT <- dedicated >= 2 A, 3.7-4 V supply (jumper on
 *                          "BAT"), independent of the ESP32-S3 entirely
 *   Optional PWRKEY on GPIO4, optional RST on GPIO5
 *
 * Refs:
 *   esp_modem component: managed_components/espressif__esp_modem
 *   esp_modem's own examples/pppos_client (canonical UART PPPoS reference)
 *   Vanix TracX-1b / Quectel EC200U-CN User Manual (Ec200u module datasheet)
 */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- */
/*  UART wiring to the modem                                             */
/* -------------------------------------------------------------------- */

/* Defaults below match esp_modem's own ESP_MODEM_DTE_DEFAULT_CONFIG() pin
 * assignment (tx=25,rx=26,rts=27,cts=23) purely as a starting point --
 * change these to whatever GPIOs you actually wire on your ESP32-S3 board.
 * All are also exposed via Kconfig (menuconfig -> "Modem / Quectel
 * EC200U-CN UART Configuration") so they can be changed without editing
 * this file. */
#define MODEM_UART_PORT_NUM         CONFIG_MODEM_UART_PORT_NUM
#define MODEM_UART_TX_GPIO          CONFIG_MODEM_UART_TX_PIN
#define MODEM_UART_RX_GPIO          CONFIG_MODEM_UART_RX_PIN
#define MODEM_UART_RTS_GPIO         CONFIG_MODEM_UART_RTS_PIN
#define MODEM_UART_CTS_GPIO         CONFIG_MODEM_UART_CTS_PIN
#define MODEM_UART_BAUD_RATE        CONFIG_MODEM_UART_BAUD_RATE

/* Datasheet default is 115200 bps, no flow control. Raised to 230400 as
 * a conservative 2x step for OTA throughput -- HW flow control isn't
 * wireable on this carrier board (see above) and SW/XON-XOFF isn't safe
 * over PPP's raw binary data stream, so there is no flow-controlled path
 * to a higher baud here; this trades a little reliability margin for
 * speed instead. The module's own UART speed must be changed to match
 * via `AT+IPR=230400` + `AT&W` (one-time, persists in NVM) BEFORE
 * flashing firmware built with this baud -- otherwise the two ends
 * simply won't agree on a bit rate at all. */
#if defined(CONFIG_MODEM_FLOW_CONTROL_NONE)
#define MODEM_FLOW_CONTROL          ESP_MODEM_FLOW_CONTROL_NONE
#elif defined(CONFIG_MODEM_FLOW_CONTROL_HW)
#define MODEM_FLOW_CONTROL          ESP_MODEM_FLOW_CONTROL_HW
#endif

/* -------------------------------------------------------------------- */
/*  Power / reset control signals (unchanged by the UART-vs-USB choice)  */
/* -------------------------------------------------------------------- */

#define MODEM_PWRKEY_GPIO           4
#define MODEM_RST_GPIO              5
#define MODEM_BOOT_WAIT_MS          5000

/* EC200U-CN control-signal timing (Vanix TracX-1b / Quectel EC200U-CN
 * User Manual, section 4.3.1):
 *   - The module auto-powers-on as soon as VBAT/USB is applied -- no
 *     PWRKEY pulse is required on a cold power-up.
 *   - Power ON from power-down: PWRKEY low >= 2 s.
 *   - Power OFF: PWRKEY low >= 3 s (module then runs its own graceful
 *     power-down sequence after release). AT+QPOWD does the same thing
 *     in software and is preferred when the AT channel is reachable.
 *   - RST: low pulse >= 100 ms hard-resets the baseband. This is NOT a
 *     graceful shutdown (no detach/dereg) -- use PWRKEY/AT+QPOWD first
 *     and reserve RST for a genuinely wedged modem.
 * Values below add margin over each datasheet minimum. */
#define MODEM_PWRKEY_ON_PULSE_MS    2500
#define MODEM_PWRKEY_OFF_PULSE_MS   3200
#define MODEM_PWRKEY_SETTLE_MS      3000
#define MODEM_RST_PULSE_MS          150

#define PPP_CONNECT_TIMEOUT_MS      60000
#define MAX_PPP_RETRIES             5
#define PPP_POST_RESET_DELAY_MS     10000
#define PPP_TEST_HOST               "example.com"
#define PPP_TEST_PORT               80

#define RETRY_BASE_DELAY_MS         3000
#define RETRY_MAX_DELAY_MS          60000

#ifdef __cplusplus
}
#endif
