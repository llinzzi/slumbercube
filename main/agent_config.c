#include "agent_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "AGENT_CFG";

/* Defaults — host matches the existing hardcoded RADIO_API_BASE in
 * audio_player_wrapper.c. enabled=false keeps the out-of-box behaviour
 * (clock + indoor temp only); the user opts in via the captive portal. */
#define AGENT_DEFAULT_HOST "192.168.8.192"
#define AGENT_NAMESPACE    "agent_cfg"
#define AGENT_KEY_HOST     "host"
#define AGENT_KEY_ENABLED  "enabled"

esp_err_t agent_config_load(agent_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    strncpy(out->host, AGENT_DEFAULT_HOST, sizeof(out->host) - 1);
    out->host[sizeof(out->host) - 1] = '\0';
    out->enabled = false;

    /* NVS is initialised centrally in app_main(). */

    nvs_handle_t handle;
    esp_err_t err = nvs_open(AGENT_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* First boot — defaults already filled in above. */
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open('%s') failed: %s, using defaults",
                 AGENT_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    size_t host_len = sizeof(out->host);
    err = nvs_get_str(handle, AGENT_KEY_HOST, out->host, &host_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace exists but host was never written — keep default. */
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_str(host) failed: %s, using defaults",
                 esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    uint8_t enabled = 0;
    err = nvs_get_u8(handle, AGENT_KEY_ENABLED, &enabled);
    nvs_close(handle);
    if (err != ESP_OK) {
        /* enabled key missing — host is fine, just keep enabled=false. */
        return ESP_OK;
    }
    out->enabled = (enabled == 1);

    /* Sanitise: enabled=true with an empty host is unrecoverable (the user
     * couldn't have submitted it; it must be left over from a bad write).
     * Coerce to disabled to keep the device in clock-only mode rather than
     * calling /api/esp with an empty host. */
    if (out->enabled && out->host[0] == '\0') {
        ESP_LOGW(TAG, "agent enabled but host empty — coercing to disabled");
        out->enabled = false;
        strncpy(out->host, AGENT_DEFAULT_HOST, sizeof(out->host) - 1);
        out->host[sizeof(out->host) - 1] = '\0';
    }

    return ESP_OK;
}

esp_err_t agent_config_save(const agent_config_t *c)
{
    if (c == NULL || c->host[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(AGENT_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open('%s') failed: %s",
                 AGENT_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, AGENT_KEY_HOST, c->host);
    if (err != ESP_OK) goto fail;
    err = nvs_set_u8(handle, AGENT_KEY_ENABLED, c->enabled ? 1 : 0);
    if (err != ESP_OK) goto fail;
    err = nvs_commit(handle);
    if (err != ESP_OK) goto fail;

    nvs_close(handle);
    ESP_LOGI(TAG, "Agent config saved: enabled=%d host='%s'", c->enabled, c->host);
    return ESP_OK;

fail:
    ESP_LOGW(TAG, "agent_config_save failed at step: %s", esp_err_to_name(err));
    nvs_close(handle);
    return err;
}
