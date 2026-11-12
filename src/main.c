/*
 * This file is part of the Pico Keys SDK distribution (https://github.com/polhenarejos/pico-keys-sdk).
 * Copyright (c) 2022 Pol Henarejos.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "picokeys.h"
#include "button.h"
#include <stdio.h>
#if !defined(ENABLE_EMULATION)
#include "tusb.h"
#endif
#if defined(ENABLE_EMULATION)
#include "emulation.h"
#elif defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sd_logger.h"
#include "rom/gpio.h"
#include "tinyusb.h"
#elif defined(PICO_PLATFORM)
#include "bsp/board.h"
#include "hardware/structs/ioqspi.h"
#include "pico/stdio.h"
#endif

#include "random.h"
#include "hwrng.h"
#include "apdu.h"
#include "usb.h"
#include "flash.h"
#include "otp.h"
#include "led/led.h"
#include "pico_time.h"
#include "serial.h"
#include "mbedtls/sha256.h"

extern int rescue_migrate_keydev(void);

app_t apps[16];
uint8_t num_apps = 0;

app_t *current_app = NULL;

const uint8_t *ccid_atr = NULL;

bool app_exists(const_byte_array_t aid) {
    for (int a = 0; a < num_apps; a++) {
        if (aid.len >= apps[a].aid[0] && !memcmp(apps[a].aid + 1, aid.data, apps[a].aid[0])) {
            return true;
        }
    }
    return false;
}

int register_app(int (*select_aid)(app_t *, uint8_t), const uint8_t *aid) {
    if (app_exists(CONST_BYTE_ARRAY(aid + 1, aid[0]))) {
        return 1;
    }
    if (num_apps < sizeof(apps) / sizeof(app_t)) {
        apps[num_apps].select_aid = select_aid;
        apps[num_apps].aid = aid;
        num_apps++;
        return 1;
    }
    return 0;
}

int select_app(const_byte_array_t aid) {
    if (current_app && current_app->aid && (current_app->aid + 1 == aid.data || (aid.len >= current_app->aid[0] && !memcmp(current_app->aid + 1, aid.data, current_app->aid[0])))) {
        current_app->select_aid(current_app, 0);
        return PICOKEYS_OK;
    }
    for (int a = 0; a < num_apps; a++) {
        if (aid.len >= apps[a].aid[0] && !memcmp(apps[a].aid + 1, aid.data, apps[a].aid[0])) {
            if (current_app) {
                if (current_app->aid && aid.len >= current_app->aid[0] && !memcmp(current_app->aid + 1, aid.data, current_app->aid[0])) {
                    current_app->select_aid(current_app, 1);
                    return PICOKEYS_OK;
                }
                if (current_app->unload) {
                    current_app->unload();
                }
            }
            current_app = &apps[a];
            if (current_app->select_aid(current_app, 1) == PICOKEYS_OK) {
                return PICOKEYS_OK;
            }
        }
    }
    return PICOKEYS_ERR_FILE_NOT_FOUND;
}


#ifdef ESP_PLATFORM
__attribute__((weak)) bool picokey_sd_logger_pins(sd_logger_pins_t *pins) {
    (void)pins;
    return false;
}
#endif

WEAK int picokey_init(void) {
    return 0;
}
WEAK void picokey_boot_message(const char *message) { (void)message; }
WEAK void picokey_boot_complete(void) {}

void execute_tasks(void);
void execute_tasks(void) {
#if !defined(ENABLE_EMULATION) && !defined(ESP_PLATFORM)
    tud_task(); // tinyusb device task
#endif
#ifdef USB_ITF_LWIP
#if !defined(ENABLE_EMULATION)
    service_traffic();
#endif
    rest_task();
#endif
    usb_task();
    led_blinking_task();
#ifdef ENABLE_LVGL_UI
    platform_ui_task();
#endif
}

#if defined(ESP_PLATFORM) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && \
    CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && !CONFIG_FREERTOS_SMP && !CONFIG_FREERTOS_UNICORE
#define REPORT_CPU_AT_BOOT
static void report_boot_cpu(void) {
    static unsigned samples;
    static uint32_t last, last_idle[2];
    if (samples > 9) return;
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t elapsed = now - last;
    if (last && elapsed < 10000000) return;
    uint32_t idle[2] = {
        ulTaskGetIdleRunTimeCounterForCore(0),
        ulTaskGetIdleRunTimeCounterForCore(1),
    };
    if (last) {
        // The unsigned difference also handles the 32-bit timer wrap.
        unsigned idle0 = (unsigned)(((uint64_t)(idle[0] - last_idle[0]) * 100) / elapsed);
        unsigned idle1 = (unsigned)(((uint64_t)(idle[1] - last_idle[1]) * 100) / elapsed);
        ESP_LOGI("RESOURCES", "scheduler_idle cpu0=%u%% cpu1=%u%% over=%lu ms; heap_min=%u",
            idle0 > 100 ? 100 : idle0, idle1 > 100 ? 100 : idle1,
            (unsigned long)(elapsed / 1000),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    last = now;
    last_idle[0] = idle[0];
    last_idle[1] = idle[1];
    ++samples;
}
#endif

static void core0_loop(void *arg) {
    (void)arg;
#if defined(ESP_PLATFORM) && defined(USB_ITF_LWIP)
    if (ITF_LWIP_TOTAL > 0) {
        lwip_itf_init();
    }
#endif
    while (1) {
        execute_tasks();
        hwrng_task();
        flash_task();
        button_task();
#ifdef ENABLE_EMULATION
        struct timespec nap = {.tv_nsec = 1000000};
        nanosleep(&nap, NULL);
#endif
#ifdef PICO_PLATFORM
        // Avoid a pure busy loop on core0; gives the system a scheduling hint.
        tight_loop_contents();
#endif
#ifdef ESP_PLATFORM
#ifdef REPORT_CPU_AT_BOOT
        report_boot_cpu();
#endif
        vTaskDelay(pdMS_TO_TICKS(10));
#endif
    }
}

#ifdef ESP_PLATFORM
extern tinyusb_config_t tusb_cfg;
extern const uint8_t desc_config[];
extern const char *string_desc_itf[];
TaskHandle_t hcore0 = NULL, hcore1 = NULL;
volatile bool core1_task_running = false;
WEAK void picokey_board_early_init(void) {}

int app_main(void) {
    sd_logger_early_init();
    ESP_LOGI("BOOT", "Reset reason=%d", (int)esp_reset_reason());
    picokey_board_early_init();
    picokey_boot_message("SD log card mounting");
    sd_logger_init();
    picokey_boot_message(sd_logger_has_error() ? "SD log error: console" :
        sd_logger_is_mounted() ? "SD log card ready" : "SD N/A: console log");
#else
#ifdef TDONGLE_DESKTOP
int tdongle_protocol_main(void) {
#else
int main(void) {
#endif
#endif

    serial_init();
    picokey_boot_message("Device identity ready");

#ifndef ENABLE_EMULATION
#ifdef PICO_PLATFORM
    board_init();
    stdio_init_all();
#endif

#endif

    random_init();

    otp_init();
    picokey_boot_message("Device key ready");

    low_flash_init();

    file_scan_flash();
    picokey_boot_message("Credential scan ready");

#ifdef TDONGLE_SPLIT_OPENPGP_STORAGE
    extern int picokey_prepare_storage(void);
#ifdef ESP_PLATFORM
    // Before protocol tasks start this task also owns the flash writer.
    low_flash_init_core1();
#endif
    if (picokey_prepare_storage() != PICOKEYS_OK) {
        picokey_boot_message("ERROR: storage upgrade");
#ifdef ESP_PLATFORM
        ESP_LOGE("STORAGE", "Credential storage migration failed; USB will not start");
#else
        printf("Credential storage migration failed; USB will not start\n");
#endif
        return 5;
    }
    picokey_boot_message("Key storage ready");
#endif

    if (rescue_migrate_keydev() != PICOKEYS_OK) {
        picokey_boot_message("ERROR: attestation key");
#ifdef ESP_PLATFORM
        ESP_LOGE("STORAGE", "Device attestation key migration failed");
#else
        printf("Device attestation key migration failed\n");
#endif
    }

    init_rtc();

    phy_init();

    led_init();

    usb_init();
#ifdef ESP_PLATFORM
    esp_card_worker_init();
    // Finish board/OTP initialization before the USB task can accept requests.
    if (picokey_init() != 0) {
        picokey_boot_message("ERROR: board keys");
        return 6;
    }
    picokey_boot_message("Board keys ready");
#endif

#ifndef ENABLE_EMULATION
#ifdef ESP_PLATFORM
    ESP_ERROR_CHECK(gpio_reset_pin(BOOT_PIN));
    gpio_set_direction(BOOT_PIN, GPIO_MODE_INPUT);
    gpio_pulldown_dis(BOOT_PIN);

    tusb_cfg.descriptor.string[3] = pico_serial_str;
    if (phy_data.usb_product_present) {
        tusb_cfg.descriptor.string[2] = phy_data.usb_product;
    }
    enum { MAX_INTERFACE_STRINGS = USB_STRING_SLOT_COUNT - USB_INTERFACE_STRING_BASE };
    static char interface_strings[MAX_INTERFACE_STRINGS][32];
    unsigned count = ITF_TOTAL < MAX_INTERFACE_STRINGS ? ITF_TOTAL : MAX_INTERFACE_STRINGS;
    for (unsigned i = 0; i < count; ++i) {
        snprintf(interface_strings[i], sizeof(interface_strings[i]), "%s %s",
            tusb_cfg.descriptor.string[2], string_desc_itf[i]);
        tusb_cfg.descriptor.string[USB_INTERFACE_STRING_BASE + i] = interface_strings[i];
    }
    tusb_cfg.descriptor.string_count = USB_INTERFACE_STRING_BASE + count;
    tusb_cfg.descriptor.full_speed_config = desc_config;

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    picokey_boot_message("USB stack ready");
#else
    tusb_init();
#endif
#endif

#ifndef ESP_PLATFORM
    if (picokey_init() != 0) {
        picokey_boot_message("ERROR: board keys");
        return 6;
    }
#endif
#ifdef ENABLE_EMULATION
    // The control receiver runs on its own thread. Do not expose it until
    // storage, transport buffers and cached OTP status are initialized.
    if (emul_init("127.0.0.1", 35963) != 0) return 4;
    picokey_boot_message("Loopback ports ready");
    led_set_mode(MODE_MOUNTED);
#endif

#ifdef ESP_PLATFORM
    if (xTaskCreatePinnedToCore(core0_loop, "core0", 4096*ITF_TOTAL*2, NULL, PICOKEYS_USB_TASK_PRIORITY - 1, &hcore0, ESP32_CORE0) != pdPASS) {
        picokey_boot_message("ERROR: protocol task");
        return 7;
    }
    picokey_boot_message("Protocols ready");
    picokey_boot_complete();
    ESP_LOGI("RESOURCES", "startup_stack_min=%u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI("RESOURCES", "internal_heap_free=%u min=%u largest=%u; credential_fs_used=%lu free=%lu",
        (unsigned)heap_caps_get_free_size(caps), (unsigned)heap_caps_get_minimum_free_size(caps),
        (unsigned)heap_caps_get_largest_free_block(caps), (unsigned long)flash_used_space(),
        (unsigned long)flash_free_space());
#else
    picokey_boot_message("Protocols ready");
    picokey_boot_complete();
    core0_loop(NULL);
#endif

    return 0;
}
