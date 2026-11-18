# ESP32 Fault-Tolerant RTOS & Secure OTA

<p align="center">
  <strong>A dual-core ESP32 telemetry node demonstrating fault recovery, persistent diagnostics, and cryptographically verified OTA firmware updates.</strong>
</p>

<p align="center">
  <a href="https://docs.espressif.com/projects/esp-idf/"><img src="https://img.shields.io/badge/Framework-ESP--IDF%20v5.x-blue.svg" alt="ESP-IDF v5.x"></a>
  <a href="https://www.freertos.org/"><img src="https://img.shields.io/badge/RTOS-FreeRTOS%20SMP-green.svg" alt="FreeRTOS SMP"></a>
  <img src="https://img.shields.io/badge/Target-ESP32%20Dual--Core-red.svg" alt="Dual-core ESP32">
  <img src="https://img.shields.io/badge/OTA-RSA--3072%20Signed-orange.svg" alt="RSA-3072 signed OTA">
  <img src="https://img.shields.io/badge/Flash-4%20MB-lightgrey.svg" alt="4 MB flash">
</p>

---

## Overview

**ESP32 Fault-Tolerant RTOS & Secure OTA** is an embedded telemetry project built with **ESP-IDF and FreeRTOS** for unattended systems that need to detect failures, recover automatically, and retain useful diagnostic information.

The firmware combines:

- **Dual-core task isolation** to separate periodic sensor acquisition from networking and management work.
- **Multi-task watchdog supervision** using FreeRTOS event-group heartbeats.
- **Persistent crash diagnostics** stored in RTC slow memory across applicable resets.
- **RSA-3072 signed OTA updates** to verify firmware authenticity and integrity before installation.
- **A/B application partitions and bootloader rollback** to recover when a newly installed firmware image fails its post-update checks.
- **Fault-injection commands** to exercise recovery and update scenarios during testing.

The project is intended to demonstrate practical embedded reliability and firmware-update mechanisms on an ESP32 development board.

## Key Features

| Feature | Implementation | Purpose |
|---|---|---|
| Dual-core scheduling | FreeRTOS SMP task affinity | Separates periodic sensor work from network and CLI activity |
| Sensor acquisition | MPU6050 over I2C at 50 Hz | Produces periodic accelerometer and gyroscope readings |
| Task supervision | EventGroup heartbeats and Task Watchdog Timer | Detects a worker that stops checking in |
| Crash diagnostics | `RTC_NOINIT_ATTR` counters | Retains boot and crash information across applicable resets |
| Firmware authentication | RSA-3072 signed images | Rejects firmware that fails signature validation |
| OTA update strategy | `ota_0` and `ota_1` application slots | Keeps an alternate application image available |
| Failed-update recovery | ESP-IDF OTA rollback | Returns to the previous valid image after a failed first-boot check |
| Testability | UART CLI and simulated sensor fallback | Allows key behaviors to be exercised without every peripheral attached |

## System Architecture

The application uses four main FreeRTOS tasks across the ESP32's two cores.

```text
+-----------------------------------------------------------------------------------+
|                                ESP32 Dual-Core SoC                                |
|                                                                                   |
|   CORE 1: Real-Time & Supervision             CORE 0: Networking & Management     |
|  +---------------------------------+        +----------------------------------+  |
|  | health_supervisor_task (P6)     |        | telemetry_task (P4)              |  |
|  | - Waits for worker heartbeats   |        | - Reads latest sensor frame     |  |
|  | - Feeds hardware TWDT on success|        | - Formats JSON telemetry        |  |
|  +----------------^----------------+        | - Reports crash counters        |  |
|                   |                         +----------------^-----------------+  |
|                   |                                          |                    |
|  +----------------+-----------------+        +---------------+-----------------+  |
|  | sensor_acq_task (P5)              |        | FreeRTOS Queue (Depth 1)        |  |
|  | - MPU6050 I2C acquisition at 50 Hz|------->| Latest sensor sample            |  |
|  | - Publishes sensor frame          |        +---------------------------------+  |
|  | - Sets sensor heartbeat bit       |                                           |
|  +-----------------------------------+        +---------------------------------+  |
|                                               | ota_and_cli_task (P3)            |  |
|  +-----------------------------------+        | - UART CLI and fault injection  |  |
|  | RTC_NOINIT_ATTR memory            |        | - OTA image/version checks      |  |
|  | - Boot counter                    |        | - RSA-3072 signature validation|  |
|  | - Watchdog/panic crash counters   |        +---------------------------------+  |
|  +-----------------------------------+                                           |
+-----------------------------------------------------------------------------------+
```

### Task Allocation & Watchdog Policy

| Task | Core | Priority | Stack | Responsibility |
|---|---:|---:|---:|---|
| `health_supervisor_task` | 1 | 6 | 3072 B | Waits for required worker heartbeat bits and feeds the hardware TWDT only when the required workers have checked in. |
| `sensor_acq_task` | 1 | 5 | 4096 B | Reads the MPU6050 at 50 Hz using `vTaskDelayUntil()`, publishes the latest sample, and signals its heartbeat. |
| `telemetry_task` | 0 | 4 | 6144 B | Consumes sensor samples, emits JSON telemetry and crash counters, and signals its heartbeat. |
| `ota_and_cli_task` | 0 | 3 | 8192 B | Handles UART commands, fault-injection input, and signed OTA update operations. |

**Sensor-to-telemetry handoff:** The sensor task publishes to a FreeRTOS queue with a depth of one using `xQueueOverwrite()`. This keeps the most recent sample available to the consumer rather than building up a backlog of stale measurements. `xQueueOverwrite()` copies the queue item; it is not a zero-copy transfer.

**Watchdog supervision:** Worker tasks report their health through EventGroup bits. The supervisor feeds the Task Watchdog Timer only after the required heartbeat conditions are met. If a worker stops reporting, the supervisor withholds the watchdog reset so the configured watchdog timeout can trigger recovery.

## Hardware Requirements

| Component | Requirement |
|---|---|
| MCU board | ESP32 DevKitC / ESP32-WROOM-32 |
| Flash | 4 MB SPI flash |
| IMU | MPU6050 6-axis accelerometer/gyroscope |
| USB connection | USB-to-UART connection for flashing and serial monitoring |
| Development host | Computer with PlatformIO and ESP-IDF tooling available |

### MPU6050 Wiring

The sensor uses I2C address `0x68` in the documented configuration.

| MPU6050 | ESP32 |
|---|---|
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| VCC | 3.3V |
| GND | GND |

### Onboard Controls

- **GPIO 0 (`BOOT`):** Used by the project as a physical fault-injection input.
- **GPIO 2:** Status heartbeat LED.

> **Sensor fallback:** If an MPU6050 is not detected during boot, `sensor_driver.c` logs a warning and uses deterministic simulated IMU waveforms. This allows the RTOS and OTA paths to be exercised without the physical sensor. Simulated readings should not be treated as real sensor measurements.

## Flash Partition Layout

The partition table is configured for a **4 MB SPI flash** device. It includes two application slots, OTA metadata, and a dedicated ELF coredump partition.

| Partition | Type / Subtype | Offset | Size | Purpose |
|---|---|---:|---:|---|
| `nvs` | `data / nvs` | `0x009000` | 24 KB | Wi-Fi credentials and persistent calibration |
| `otadata` | `data / ota` | `0x00F000` | 8 KB | Redundant OTA boot metadata and sequence counters |
| `phy_init` | `data / phy` | `0x011000` | 4 KB | RF PHY calibration |
| `ota_0` | `app / ota_0` | `0x020000` | 1.75 MB | Primary application slot |
| `ota_1` | `app / ota_1` | `0x1E0000` | 1.75 MB | Secondary OTA application slot |
| `coredump` | `data / coredump` | `0x3A0000` | 64 KB | ELF crash coredump storage |

The two application slots allow the device to write an update to the inactive slot while retaining the previously selected application image.

## OTA Update & Rollback Lifecycle

The update flow is designed to avoid permanently switching to firmware that cannot pass its first-boot checks.

1. **Prepare:** The device receives an OTA image URL.
2. **Check:** The update path checks the incoming application metadata and version.
3. **Verify:** The image is validated using the configured RSA-3072 signing mechanism.
4. **Install:** A verified image is written to the inactive OTA application partition.
5. **Reboot:** The bootloader selects the newly installed image.
6. **Pending verification:** The new image starts in the `ESP_OTA_IMG_PENDING_VERIFY` state.
7. **Self-test:** The application runs its post-update hardware and health checks.
8. **Confirm or roll back:** A successful check marks the image valid. A failed check marks it invalid and requests rollback to the previous working image.

The project enables the ESP-IDF rollback configuration with:

```ini
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

The application must perform the appropriate OTA state-transition calls for this mechanism to work; enabling the configuration alone does not implement the self-test or confirmation logic.

## Cryptographic OTA Verification

Firmware updates use **RSA-3072 signatures** to validate image authenticity and integrity.

The documented configuration uses ESP-IDF signed-app verification without enabling hardware Secure Boot eFuse enforcement:

```ini
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
CONFIG_SECURE_BOOT_VERIFICATION_KEY="keys/ota_public_key.bin"
```

This project-level approach is intended to demonstrate signed OTA validation without permanently burning Secure Boot eFuses. It should not be interpreted as equivalent to a device protected by hardware-enforced Secure Boot.

### Signing Keys

- The **private signing key** is used to sign firmware images and must remain secret.
- The **public verification key** is included in the firmware verification configuration.
- Do not commit private keys, passwords, or production credentials to source control.

The repository is expected to exclude private key material through `.gitignore`. Check that rule before generating or storing keys.

## Getting Started

### Prerequisites

Install and configure:

- [Visual Studio Code](https://code.visualstudio.com/)
- [PlatformIO IDE](https://platformio.org/install/ide?install=vscode)
- ESP-IDF v5.x-compatible PlatformIO platform/tooling
- Python 3
- A supported ESP32 board with 4 MB flash
- A USB data cable and serial terminal

You will also need a reachable **2.4 GHz Wi-Fi network** for the network and OTA features.

### 1. Clone the Repository

```bash
git clone <repository-url>
cd esp32-fault-tolerant-rtos-ota
```

Replace `<repository-url>` with the URL of your repository.

### 2. Generate RSA-3072 Signing Keys

Create the signing key directory if it does not already exist:

```bash
mkdir -p keys
```

Generate a private signing key and extract its public verification key:

```bash
espsecure.py generate_signing_key --version 2 --scheme rsa3072 keys/ota_private_key.pem
espsecure.py extract_public_key --version 2 --keyfile keys/ota_private_key.pem keys/ota_public_key.bin
```

**Important:** Keep `keys/ota_private_key.pem` private. Do not commit it to Git or distribute it with firmware releases.

### 3. Configure Firmware and Wi-Fi

Review `sdkconfig.defaults` and ensure the signed OTA options and verification key path match your project setup.

Set your local Wi-Fi credentials using the project's supported configuration mechanism. The documented configuration symbols are:

```ini
CONFIG_NODE_WIFI_SSID="Your_2.4GHz_SSID"
CONFIG_NODE_WIFI_PASSWORD="Your_WiFi_Password"
```

Do not commit real Wi-Fi credentials. Use local configuration or another secrets-management approach appropriate to your workflow.

### 4. Build, Flash, and Monitor

Connect the ESP32 board, select the correct PlatformIO environment and serial port, then run:

```bash
pio run -t upload -t monitor
```

The serial monitor is configured for **115200 baud**.

### 5. Build, Sign, and Serve an OTA Update

Update the application version in the project configuration. For the example release, set:

```ini
CONFIG_APP_PROJECT_VER="1.1.0"
```

Build the firmware:

```bash
pio run
```

Then run the helper script to sign the compiled image and serve it over HTTP:

```bash
python scripts/sign_and_serve.py --version 1.1.0
```

Use the URL printed or expected by the helper script with the device's OTA command:

```text
ota <url>
```

The example workflow uses a local HTTP server. Image signature verification is still required; HTTP transport itself does not provide encryption or server authentication.

## UART CLI & Fault-Injection Tests

Use the serial monitor at **115200 baud** to interact with the device.

| Command / Action | Behavior | Expected result |
|---|---|---|
| `status` | Prints the active flash slot, firmware version, boot count, and crash count. | Allows inspection of the running OTA slot and image state. |
| `deadlock` or the documented BOOT fault trigger | Injects a stall into `sensor_acq_task`. | The missing heartbeat prevents the supervisor from feeding the TWDT; the watchdog should trigger a reset. |
| `ota <url>` | Downloads and processes a firmware image. | Unsigned or invalid images should fail validation; valid signed images can be installed. |
| Hold `BOOT` during the documented OTA test | Exercises the project's post-update failure path. | If the application self-test detects the injected failure, the new slot should be rejected and rollback should occur. |

> **Testing note:** GPIO 0 is also an ESP32 boot-strapping pin. The exact behavior of holding `BOOT` across a reset depends on the board and the firmware's fault-injection implementation. Confirm the intended test sequence in the application before using it during an OTA reboot.

## Verification Examples

The following are representative serial logs from the documented test scenarios.

### 1. Watchdog Deadlock Recovery

A stalled sensor task stops reporting its heartbeat. The supervisor withholds the watchdog feed, and the device reboots.

```text
E (18420) SENSOR_TASK: FAULT INJECTED: Simulating infinite task stall!
E (21420) SUPERVISOR: CRITICAL: Task heartbeat timeout (bits=0x02)! Withholding WDT reset...
E (23420) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (23420) task_wdt:  - health_sup (CPU 1)
...
I (312) MAIN_NODE: Boot #2 | Reset Reason: TASK_WATCHDOG | Recorded WDT/Panic Crashes: 1
I (1340) TELEMETRY: {"seq":50,"hw":true,"ax":-0.48,"ay":0.09,"az":-0.03,"temp":26.8,"crashes":1,"heap":202620}
```

### 2. Unsigned OTA Image Rejection

An unsigned image is rejected during image validation.

```text
I (42110) OTA_MGR: Initiating OTA check from: http://192.168.1.15:8080/firmware_unsigned.bin
I (42195) OTA_MGR: Current version: 1.0.0 | Incoming version: 1.1.0
E (44820) esp_image: image corrupted or invalid signature
E (44825) OTA_MGR: OTA signature/integrity verification FAILED (err=0x1503: ESP_ERR_OTA_VALIDATE_FAILED)!
```

### 3. Signed OTA Update and Automatic Rollback

The new image is installed and selected for the next boot. Its post-update self-test fails, so the device returns to the previous valid slot.

```text
I (12850) OTA_MGR: OTA image verified and written! Rebooting into new slot...
...
I (310) OTA_MGR: Running Partition : ota_1 (offset 0x1e0000)
I (315) OTA_MGR: Firmware Version  : 1.1.0
I (320) OTA_MGR: OTA Slot State    : PENDING_VERIFY
W (325) OTA_MGR: First boot of unverified OTA image in slot [ota_1]!
E (830) OTA_MGR: Post-OTA self-test FAILED (health=1, forced_fault=1)!
E (835) OTA_MGR: Marking slot [ota_1] INVALID and rolling back to previous firmware...
...
I (310) OTA_MGR: Running Partition : ota_0 (offset 0x020000)
I (315) OTA_MGR: Firmware Version  : 1.0.0
I (320) OTA_MGR: OTA Slot State    : VALID
```

## Repository Structure

```text
esp32-fault-tolerant-rtos-ota/
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild       # Project configuration entries
│   ├── main.c                  # RTOS tasks, watchdog supervisor, UART CLI
│   ├── ota_manager.c           # OTA metadata/version checks, signature validation, rollback
│   ├── ota_manager.h
│   ├── sensor_driver.c         # MPU6050 I2C driver and simulation fallback
│   └── sensor_driver.h
├── scripts/
│   └── sign_and_serve.py       # Signs firmware and hosts the OTA image
├── keys/
│   └── ota_public_key.bin      # Public verification key (generated locally)
├── partitions.csv              # Custom 4 MB partition table
├── platformio.ini               # PlatformIO build and monitor configuration
├── sdkconfig.defaults           # ESP-IDF default configuration
└── README.md
```

Private signing keys should remain local and be excluded from version control.

## Troubleshooting

| Symptom | Checks |
|---|---|
| ESP32 does not appear in PlatformIO | Check the USB cable, USB-to-UART driver, selected serial port, and board connection. |
| MPU6050 is not detected | Check 3.3 V power, common ground, SDA/SCL wiring, and I2C address `0x68`. The documented simulation fallback can be used to test other paths. |
| Wi-Fi connection fails | Verify the 2.4 GHz SSID, password, signal, and project configuration. |
| OTA image is rejected | Confirm that the image was signed with the matching private key and that the device has the corresponding public verification key. |
| OTA update cannot be served | Confirm that the host and ESP32 can reach each other and that the helper server is running on the expected address and port. |
| Device rolls back after an update | Inspect the post-update self-test and serial logs to identify which health or peripheral check failed. |
| Watchdog reset occurs unexpectedly | Review task heartbeat reporting, blocking calls, I2C behavior, task stack usage, and watchdog configuration. |

## Reliability and Security Notes

- **Watchdog recovery is not a substitute for eliminating deadlocks.** It provides a recovery path when monitored tasks stop reporting.
- **RTC-retained data has limits.** `RTC_NOINIT_ATTR` memory can retain values across applicable reset paths, but it is not a substitute for durable storage across power loss or every reset condition.
- **Coredump availability depends on configuration.** Confirm that the ESP-IDF coredump settings, partition layout, and panic behavior are consistent with the target build.
- **Signed OTA is not the same as hardware Secure Boot.** The documented no-Secure-Boot signing configuration validates signed images through the configured software/bootloader mechanisms; it does not provide the same hardware root-of-trust guarantees as eFuse-enforced Secure Boot.
- **Protect the private key.** Anyone with access to the signing key may be able to produce firmware that passes signature verification.
- **HTTP is not encrypted transport.** The sample local HTTP server is useful for development, but production deployments should consider authenticated and encrypted transport as an additional protection layer.
- **Validate on the exact hardware and ESP-IDF version being deployed.** Flash layout, watchdog configuration, signing options, and rollback behavior must be tested together.

## Project Goals

This project brings together several important embedded-systems concepts in one demonstrable firmware application:

- Real-time scheduling and task affinity on a dual-core MCU.
- Sensor data acquisition and inter-task communication.
- Health monitoring and watchdog-driven fault recovery.
- Crash diagnostics and reset tracking.
- Firmware authenticity checks and A/B OTA updates.
- Failure injection and repeatable serial verification.

The result is a practical foundation for exploring reliability-oriented ESP32 firmware design.

---

## References

- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/)
- [FreeRTOS Documentation](https://www.freertos.org/)
- [ESP-IDF OTA Updates](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html)
- [ESP-IDF Watchdogs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/wdts.html)
- [ESP-IDF Application Image Format](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/app_image_format.html)
