# esp32-fault-tolerant-rtos-ota

[![Framework: ESP-IDF](https://img.shields.io/badge/Framework-ESP--IDF%20v5.x-blue.svg)](https://docs.espressif.com/projects/esp-idf/)
[![RTOS: FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS%20SMP-green.svg)](https://www.freertos.org/)
[![Target: ESP32](https://img.shields.io/badge/Target-ESP32%20Dual--Core-red.svg)](https://www.espressif.com/)
[![Security: RSA-3072 OTA](https://img.shields.io/badge/OTA%20Security-RSA--3072%20Signed-orange.svg)](#cryptographic-ota-verification)

A production-oriented **ESP32 (ESP-IDF + FreeRTOS)** embedded telemetry node engineered for high availability in unattended environments. It combines **SMP dual-core task isolation**, **multi-task hardware Watchdog supervision**, **RTC-retained crash diagnostics**, **RSA-3072 cryptographically signed Over-The-Air (OTA) updates**, and **automatic A/B bootloader rollback**.

---

## Key Engineering Features

* **Deterministic Dual-Core SMP Scheduling:** Isolates real-time 50 Hz I2C sensor acquisition on **Core 1** from non-deterministic Wi-Fi/HTTP network I/O and CLI management on **Core 0** using a zero-copy overwrite FreeRTOS Queue (`xQueueOverwrite`).
* **Multi-Task Hardware Watchdog Supervisor:** Uses a FreeRTOS `EventGroup` heartbeat aggregator to gate `esp_task_wdt_reset()`. If any worker task deadlocks or experiences an I2C bus hang, the supervisor withholds the watchdog feed, forcing a deterministic hardware reset and saving an ELF coredump to flash.
* **Persistent Crash Telemetry (`RTC_NOINIT_ATTR`):** Tracks total boot counts and Watchdog/panic crash counters across resets in uninitialized RTC slow memory without causing flash wear.
* **Cryptographically Signed OTA Updates:** Verifies incoming firmware binaries using **RSA-3072** signature validation (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y`) and inspects `esp_app_desc_t` headers prior to flashing to reject unsigned, tampered, or redundant images without permanently burning Secure Boot eFuses.
* **A/B Partition Layout & Automatic Bootloader Rollback:** Uses dual 1.75 MB application slots (`ota_0` / `ota_1`) with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. Newly booted firmware starts in `ESP_OTA_IMG_PENDING_VERIFY` and must pass hardware self-tests before locking itself `VALID`—automatically reverting to the previous working slot on failure.

---

## System Architecture

```text
+-----------------------------------------------------------------------------------+
|                                ESP32 Dual-Core SoC                                |
|                                                                                   |
|   CORE 1 (Real-Time & Supervision)           CORE 0 (Networking & Management)     |
|  +---------------------------------+        +----------------------------------+  |
|  | health_supervisor_task (Prio 6) |        | telemetry_task (Prio 4)          |  |
|  |  - Waits on EventGroup bits     |        |  - Drains sensor Queue           |  |
|  |  - Feeds Hardware TWDT (5s)     |        |  - Formats JSON telemetry        |  |
|  +----------------^----------------+        |  - Sets BIT_TELEMETRY_ALIVE      |  |
|                   |                         +----------------^-----------------+  |
|        +----------+----------+                               |                    |
|        |                     |               +---------------+-----------------+  |
|  +-----+-------------------+ |               |       FreeRTOS Queue (Depth 1)  |  |
|  | sensor_acq_task (Prio 5)| |               +---------------^-----------------+  |
|  |  - 50 Hz I2C MPU6050    |-+                               |                    |
|  |  - Sets BIT_SENSOR_ALIVE|---------------------------------+                    |
|  +-------------------------+                +----------------------------------+  |
|                                             | ota_and_cli_task (Prio 3)        |  |
|  +-------------------------+                |  - Serial CLI & Fault Trigger    |  |
|  | RTC_NOINIT_ATTR Memory  |                |  - Header Version Verification   |  |
|  |  - Survives WDT resets  |                |  - RSA-3072 Signature Check      |  |
|  +-------------------------+                +----------------------------------+  |
+-----------------------------------------------------------------------------------+
```
