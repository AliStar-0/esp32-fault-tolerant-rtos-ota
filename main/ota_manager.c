#include "ota_manager.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

static const char *TAG = "OTA_MGR";

static const char *SERVER_CERT_PEM =
    "-----BEGIN CERTIFICATE-----\n"
    "REPLACE_WITH_YOUR_OTA_SERVER_CERT_FOR_HTTPS\n"
    "-----END CERTIFICATE-----\n";

static const char *ota_state_to_str(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW:            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID:          return "VALID";
        case ESP_OTA_IMG_INVALID:        return "INVALID";
        case ESP_OTA_IMG_ABORTED:        return "ABORTED";
        default:                         return "UNDEFINED";
    }
}

void ota_log_partition_info(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app_desc = esp_app_get_description();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &ota_state);

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "Running Partition : %s (offset 0x%06lx)", running->label, running->address);
    ESP_LOGI(TAG, "Firmware Version  : %s", app_desc->version);
    ESP_LOGI(TAG, "Build Timestamp   : %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, "OTA Slot State    : %s", ota_state_to_str(ota_state));
    ESP_LOGI(TAG, "==========================================");
}

void ota_run_boot_diagnostics(bool subsystem_health_ok) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        ESP_LOGW(TAG, "Could not query OTA state (likely factory partition).");
        return;
    }

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "First boot of unverified OTA image in slot [%s]!", running->label);
        ESP_LOGW(TAG, "Hold BOOT button (GPIO 0) low to simulate failed self-test...");

        /* Give user/test bench 500ms window to assert GPIO 0 fault trigger */
        vTaskDelay(pdMS_TO_TICKS(500));
        bool forced_fault = (gpio_get_level(GPIO_NUM_0) == 0);

        if (!forced_fault && subsystem_health_ok) {
            ESP_LOGI(TAG, "Post-OTA self-test PASSED. Committing slot [%s] as VALID.", running->label);
            esp_ota_mark_app_valid_cancel_rollback();
        } else {
            ESP_LOGE(TAG, "Post-OTA self-test FAILED (health=%d, forced_fault=%d)!",
                     subsystem_health_ok, forced_fault);
            ESP_LOGE(TAG, "Marking slot [%s] INVALID and rolling back to previous firmware...",
                     running->label);
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }
}

esp_err_t ota_perform_signed_update(const char *firmware_url) {
    if (!firmware_url || strlen(firmware_url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initiating OTA check from: %s", firmware_url);

    esp_http_client_config_t http_cfg = {
        .url = firmware_url,
        .cert_pem = SERVER_CERT_PEM,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = true,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Inspect incoming firmware header before writing full binary */
    esp_app_desc_t incoming_desc;
    err = esp_https_ota_get_img_desc(ota_handle, &incoming_desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read incoming image header: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        return err;
    }

    const esp_app_desc_t *running_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Current version: %s | Incoming version: %s",
             running_desc->version, incoming_desc.version);

    if (strcmp(incoming_desc.version, running_desc->version) == 0) {
        ESP_LOGW(TAG, "Incoming version matches active version (%s). Aborting redundant OTA.",
                 running_desc->version);
        esp_https_ota_abort(ota_handle);
        return ESP_ERR_INVALID_STATE;
    }

    /* Stream image chunks to inactive OTA partition */
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int bytes_read = esp_https_ota_get_image_len_read(ota_handle);
        if (bytes_read % (64 * 1024) < 4096) {
            ESP_LOGI(TAG, "Downloaded %d bytes...", bytes_read);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA stream failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        return err;
    }

    /* Verify SHA-256 & RSA-3072 signature (when CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y) */
    esp_err_t finish_err = esp_https_ota_finish(ota_handle);
    if (finish_err == ESP_OK) {
        ESP_LOGI(TAG, "OTA image verified and written! Rebooting into new slot...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA signature/integrity verification FAILED (err=0x%x: %s)!",
                 finish_err, esp_err_to_name(finish_err));
    }

    return finish_err;
}
