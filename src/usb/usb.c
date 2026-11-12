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
#include "usb.h"
#include <stdatomic.h>
#include "led/led.h"
#include "button.h"
#include "pico_time.h"
#if defined(PICO_PLATFORM)
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#if PICO_RP2350
// OpenPGP vault frames alone can exceed 12 KiB, before nested crypto calls.
#define CARD_THREAD_STACK_SIZE 32768
#else
#define CARD_THREAD_STACK_SIZE 16384
#endif
// Keep the card worker off the scratch RAM adjacent to core 0's stack.
static uint32_t card_thread_stack[CARD_THREAD_STACK_SIZE / sizeof(uint32_t)] __attribute__((aligned(8)));
#define multicore_launch_func_core1(a) multicore_launch_core1_with_stack((void (*) (void))a, card_thread_stack, sizeof(card_thread_stack))
#endif
#include "apdu.h"
#ifndef ENABLE_EMULATION
#include "tusb.h"
#else
#include "emulation.h"
#endif

// Device specific functions
static uint32_t *timeout_counter = NULL;
static uint8_t card_locked_itf = 0; // no locked
static void *(*card_locked_func)(void *) = NULL;
uint8_t enabled_usb_itf = PHY_USB_ITF_ALL;
#ifndef ENABLE_EMULATION
static mutex_t mutex;
#endif
#if !defined(PICO_PLATFORM) && !defined(ENABLE_EMULATION) && !defined(ESP_PLATFORM)
#ifdef _MSC_VER
#include "compat/pthread_win32.h"
#endif
pthread_t hcore0, hcore1;
#endif

#ifdef USB_ITF_HID
    uint8_t ITF_HID_CTAP = ITF_INVALID, ITF_HID_KB = ITF_INVALID;
    uint8_t ITF_HID = ITF_INVALID, ITF_KEYBOARD = ITF_INVALID;
    uint8_t ITF_HID_TOTAL = 0;
    extern void hid_init(void);
#endif

#ifdef USB_ITF_CCID
    uint8_t ITF_SC_CCID = ITF_INVALID, ITF_SC_WCID = ITF_INVALID;
    uint8_t ITF_CCID = ITF_INVALID, ITF_WCID = ITF_INVALID;
    uint8_t ITF_SC_TOTAL = 0;
    extern void ccid_init(void);
#endif

#ifdef USB_ITF_LWIP
    uint8_t ITF_LWIP_NET = ITF_INVALID, ITF_LWIP = ITF_INVALID;
    uint8_t ITF_LWIP_TOTAL = 0;
    extern void lwip_init(void);
#endif
uint8_t ITF_TOTAL = 0;
static app_t **transport_apps;
static uint8_t app_transport = ITF_INVALID;
/* Smart-card authentication belongs to one transport. FIDO CBOR and native
 * OTP feature reports borrow the worker, not this authenticated context. */
static uint8_t apdu_context_owner = ITF_INVALID;

void usb_set_timeout_counter(uint8_t itf, uint32_t v) {
    timeout_counter[itf] = v;
}

queue_t usb_to_card_q = {0};
queue_t card_to_usb_q = {0};

#ifndef ENABLE_EMULATION
extern tusb_desc_device_t desc_device;
extern const char *string_desc_itf[5];
extern const char *const usb_interface_names[5];
#endif
void usb_init(void)
{
#ifndef ENABLE_EMULATION
    if (phy_data.vidpid_present) {
        desc_device.idVendor = phy_data.vid;
        desc_device.idProduct = phy_data.pid;
    }
    else {
        phy_data.vid = desc_device.idVendor;
        phy_data.pid = desc_device.idProduct;
        phy_data.vidpid_present = true;
    }
    mutex_init(&mutex);
#endif
    queue_init(&card_to_usb_q, sizeof(uint32_t), 64);
    queue_init(&usb_to_card_q, sizeof(uint32_t), 64);

    enabled_usb_itf = PHY_USB_ITF_ALL;
#ifndef ENABLE_EMULATION
    if (phy_data.enabled_usb_itf_present) {
        enabled_usb_itf = phy_data.enabled_usb_itf;
    }
#endif

#ifdef USB_ITF_HID
    ITF_HID_TOTAL = 0;
#endif
#ifdef USB_ITF_CCID
    ITF_SC_TOTAL = 0;
#endif
#ifdef USB_ITF_LWIP
    ITF_LWIP_TOTAL = 0;
#endif
    ITF_TOTAL = 0;
#ifdef USB_ITF_HID
    if (enabled_usb_itf & PHY_USB_ITF_HID) {
        ITF_HID_CTAP = ITF_HID_TOTAL++;
        ITF_HID = ITF_TOTAL++;
#ifndef ENABLE_EMULATION
        string_desc_itf[ITF_TOTAL - 1] = usb_interface_names[0];
#endif
    }
    if (enabled_usb_itf & PHY_USB_ITF_KB) {
        ITF_HID_KB = ITF_HID_TOTAL++;
        ITF_KEYBOARD = ITF_TOTAL++;
#ifndef ENABLE_EMULATION
        string_desc_itf[ITF_TOTAL - 1] = usb_interface_names[1];
#endif
    }
#endif
#ifdef USB_ITF_CCID
    if (enabled_usb_itf & PHY_USB_ITF_CCID) {
        ITF_SC_CCID = ITF_SC_TOTAL++;
        ITF_CCID = ITF_TOTAL++;
#ifndef ENABLE_EMULATION
        string_desc_itf[ITF_TOTAL - 1] = usb_interface_names[2];
#endif
    }
    if (enabled_usb_itf & PHY_USB_ITF_WCID) {
        ITF_SC_WCID = ITF_SC_TOTAL++;
        ITF_WCID = ITF_TOTAL++;
#ifndef ENABLE_EMULATION
        string_desc_itf[ITF_TOTAL - 1] = usb_interface_names[3];
#endif
    }
#endif
#ifdef USB_ITF_LWIP
    if (enabled_usb_itf & PHY_USB_ITF_LWIP) {
        ITF_LWIP_NET = ITF_LWIP_TOTAL++;
        ITF_LWIP = ITF_TOTAL++;
#ifndef ENABLE_EMULATION
        string_desc_itf[ITF_TOTAL - 1] = usb_interface_names[4];
#endif
    }
#endif
    card_locked_itf = ITF_TOTAL;
    transport_apps = calloc(ITF_TOTAL, sizeof(*transport_apps));
    if (!transport_apps) abort();
    apdu_transport_init();
    if (timeout_counter == NULL) {
        timeout_counter = (uint32_t *)calloc(ITF_TOTAL, sizeof(uint32_t));
    }
    if (!timeout_counter) abort();
#ifdef USB_ITF_HID
    if (ITF_HID_TOTAL > 0) {
        hid_init();
    }
#endif
#ifdef USB_ITF_CCID
    if (ITF_SC_TOTAL > 0) {
        ccid_init();
    }
#endif
#ifdef USB_ITF_LWIP
    if (ITF_LWIP_TOTAL > 0) {
#ifndef ESP_PLATFORM
        lwip_itf_init();
#endif
    }
#endif
#ifdef ESP_PLATFORM
    usb_desc_setup();
#endif
    set_atr();
}

#ifdef PICO_PLATFORM
extern char __end__, __HeapLimit;
extern char __StackBottom, __StackTop;
extern char __StackOneBottom, __StackOneTop;
static uint8_t reboot_temp_stack[1024] __attribute__((aligned(8)));

static inline void secure_bzero(void *ptr, size_t len) {
    volatile uint8_t *p = (volatile uint8_t *) ptr;
    while (len--) {
        *p++ = 0xFF;
    }
}

static void __attribute__((noreturn, noinline)) usb_secure_reboot_now(void) {
    uintptr_t heap_start = (uintptr_t) &__end__;
    uintptr_t heap_end = (uintptr_t) &__HeapLimit;
    uintptr_t stack0_start = (uintptr_t) &__StackBottom;
    uintptr_t stack0_end = (uintptr_t) &__StackTop;
    uintptr_t stack1_start = (uintptr_t) &__StackOneBottom;
    uintptr_t stack1_end = (uintptr_t) &__StackOneTop;

    (void) save_and_disable_interrupts();
    multicore_reset_core1();

    if (stack1_end > stack1_start) {
        secure_bzero((void *) stack1_start, stack1_end - stack1_start);
    }
    secure_bzero(card_thread_stack, sizeof(card_thread_stack));

    uintptr_t new_sp = (((uintptr_t) reboot_temp_stack) + sizeof(reboot_temp_stack)) & ~(uintptr_t)0x7;
#if defined(__arm__) || defined(__thumb__)
    __asm volatile ("msr msp, %0" :: "r"(new_sp) : "memory");
#endif

    if (heap_end > heap_start) {
        secure_bzero((void *) heap_start, heap_end - heap_start);
    }
    if (stack0_end > stack0_start) {
        secure_bzero((void *) stack0_start, stack0_end - stack0_start);
    }
    secure_bzero(reboot_temp_stack, sizeof(reboot_temp_stack));

    reset_usb_boot(0, 0);
    while (true) {
        tight_loop_contents();
    }
}
#endif

uint32_t timeout = 0;
void timeout_stop(void) {
    timeout = 0;
}

void timeout_start(void) {
    timeout = board_millis();
}

bool is_busy(void) {
    return timeout > 0;
}

#ifdef ESP_PLATFORM
/* Reserve one worker at startup. Deleting and immediately creating tasks
 * during a transport switch can exhaust the heap before the idle task has
 * reclaimed the previous stack, leaving the USB loop waiting for an ACK
 * from a worker that never started. */
static TaskHandle_t esp_card_worker;
static void *(*esp_card_function)(void *);

static void esp_card_worker_loop(void *arg) {
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_card_function(NULL);
        hcore1 = NULL;
        core1_task_running = false;
    }
}

void esp_card_worker_init(void) {
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(esp_card_worker_loop, "card",
        4096 * ITF_TOTAL * 2, NULL, PICOKEYS_USB_TASK_PRIORITY - 2,
        &esp_card_worker, ESP32_CORE1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void esp_card_worker_launch(void *(*func)(void *)) {
    esp_card_function = func;
    hcore1 = esp_card_worker;
    core1_task_running = true;
    xTaskNotifyGive(esp_card_worker);
}

#endif

bool card_busy_on_other_interface(uint8_t itf) {
    return is_busy() && card_locked_itf != itf;
}

void usb_send_event(uint32_t flag) {
    if (flag == EV_CMD_AVAILABLE) cancel_button = false;
#ifndef ENABLE_EMULATION
    mutex_enter_blocking(&mutex);
#endif
    queue_add_blocking(&usb_to_card_q, &flag);
    if (flag == EV_CMD_AVAILABLE) {
        timeout_start();
    }
#ifndef ENABLE_EMULATION
    mutex_exit(&mutex);
#endif

    if (flag != EV_CMD_AVAILABLE) {
        uint32_t m;
        do {
            queue_remove_blocking(&card_to_usb_q, &m);
        } while (m != flag + 1);
    }
}

void card_init_core1(void) {
    low_flash_init_core1();
}

volatile uint16_t finished_data_size = 0;

void card_start(uint8_t itf, void *(*func)(void *)) {
    timeout_start();
    if (card_locked_itf != itf || card_locked_func != func) {
        if (card_locked_itf != ITF_TOTAL || card_locked_func != NULL) {
            card_exit();
        }
        // Switching the shared worker must not deselect the other USB
        // transport's smart-card application or invalidate its PIN session.
        if (app_transport != itf) {
            if (app_transport < ITF_TOTAL) transport_apps[app_transport] = current_app;
            current_app = transport_apps[itf];
            app_transport = itf;
        }
        if (func) {
            multicore_reset_core1();
#ifdef PICO_PLATFORM
            secure_bzero(card_thread_stack, sizeof(card_thread_stack));
#endif
            multicore_launch_func_core1(func);
        }
        led_set_mode(MODE_MOUNTED);
        card_locked_itf = itf;
        card_locked_func = func;
    }
}

bool card_claim_apdu_context(uint8_t itf) {
    if (itf >= ITF_TOTAL) return false;
    if (apdu_context_owner == itf) return true;
    // Like CanoKey's applet session owner, refuse to steal an unfinished APDU.
    if (!apdu_transport_can_yield(apdu_context_owner)) return false;
    if (apdu_context_owner < ITF_TOTAL) card_reset_app(apdu_context_owner);
    apdu_context_owner = itf;
    return true;
}

void card_reset_app(uint8_t itf) {
    if (itf >= ITF_TOTAL) return;
    apdu_reset_transport(itf);
    app_t *selected = app_transport == itf ? current_app : transport_apps[itf];
    if (selected && selected->unload) selected->unload();
    transport_apps[itf] = NULL;
    if (app_transport == itf) current_app = NULL;
    if (apdu_context_owner == itf) apdu_context_owner = ITF_INVALID;
}

void card_exit(void) {
    if (card_locked_itf != ITF_TOTAL || card_locked_func != NULL) {
        usb_send_event(EV_EXIT);
#ifdef ESP_PLATFORM
        while (core1_task_running) vTaskDelay(1);
#elif defined(ENABLE_EMULATION)
        pthread_join(hcore1, NULL);
#endif
        uint32_t m;
        while (queue_is_empty(&usb_to_card_q) == false) {
            if (queue_try_remove(&usb_to_card_q, &m) == false) {
                break;
            }
        }
        while (queue_is_empty(&card_to_usb_q) == false) {
#ifndef ENABLE_EMULATION
            mutex_enter_blocking(&mutex);
#endif
            bool removed = queue_try_remove(&card_to_usb_q, &m);
#ifndef ENABLE_EMULATION
            mutex_exit(&mutex);
#endif
            if (!removed) break;
        }
        led_set_mode(MODE_SUSPENDED);
#ifdef ESP_PLATFORM
        hcore1 = NULL;
#endif
    }
    card_locked_itf = ITF_TOTAL;
    card_locked_func = NULL;
}
extern void hid_task(void);
extern void ccid_task(void);
WEAK void otp_hid_task(void) {}
static atomic_bool session_reset_pending;
void usb_session_reset_request(void) { atomic_store(&session_reset_pending, true); }
bool usb_session_reset_pending(void) { return atomic_load(&session_reset_pending); }
WEAK void otp_hid_session_reset(void) {}
#ifdef USB_ITF_HID
extern void hid_session_reset(void);
#endif
#ifdef USB_ITF_CCID
extern void ccid_session_reset(void);
#endif
#ifdef ENABLE_EMULATION
extern void emul_session_reset(void);
#endif
void usb_task(void) {
    if (usb_session_reset_pending()) {
        // Reset revokes the whole bus session. Let the worker unwind before
        // clearing its APDU, keys, queues or transport buffers.
        cancel_button = true;
        if (is_busy()) (void)card_status(card_locked_itf);
        if (is_busy()) return;
        card_exit();
        for (uint8_t itf = 0; itf < ITF_TOTAL; ++itf) card_reset_app(itf);
#ifdef USB_ITF_HID
        hid_session_reset();
#endif
#ifdef USB_ITF_CCID
        ccid_session_reset();
#endif
        otp_hid_session_reset();
#ifdef ENABLE_EMULATION
        emul_session_reset();
#endif
#ifdef ENABLE_EMULATION
        led_set_mode(MODE_MOUNTED);
#else
        led_set_mode(tud_mounted() ? MODE_MOUNTED : MODE_NOT_MOUNTED);
#endif
        atomic_store(&session_reset_pending, false);
        return;
    }
    otp_hid_task();
#ifdef USB_ITF_HID
    hid_task();
#endif
#ifdef ENABLE_EMULATION
    emul_task();
#else
#ifdef USB_ITF_CCID
    ccid_task();
#endif
#endif
}

int card_status(uint8_t itf) {
    if (card_locked_itf == itf) {
        if (timeout == 0) {
            return PICOKEYS_ERR_FILE_NOT_FOUND;
        }
        uint32_t m = 0x0;
#ifndef ENABLE_EMULATION
        mutex_enter_blocking(&mutex);
#endif
        bool has_m = queue_try_remove(&card_to_usb_q, &m);
#ifndef ENABLE_EMULATION
        mutex_exit(&mutex);
#endif
        //if (m != 0)
        //    printf("\n ------ M = %lu\n",m);
        if (has_m) {
            if (m == EV_EXEC_FINISHED) {
                timeout_stop();
                if (led_get_mode() == MODE_PROCESSING) {
                    led_set_mode(MODE_MOUNTED);
                }
                return PICOKEYS_OK;
            }
            else if ((m & 0xffu) == EV_PRESS_BUTTON) {
                uint32_t seconds = EV_PRESS_BUTTON_GET_TIMEOUT(m);
                if (!(m & EV_PRESS_BUTTON_EXPLICIT_TIMEOUT)) {
                    /* Legacy OpenPGP/PIV requests mean required confirmation,
                     * not an explicitly disabled (zero-second) FIDO wait. */
                    seconds = button_timeout_seconds();
                    if (seconds == 0) seconds = 15;
                }
                button_wait_start_timeout(seconds);
            }
#ifdef PICO_PLATFORM
            else if (m == EV_RESET) {
                usb_secure_reboot_now();
            }
#endif
            return PICOKEYS_ERR_FILE_NOT_FOUND;
        }
        else {
            if (timeout > 0) {
                if (timeout + timeout_counter[itf] < board_millis()) {
                    timeout = board_millis();
                    return PICOKEYS_ERR_BLOCKED;
                }
            }
        }
    }
    return PICOKEYS_ERR_FILE_NOT_FOUND;
}

#if !defined(USB_ITF_CCID) && !defined(ENABLE_EMULATION)
#include "device/usbd_pvt.h"
// Keep the lifecycle hook even in HID-only builds. This driver claims no
// interface; TinyUSB still invokes its reset alongside the built-in classes.
static void session_driver_init(void) {}
static void session_driver_reset(uint8_t rhport) { (void)rhport; usb_session_reset_request(); }
static uint16_t session_driver_open(uint8_t rhport, const tusb_desc_interface_t *desc, uint16_t len) {
    (void)rhport; (void)desc; (void)len; return 0;
}
static const usbd_class_driver_t session_driver = {
    .init = session_driver_init, .reset = session_driver_reset, .open = session_driver_open,
};
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &session_driver;
}
#endif
