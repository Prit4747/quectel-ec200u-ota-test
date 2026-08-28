/*
 * ota_manager.cpp -- Full .bin OTA and delta .patch OTA over HTTPS/HTTP.
 *
 * Unchanged from the A7672S project: this layer only talks to the PPP
 * netif's IP connectivity via esp_http_client/esp_https_ota, never to the
 * modem itself, so nothing here is Quectel/SIMCom-specific.
 *
 * Full OTA  : esp_https_ota
 * Delta OTA : espressif/esp_delta_ota (detools sequential + heatshrink patch)
 *
 * Patch file layout (Espressif https_delta_ota):
 *   [64 B header: magic + partition SHA256 + reserved] + [detools patch body]
 * The 64-byte header is verified then stripped; only the body goes to
 * esp_delta_ota_feed_patch().
 */

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cctype>
#include <cinttypes>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "esp_delta_ota.h"

#include "ota_manager.h"
#include "web_server.h"

static const char *TAG = "OTA";

#define OTA_HTTP_RX_BUF      4096
#define OTA_HTTP_TX_BUF      4096
#define PATCH_HEADER_SIZE    64
#define PATCH_MAGIC          0xfccdde10
#define PATCH_DIGEST_SIZE    32
#define IMG_HEADER_LEN       sizeof(esp_image_header_t)

static volatile ota_state_t s_state = OTA_STATE_IDLE;
static char s_url[512];
static ota_pre_reboot_hook_t s_pre_reboot_hook = nullptr;

/* ------------------------------------------------------------------ */

static void ota_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", buf);

    char event[300];
    snprintf(event, sizeof(event), "{\"type\":\"log\",\"msg\":\"OTA: %s\"}", buf);
    web_server_push_event(event);
}

static void ota_progress(int pct, const char *label)
{
    char event[128];
    snprintf(event, sizeof(event),
             "{\"type\":\"progress\",\"pct\":%d,\"label\":\"%s\"}", pct, label);
    web_server_push_event(event);
}

/* Reporting every 1% change pushes ~100 SSE sends (each a blocking socket
 * write over the WiFi AP) over the course of a single OTA transfer, all
 * from ota_task while it's also driving the HTTPS/TLS read loop -- that
 * competes for CPU with the modem's UART relay task right when it can
 * least afford it, contributing to the UART_FIFO_OVF ("HW FIFO Overflow")
 * warnings seen under sustained OTA load. Bucketing to 5% steps cuts that
 * by ~5x while still giving smooth-looking web UI progress. 100% is always
 * reported so the "Done" transition never gets skipped by the bucketing. */
#define OTA_PROGRESS_STEP_PCT 5

static bool progress_step_changed(int pct, int *last_reported)
{
    int bucket = (pct >= 100) ? 100 : (pct / OTA_PROGRESS_STEP_PCT) * OTA_PROGRESS_STEP_PCT;
    if (bucket == *last_reported) {
        return false;
    }
    *last_reported = bucket;
    return true;
}

static bool url_is_delta_patch(const char *url)
{
    if (!url) {
        return false;
    }
    const char *dot = strrchr(url, '.');
    if (!dot) {
        return false;
    }
    /* Ignore query string: foo.patch?token=abc */
    char ext[16];
    size_t n = 0;
    while (dot[n + 1] && dot[n + 1] != '?' && dot[n + 1] != '#' && n + 1 < sizeof(ext)) {
        ext[n] = (char)tolower((unsigned char)dot[n + 1]);
        n++;
    }
    ext[n] = '\0';
    return (strcmp(ext, "patch") == 0);
}

/* ------------------------------------------------------------------ */
/*  Full binary OTA                                                     */
/* ------------------------------------------------------------------ */

static esp_err_t run_full_ota(const char *url)
{
    ota_log("Full OTA starting: %s", url);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.timeout_ms = 30000;
    http_cfg.keep_alive_enable = true;
    http_cfg.buffer_size = OTA_HTTP_RX_BUF;
    http_cfg.buffer_size_tx = OTA_HTTP_TX_BUF;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;

    /* DNS/connect over cellular is flaky enough (EAI_FAIL from a dropped
     * DNS round-trip, transient PPP hiccups) that a single failed attempt
     * here shouldn't abort the whole OTA -- retry the connect a few times
     * before giving up. esp_https_ota_perform() failures further down are
     * not retried here; a corrupt/aborted image download is a different
     * failure class from "couldn't even open the connection". */
    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = ESP_FAIL;
    const int kConnectRetries = 3;
    for (int attempt = 1; attempt <= kConnectRetries; attempt++) {
        err = esp_https_ota_begin(&ota_cfg, &handle);
        if (err == ESP_OK) {
            break;
        }
        ota_log("esp_https_ota_begin failed (attempt %d/%d): %s",
                attempt, kConnectRetries, esp_err_to_name(err));
        if (attempt < kConnectRetries) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    esp_app_desc_t app_desc;
    if (esp_https_ota_get_img_desc(handle, &app_desc) == ESP_OK) {
        ota_log("New firmware: %s %s", app_desc.project_name, app_desc.version);
    }

    int last_pct = -1;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int image_len = esp_https_ota_get_image_len_read(handle);
        int total     = esp_https_ota_get_image_size(handle);
        if (total > 0) {
            int pct = (image_len * 100) / total;
            if (progress_step_changed(pct, &last_pct)) {
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "%d / %d KB", image_len / 1024, total / 1024);
                ota_progress(last_pct, lbl);
            }
        }
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ota_log("Incomplete data received");
        esp_https_ota_abort(handle);
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ota_log("esp_https_ota_finish: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ota_log("Image validation failed -- wrong chip or binary?");
        }
        return err;
    }

    ota_progress(100, "Done");
    ota_log("Full OTA complete -- rebooting");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Delta OTA                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    esp_ota_handle_t       ota;
    const esp_partition_t *src_part;
    uint8_t                img_hdr[IMG_HEADER_LEN];
    int                    img_hdr_len;
    bool                   chip_verified;
} delta_ctx_t;

static esp_err_t delta_write_cb(const uint8_t *buf, size_t len, void *user_data)
{
    delta_ctx_t *ctx = (delta_ctx_t *)user_data;
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = 0;

    if (!ctx->chip_verified) {
        int need = IMG_HEADER_LEN - ctx->img_hdr_len;
        int take = (int)len < need ? (int)len : need;

        memcpy(ctx->img_hdr + ctx->img_hdr_len, buf, (size_t)take);
        ctx->img_hdr_len += take;

        if (ctx->img_hdr_len < IMG_HEADER_LEN) {
            return ESP_OK;
        }

        esp_image_header_t *ih = (esp_image_header_t *)ctx->img_hdr;
        if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
            ota_log("Patch targets wrong chip (id=%d, expected=%d)",
                    ih->chip_id, CONFIG_IDF_FIRMWARE_CHIP_ID);
            return ESP_ERR_INVALID_VERSION;
        }
        ctx->chip_verified = true;

        esp_err_t err = esp_ota_write(ctx->ota, ctx->img_hdr, IMG_HEADER_LEN);
        if (err != ESP_OK) {
            return err;
        }
        index = take;
    }

    if (index < (int)len) {
        return esp_ota_write(ctx->ota, buf + index, len - (size_t)index);
    }
    return ESP_OK;
}

static esp_err_t delta_read_cb(uint8_t *buf, size_t size, int offset, void *user_data)
{
    delta_ctx_t *ctx = (delta_ctx_t *)user_data;
    return esp_partition_read(ctx->src_part, (size_t)offset, buf, size);
}

static bool verify_patch_header(const uint8_t *hdr)
{
    uint32_t magic;
    memcpy(&magic, hdr, sizeof(magic));
    if (magic != PATCH_MAGIC) {
        ota_log("Invalid patch magic 0x%08" PRIx32 " (need 0x%08x)", magic, PATCH_MAGIC);
        return false;
    }

    uint8_t part_sha[PATCH_DIGEST_SIZE] = {0};
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_partition_get_sha256(running, part_sha) != ESP_OK) {
        ota_log("Could not hash running partition");
        return false;
    }

    if (memcmp(part_sha, hdr + 4, PATCH_DIGEST_SIZE) != 0) {
        ota_log("Patch not for this firmware (partition SHA256 mismatch)");
        ota_log("Rebuild patch from the .bin currently on the device");
        return false;
    }

    ota_log("Patch header OK for running firmware (%s)", running->label);
    return true;
}

static esp_err_t http_open_following_redirects(esp_http_client_handle_t client,
                                               int *content_len_out)
{
    for (int hop = 0; hop < 5; hop++) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ota_log("http_client_open: %s", esp_err_to_name(err));
            return err;
        }

        int clen   = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 200) {
            *content_len_out = clen;
            return ESP_OK;
        }

        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            ota_log("HTTP %d -- following redirect", status);
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }

        ota_log("HTTP %d -- server refused the request", status);
        esp_http_client_close(client);
        return ESP_FAIL;
    }

    ota_log("Too many redirects");
    return ESP_FAIL;
}

static esp_err_t http_read_exact(esp_http_client_handle_t client,
                                 uint8_t *buf, int want)
{
    int got = 0;
    while (got < want) {
        int rd = esp_http_client_read(client, (char *)(buf + got), want - got);
        if (rd < 0) {
            ota_log("HTTP read error");
            return ESP_FAIL;
        }
        if (rd == 0) {
            ota_log("Connection closed early (%d/%d bytes)", got, want);
            return ESP_FAIL;
        }
        got += rd;
    }
    return ESP_OK;
}

static esp_err_t run_delta_ota(const char *url)
{
    ota_log("Delta OTA starting: %s", url);

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running_part = esp_ota_get_running_partition();
    if (!update_part || !running_part) {
        ota_log("No OTA partition available");
        return ESP_FAIL;
    }
    ota_log("Source: %s  Target: %s", running_part->label, update_part->label);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ota_log("esp_ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    delta_ctx_t ctx = {};
    ctx.ota = ota_handle;
    ctx.src_part = running_part;

    esp_delta_ota_cfg_t dcfg = {};
    dcfg.user_data = &ctx;
    dcfg.read_cb_with_user_data = delta_read_cb;
    dcfg.write_cb_with_user_data = delta_write_cb;
    esp_delta_ota_handle_t delta = esp_delta_ota_init(&dcfg);
    if (!delta) {
        ota_log("esp_delta_ota_init failed");
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = url;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.timeout_ms = 60000;
    http_cfg.buffer_size = OTA_HTTP_RX_BUF;
    http_cfg.buffer_size_tx = OTA_HTTP_TX_BUF;
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ota_log("http_client_init failed");
        esp_delta_ota_deinit(delta);
        esp_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    /* Declared up front (rather than at first use, as the original C did)
     * so that the `goto cleanup` jumps below don't cross any initialized
     * C++ automatic variable -- that's ill-formed in C++ even for POD
     * types with an initializer, unlike in C. */
    int content_len = 0;
    uint8_t patch_hdr[PATCH_HEADER_SIZE];
    int body_total = 0;
    int body_read  = 0;
    int last_pct   = -1;
    uint8_t buf[1024];

    /* DNS/connect over cellular is flaky enough (transient PPP hiccups,
     * a dropped TLS handshake) that a single failed attempt here shouldn't
     * abort the whole delta OTA -- retry the connect a few times before
     * giving up, matching run_full_ota()'s esp_https_ota_begin retry loop.
     * Without this, delta OTA had no cushion at all against exactly the
     * kind of transient connect failure full OTA already tolerated, which
     * is why full OTA could succeed and an immediately-following delta OTA
     * attempt could fail on what was likely the same class of hiccup.
     * esp_http_client_set_redirection() (inside http_open_following_redirects)
     * updates the client's URL in place, so retrying just calls open() again
     * against wherever the previous attempt left off -- no need to
     * re-resolve the original URL from scratch each time. */
    {
        const int kConnectRetries = 3;
        for (int attempt = 1; attempt <= kConnectRetries; attempt++) {
            err = http_open_following_redirects(client, &content_len);
            if (err == ESP_OK) {
                break;
            }
            ota_log("connect failed (attempt %d/%d)", attempt, kConnectRetries);
            if (attempt < kConnectRetries) {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
    }
    if (err != ESP_OK) {
        goto cleanup;
    }
    ota_log("Download size: %d bytes", content_len);

    err = http_read_exact(client, patch_hdr, PATCH_HEADER_SIZE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (!verify_patch_header(patch_hdr)) {
        err = ESP_ERR_INVALID_VERSION;
        goto cleanup;
    }

    body_total = (content_len > PATCH_HEADER_SIZE)
                 ? (content_len - PATCH_HEADER_SIZE) : 0;

    while (1) {
        int rd = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (rd < 0) {
            err = ESP_FAIL;
            ota_log("Read error");
            break;
        }
        if (rd == 0) {
            if (!esp_http_client_is_complete_data_received(client)) {
                err = ESP_FAIL;
                ota_log("Incomplete patch (%d body bytes)", body_read);
            }
            break;
        }

        body_read += rd;
        err = esp_delta_ota_feed_patch(delta, buf, rd);
        if (err != ESP_OK) {
            ota_log("esp_delta_ota_feed_patch: %s", esp_err_to_name(err));
            ota_log("Patch must be detools+heatshrink (see tools/make_patch.py)");
            break;
        }

        if (body_total > 0) {
            int pct = (body_read * 100) / body_total;
            if (progress_step_changed(pct, &last_pct)) {
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "%d / %d KB", body_read / 1024, body_total / 1024);
                ota_progress(last_pct, lbl);
            }
        }
    }

    if (err == ESP_OK) {
        err = esp_delta_ota_finalize(delta);
        if (err != ESP_OK) {
            ota_log("esp_delta_ota_finalize: %s", esp_err_to_name(err));
        }
    }

cleanup:
    esp_http_client_cleanup(client);
    esp_delta_ota_deinit(delta);

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        return err;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ota_log("esp_ota_end: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ota_log("esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return err;
    }

    ota_progress(100, "Done");
    ota_log("Delta OTA complete -- rebooting");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Task                                                                */
/* ------------------------------------------------------------------ */

static void ota_task(void *arg)
{
    (void)arg;
    bool is_delta = url_is_delta_patch(s_url);
    ota_log("Mode: %s", is_delta ? "delta (.patch)" : "full (.bin)");
    esp_err_t err = is_delta ? run_delta_ota(s_url) : run_full_ota(s_url);

    if (err == ESP_OK) {
        s_state = OTA_STATE_DONE;

        /* The modem has its own independent power supply and does not
         * reset alongside the ESP32 -- if left in DATA mode (still
         * PPP-dialed) across the reboot below, the fresh esp_modem session
         * on the other side can have its first AT probe swallowed/misread
         * as PPP payload, costing a full retry cycle before the modem is
         * usable again post-OTA. Best-effort: OTA has already succeeded at
         * this point regardless of whether this hook does anything useful. */
        if (s_pre_reboot_hook) {
            ota_log("Returning modem to command mode before reboot...");
            s_pre_reboot_hook();
        }

        web_server_push_event("{\"type\":\"reboot\",\"secs\":3}");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        ota_log("FAILED: %s", esp_err_to_name(err));
        s_state = OTA_STATE_FAILED;
        web_server_push_event("{\"type\":\"ota_failed\"}");
    }

    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void ota_manager_init(void)
{
    s_state = OTA_STATE_IDLE;
}

esp_err_t ota_manager_start(const char *url)
{
    if (s_state == OTA_STATE_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    s_state = OTA_STATE_RUNNING;

    /* Delta patching needs more stack than plain HTTPS OTA */
    if (xTaskCreate(ota_task, "ota_task", 12288, NULL, 5, NULL) != pdPASS) {
        s_state = OTA_STATE_IDLE;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

ota_state_t ota_manager_get_state(void)
{
    return s_state;
}

void ota_manager_set_pre_reboot_hook(ota_pre_reboot_hook_t hook)
{
    s_pre_reboot_hook = hook;
}
