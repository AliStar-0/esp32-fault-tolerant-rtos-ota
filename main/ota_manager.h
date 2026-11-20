#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Print active partition label, firmware version, and OTA slot state.
 */
void ota_log_partition_info(void);

/**
 * @brief Validate a newly booted OTA image (ESP_OTA_IMG_PENDING_VERIFY).
 *        Marks firmware valid if self-tests pass, or triggers immediate
 *        bootloader rollback to the previous working slot on failure.
 *
 * @param subsystem_health_ok True if critical peripherals/tasks initialized properly.
 */
void ota_run_boot_diagnostics(bool subsystem_health_ok);

/**
 * @brief Download, version-check, verify cryptographic signature, and apply
 *        an Over-The-Air firmware update to the inactive flash partition.
 *
 * @param firmware_url HTTPS or HTTP URL of the signed .bin image.
 * @return ESP_OK if update succeeded (triggers reboot), or ESP_ERR_* on failure.
 */
esp_err_t ota_perform_signed_update(const char *firmware_url);

#endif // OTA_MANAGER_H
