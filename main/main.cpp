/*
 * ====================================================================
 *  main.cpp -- ESP32-S3 Cellular Hub (Quectel EC200U-CN via esp_modem
 *              UART PPPoS)
 * ====================================================================
 *
 * Migrated from the A7672S (SIMCom) USB-Modem project, then re-migrated
 * from a USB-Host PPP design (iot_usbh_modem) to UART PPPoS (esp_modem)
 * once the EC200U-CN was wired over its UART header instead of USB --
 * see docs/WIRING.md and docs/MIGRATION_GUIDE.md for why.
 *
 * esp_modem flow (see modem.cpp, mirrors esp_modem's own pppos_client
 * example):
 *   esp_modem_new()               -- generic DCE over UART
 *   esp_modem_set_mode(DATA)      -- dial PPP
 *   wait for IP_EVENT_PPP_GOT_IP  -- ppp_manager.cpp
 *   esp_modem_set_mode(COMMAND)   -- hang up / recover
 * ====================================================================
 */

#include <cstdio>
#include <cstring>
#include <cstdarg>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"

#include "config.h"
#include "modem.h"
#include "ppp_manager.h"
#include "web_server.h"
#include "ota_manager.h"

static const char *TAG = "APP";

#define AP_NAME "IOT-HUB-AP"

static char s_fw[32] = "?";
static char s_ip[20] = "-";
static char s_op[32] = "-";
static int  s_rssi = 0;

static void push_log(const char *fmt, ...)
{
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ESP_LOGI(TAG, "%s", msg);

    char ev[256];
    snprintf(ev, sizeof(ev), "{\"type\":\"log\",\"msg\":\"%s\"}", msg);
    web_server_push_event(ev);
}

static void refresh_ppp_ip(void)
{
    esp_netif_t *ppp = modem_get_netif();
    if (ppp == NULL) {
        ppp = esp_netif_get_handle_from_ifkey("PPP_DEF");
    }
    esp_netif_ip_info_t info;
    if (ppp && esp_netif_get_ip_info(ppp, &info) == ESP_OK) {
        esp_ip4addr_ntoa(&info.ip, s_ip, sizeof(s_ip));
    } else {
        strncpy(s_ip, "-", sizeof(s_ip) - 1);
    }
}

static bool ppp_bring_up(void)
{
    for (int attempt = 1; attempt <= MAX_PPP_RETRIES; attempt++) {
        push_log("PPP: attempt %d/%d (APN=%s)", attempt, MAX_PPP_RETRIES,
                 CONFIG_MODEM_APN);

        if (!modem_is_installed()) {
            if (modem_install(CONFIG_MODEM_APN) != ESP_OK) {
                push_log("PPP: modem_install failed (UART wiring/baud?)");
                goto backoff;
            }
        }

        if (modem_ppp_start() != ESP_OK) {
            push_log("PPP: modem_ppp_start (DATA mode) failed");
            goto backoff;
        }

        web_server_set_status(s_fw, AP_NAME, 1, "-", s_op, s_rssi);
        push_log("PPP: dialing -- waiting for GOT_IP");

        if (ppp_manager_connect() == ESP_OK) {
            return true;
        }

        push_log("PPP: GOT_IP timeout -- restoring command mode");
        modem_ppp_stop();

backoff:
        if (attempt < MAX_PPP_RETRIES) {
            int delay_ms = RETRY_BASE_DELAY_MS << (attempt - 1);
            if (delay_ms > RETRY_MAX_DELAY_MS) {
                delay_ms = RETRY_MAX_DELAY_MS;
            }
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    return false;
}

extern "C" void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    strncpy(s_fw, app->version, sizeof(s_fw) - 1);
    s_fw[sizeof(s_fw) - 1] = '\0';

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  ESP32-S3 Cellular Hub (esp_modem) fw=%s", s_fw);
    ESP_LOGI(TAG, "  Modem: Quectel EC200U-CN (Vanix TracX-1b carrier)");
    ESP_LOGI(TAG, "  Transport: UART PPPoS via esp_modem_* APIs");
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, ">>> OTA-TEST-MARKER: this firmware was installed via OTA update <<<");
    ESP_LOGI(TAG, ">>> BUILD MARKER: %s -- if you see this, the OTA update worked <<<", s_fw);

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(web_server_start());
    ota_manager_init();
    ESP_ERROR_CHECK(ppp_manager_init_events());

    web_server_set_status(s_fw, AP_NAME, 1, "-", "-", 0);
    push_log("SYS: booted -- starting esp_modem (Quectel EC200U-CN)");

    bool online = false;

    while (1) {
        if (!online) {
            online = ppp_bring_up();
            if (!online) {
                web_server_set_status(s_fw, AP_NAME, 0, "-", s_op, s_rssi);
                push_log("PPP: all attempts failed -- resetting modem (AT+CFUN=1,1)");
                modem_force_reset();
                modem_uninstall();
                push_log("PPP: hard power-cycling modem via PWRKEY (~8s)");
                modem_hard_power_cycle();
                web_server_set_status(s_fw, AP_NAME, 1, "-", s_op, s_rssi);
                push_log("PPP: all attempts failed -- retry in 60 s");
                for (int w = 0; w < 60; w++) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    if (web_server_retry_requested()) {
                        push_log("PPP: manual retry");
                        break;
                    }
                }
                continue;
            }

            refresh_ppp_ip();
            esp_netif_t *nif = modem_get_netif();
            if (nif) {
                esp_netif_set_default_netif(nif);
            }
            web_server_set_status(s_fw, AP_NAME, 2, s_ip, s_op, s_rssi);
            push_log("PPP: ONLINE IP=%s op=%s rssi=%d", s_ip, s_op, s_rssi);

            if (ppp_manager_verify_connectivity(PPP_TEST_HOST, PPP_TEST_PORT) == ESP_OK) {
                push_log("PPP: internet OK -- OTA ready");
            } else {
                push_log("PPP: internet self-test FAILED");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));

        if (web_server_retry_requested()) {
            push_log("PPP: reconnect from Web UI");
            modem_uninstall();
            online = false;
            web_server_set_status(s_fw, AP_NAME, 1, "-", s_op, s_rssi);
            continue;
        }

        if (ota_manager_get_state() == OTA_STATE_RUNNING) {
            continue;
        }

        if (!ppp_manager_is_connected()) {
            push_log("PPP: link lost -- recovering");
            modem_uninstall();
            online = false;
            web_server_set_status(s_fw, AP_NAME, 1, "-", s_op, s_rssi);
        }
    }
}
