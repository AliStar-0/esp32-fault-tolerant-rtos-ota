#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ota_manager.h"
#include "sensor_driver.h"

static const char *TAG = "MAIN_NODE";

/* Hardware Pins */
#define LED_STATUS_GPIO         GPIO_NUM_2
#define FAULT_BUTTON_GPIO       GPIO_NUM_0

/* Health Supervisor EventGroup Bits */
#define BIT_SENSOR_ALIVE        (1 << 0)
#define BIT_TELEMETRY_ALIVE     (1 << 1)
#define ALL_TASKS_ALIVE         (BIT_SENSOR_ALIVE | BIT_TELEMETRY_ALIVE)

/* RTC No-Init Memory: survives Watchdog and Panic resets without wearing flash */
#define RTC_MAGIC_COOKIE        0xDEADBEEF
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static uint32_t s_rtc_wdt_crash_count;
RTC_NOINIT_ATTR static uint32_t s_rtc_total_boot_count;

/* RTOS Synchronization Primitives */
static QueueHandle_t s_sensor_queue = NULL;
static EventGroupHandle_t s_health_eg = NULL;
static volatile bool s_inject_sensor_deadlock = false;
static volatile bool s_wifi_connected = false;
static char s_pending_ota_url[256] = {0};
static volatile bool s_trigger_ota = false;

static const char *reset_reason_to_str(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWER_ON";
        case ESP_RST_EXT:       return "EXTERNAL_PIN";
        case ESP_RST_SW:        return "SOFTWARE_REBOOT";
        case ESP_RST_PANIC:     return "EXCEPTION_PANIC";
        case ESP_RST_INT_WDT:   return "INTERRUPT_WATCHDOG";
        case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
        case ESP_RST_WDT:       return "OTHER_WATCHDOG";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        default:                return "UNKNOWN";
    }
}

static void init_rtc_diagnostics(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    if (s_rtc_magic != RTC_MAGIC_COOKIE) {
        s_rtc_magic = RTC_MAGIC_COOKIE;
        s_rtc_wdt_crash_count = 0;
        s_rtc_total_boot_count = 1;
    } else {
        s_rtc_total_boot_count++;
        if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT) {
            s_rtc_wdt_crash_count++;
        }
    }

    ESP_LOGI(TAG, "Boot #%lu | Reset Reason: %s | Recorded WDT/Panic Crashes: %lu",
             s_rtc_total_boot_count, reset_reason_to_str(reason), s_rtc_wdt_crash_count);
}

/* Wi-Fi Event Handler with Exponential Backoff */
static int s_wifi_retry_num = 0;
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        int backoff_ms = (1 << (s_wifi_retry_num < 5 ? s_wifi_retry_num : 5)) * 500;
        s_wifi_retry_num++;
        ESP_LOGW(TAG, "Wi-Fi disconnected. Reconnecting in %d ms (attempt %d)...",
                 backoff_ms, s_wifi_retry_num);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry_num = 0;
        s_wifi_connected = true;
        ESP_LOGI(TAG, "Wi-Fi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void) {
    if (strcmp(CONFIG_NODE_WIFI_SSID, "YOUR_WIFI_SSID") == 0) {
        ESP_LOGW(TAG, "Default Wi-Fi SSID configured. Update CONFIG_NODE_WIFI_SSID in sdkconfig/menuconfig.");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_NODE_WIFI_SSID,
            .password = CONFIG_NODE_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* Core 1 (High Priority): Deterministic 50 Hz Sensor Acquisition */
static void sensor_acq_task(void *pvParameters) {
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(20); // 50 Hz
    imu_sample_t sample;

    while (1) {
        /* Fault-injection hook: simulates an I2C bus hang or infinite loop deadlock */
        if (s_inject_sensor_deadlock) {
            ESP_LOGE("SENSOR_TASK", "FAULT INJECTED: Simulating infinite task stall!");
            while (1) {
                /* Intentionally starves health_supervisor_task so TWDT fires */
            }
        }

        if (sensor_driver_read(&sample) == ESP_OK) {
            xQueueOverwrite(s_sensor_queue, &sample);
            xEventGroupSetBits(s_health_eg, BIT_SENSOR_ALIVE);
        }

        vTaskDelayUntil(&last_wake_time, period);
    }
}

/* Core 0 (Medium Priority): Telemetry Aggregation & Status Heartbeat */
static void telemetry_task(void *pvParameters) {
    imu_sample_t latest;
    uint32_t report_counter = 0;
    bool led_state = false;

    while (1) {
        if (xQueueReceive(s_sensor_queue, &latest, pdMS_TO_TICKS(500)) == pdTRUE) {
            report_counter++;

            /* Log structured JSON telemetry every 50 samples (~1 second) */
            if (report_counter % 50 == 0) {
                led_state = !led_state;
                gpio_set_level(LED_STATUS_GPIO, led_state);

                ESP_LOGI("TELEMETRY",
                         "{\"seq\":%lu,\"hw\":%s,\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
                         "\"temp\":%.1f,\"crashes\":%lu,\"heap\":%lu}",
                         latest.sequence_num,
                         latest.hardware_verified ? "true" : "false",
                         latest.accel_x_g, latest.accel_y_g, latest.accel_z_g,
                         latest.temp_c,
                         s_rtc_wdt_crash_count,
                         (unsigned long)esp_get_free_heap_size());
            }

            xEventGroupSetBits(s_health_eg, BIT_TELEMETRY_ALIVE);
        }
    }
}

/* Core 0 (Low Priority): Signed OTA Update Manager & Serial CLI */
static void ota_and_cli_task(void *pvParameters) {
    /* Configure UART0 for interactive serial test commands */
    uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_cfg);

    char line[256];
    int idx = 0;

    ESP_LOGI("CLI", "Commands ready: 'status' | 'deadlock' | 'ota [url]' | 'reboot'");

    while (1) {
        /* Poll BOOT button (GPIO 0) for physical fault injection */
        if (gpio_get_level(FAULT_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
            if (gpio_get_level(FAULT_BUTTON_GPIO) == 0) {
                s_inject_sensor_deadlock = true;
            }
        }

        /* Trigger OTA if requested via CLI */
        if (s_trigger_ota) {
            s_trigger_ota = false;
            const char *url = (strlen(s_pending_ota_url) > 0)
                                  ? s_pending_ota_url
                                  : CONFIG_NODE_OTA_DEFAULT_URL;
            ota_perform_signed_update(url);
        }

        /* Non-blocking UART command reader */
        uint8_t ch;
        while (uart_read_bytes(UART_NUM_0, &ch, 1, 0) > 0) {
            if (ch == '\r' || ch == '\n') {
                if (idx > 0) {
                    line[idx] = '\0';
                    idx = 0;

                    if (strcmp(line, "status") == 0) {
                        ota_log_partition_info();
                        ESP_LOGI("CLI", "Boot #%lu | WDT Crashes: %lu | Free Heap: %lu B",
                                 s_rtc_total_boot_count, s_rtc_wdt_crash_count,
                                 (unsigned long)esp_get_free_heap_size());
                    } else if (strcmp(line, "deadlock") == 0) {
                        s_inject_sensor_deadlock = true;
                    } else if (strncmp(line, "ota", 3) == 0) {
                        if (strlen(line) > 4) {
                            strncpy(s_pending_ota_url, line + 4, sizeof(s_pending_ota_url) - 1);
                            s_pending_ota_url[sizeof(s_pending_ota_url) - 1] = '\0';
                        } else {
                            s_pending_ota_url[0] = '\0';
                        }
                        s_trigger_ota = true;
                    } else if (strcmp(line, "reboot") == 0) {
                        ESP_LOGW("CLI", "Manual software reboot...");
                        esp_restart();
                    } else {
                        ESP_LOGW("CLI", "Unknown command '%s'. Use: status | deadlock | ota [url] | reboot", line);
                    }
                }
            } else if (idx < (int)sizeof(line) - 1) {
                line[idx++] = (char)ch;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* Core 1 (Top Priority): Hardware Task Watchdog Supervisor */
static void health_supervisor_task(void *pvParameters) {
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_LOGI("SUPERVISOR", "Hardware Task Watchdog Supervisor armed (5s timeout).");

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            s_health_eg,
            ALL_TASKS_ALIVE,
            pdTRUE,
            pdTRUE,
            pdMS_TO_TICKS(3000)
        );

        if ((bits & ALL_TASKS_ALIVE) == ALL_TASKS_ALIVE) {
            esp_task_wdt_reset();
        } else {
            ESP_LOGE("SUPERVISOR",
                     "CRITICAL: Task heartbeat timeout (bits=0x%02lx)! Withholding WDT reset...",
                     (unsigned long)bits);
        }
    }
}

void app_main(void) {
    /* Configure GPIO 2 (Status LED) and GPIO 0 (BOOT Fault-Injection Button) */
    gpio_reset_pin(LED_STATUS_GPIO);
    gpio_set_direction(LED_STATUS_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(FAULT_BUTTON_GPIO);
    gpio_set_direction(FAULT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(FAULT_BUTTON_GPIO, GPIO_PULLUP_ONLY);

    init_rtc_diagnostics();
    ota_log_partition_info();

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize I2C Sensor Driver & Validate Boot Diagnostics */
    sensor_driver_init();
    ota_run_boot_diagnostics(true);

    /* Initialize Wi-Fi */
    wifi_init_sta();

    /* Create RTOS Queue (depth 1 for latest-sample overwrite) & Health EventGroup */
    s_sensor_queue = xQueueCreate(1, sizeof(imu_sample_t));
    s_health_eg = xEventGroupCreate();
    configASSERT(s_sensor_queue != NULL && s_health_eg != NULL);

    /* Pin Tasks Across Dual Cores */
    xTaskCreatePinnedToCore(health_supervisor_task, "health_sup",   3072, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(sensor_acq_task,        "sensor_acq",   4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(telemetry_task,         "telemetry",    6144, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(ota_and_cli_task,       "ota_cli",      8192, NULL, 3, NULL, 0);
}
