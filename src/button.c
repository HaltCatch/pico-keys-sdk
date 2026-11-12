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
#include "led/led.h"
#include "pico_time.h"
#if defined(PICO_PLATFORM)
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/gpio.h"
#if PICO_RP2350
#include "pico/flash.h"
#endif
#elif defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "esp_log.h"
#elif defined(ENABLE_EMULATION)
#include <stdio.h>
#endif
#include "usb.h"
#include "signal.h"

#if defined(ESP_PLATFORM)
#define PRESENCE_LOG(event) ESP_LOGI("USER_PRESENCE", "%s", event)
#elif defined(ENABLE_EMULATION)
#define PRESENCE_LOG(event) fprintf(stderr, "USER_PRESENCE: %s\n", event)
#else
#define PRESENCE_LOG(event) ((void)0)
#endif

extern void execute_tasks(void);

int (*button_pressed_cb)(uint8_t) = NULL;

static bool req_button_pending = false;
static bool async_button_wait = false;
static bool async_button_pressed = false;
static uint32_t async_button_started = 0;
static uint32_t async_button_timeout = 0;
static uint32_t async_button_led_mode = MODE_MOUNTED;

bool is_req_button_pending(void) {
    return req_button_pending;
}

atomic_bool cancel_button = false;
bool touch_accept_button = false;
volatile bool force_button_wait = false;

#if !defined(ENABLE_EMULATION)
#ifdef ESP_PLATFORM
static bool picok_board_button_read(void) {
    int boot_state = gpio_get_level(BOOT_PIN);
    return boot_state == 0;
}
#elif defined(PICO_PLATFORM)
static bool __no_inline_not_in_flash_func(picok_get_bootsel_button)(void) {
    const uint CS_PIN_INDEX = 1;

    // Must disable interrupts, as interrupt handlers may be in flash, and we
    // are about to temporarily disable flash access!
    uint32_t flags = save_and_disable_interrupts();

    // Set chip select to Hi-Z
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Note we can't call into any sleep functions in flash right now
    for (volatile int i = 0; i < 1000; ++i);

    // The HI GPIO registers in SIO can observe and control the 6 QSPI pins.
    // Note the button pulls the pin *low* when pressed.
#ifdef PICO_RP2040
    #define CS_BIT (1u << 1)
#else
    #define CS_BIT SIO_GPIO_HI_IN_QSPI_CSN_BITS
#endif
    bool button_state = !(sio_hw->gpio_hi_in & CS_BIT);

    // Need to restore the state of chip select, else we are going to have a
    // bad time when we return to code in flash!
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);

    return button_state;
}
#if PICO_RP2350
static void __no_inline_not_in_flash_func(picok_sample_bootsel)(void *sample) {
    *(bool *)sample = picok_get_bootsel_button();
}
#endif
static bool picok_board_button_read(void) {
#if PICO_RP2350
    // BOOTSEL temporarily disconnects XIP flash. Disabling core 0 interrupts
    // alone does not protect the protocol worker executing from flash on core 1.
    static bool previous;
    if (!multicore_lockout_ready()) return previous;
    bool sampled = previous;
    if (flash_safe_execute(picok_sample_bootsel, &sampled, 1) == PICO_OK) {
        previous = sampled;
    }
    // A busy worker is not a button release; retry on the next poll.
    return previous;
#else
    return picok_get_bootsel_button();
#endif
}
#else
static bool picok_board_button_read(void) {
    return true; // always unpressed
}
#endif
#else
static bool emulated_button_down;
static pthread_mutex_t emulated_button_lock = PTHREAD_MUTEX_INITIALIZER;
void emul_set_button(bool down) {
    pthread_mutex_lock(&emulated_button_lock);
    emulated_button_down = down;
    pthread_mutex_unlock(&emulated_button_lock);
}
static bool picok_board_button_read(void) {
    pthread_mutex_lock(&emulated_button_lock);
    bool down = emulated_button_down;
    pthread_mutex_unlock(&emulated_button_lock);
    static uint32_t auto_started;
    const char *automatic = getenv("TDONGLE_EMUL_AUTO_TOUCH");
    if (automatic && strcmp(automatic, "1") == 0 && req_button_pending) {
        if (!auto_started) auto_started = board_millis();
        down = (uint32_t)(board_millis() - auto_started) < 50;
    } else auto_started = 0;
    return down;
}
#endif
static bool button_pressed_state = false;
static uint32_t button_pressed_time = 0;
static uint8_t button_press = 0;
static bool suppress_otp_until_release;
static bool button_hold_state = false;
static uint32_t button_hold_started = 0;
static uint32_t button_last_poll = 0;
volatile uint32_t button_pressed_duration = 0;

void button_wait_start(void) {
    button_wait_start_timeout(button_timeout_seconds());
}

void button_wait_start_timeout(uint32_t timeout_seconds) {
    // Cancellation can arrive after dispatch but before the worker asks for
    // presence. Only a new command may clear it, never a late wait event.
    if (cancel_button) {
        uint32_t flag = EV_BUTTON_CANCELLED;
        queue_try_add(&usb_to_card_q, &flag);
        return;
    }
    uint32_t button_timeout = timeout_seconds * 1000;
    if (button_timeout == 0 && !force_button_wait) {
        signal_emit(SIGNAL_USER_PRESENCE_COMPLETED);
        uint32_t flag = EV_BUTTON_PRESSED;
        queue_try_add(&usb_to_card_q, &flag);
        return;
    }
    if (button_timeout == 0) {
        button_timeout = 30000;
    }
    signal_user_presence_request_data_t data = {
        .timeout = button_timeout / 1000,
    };
    signal_emit_param(SIGNAL_USER_PRESENCE_REQUEST, &data);
    async_button_wait = true;
    async_button_pressed = picok_board_button_read();
    async_button_started = board_millis();
    async_button_timeout = button_timeout;
    async_button_led_mode = led_get_mode();
    req_button_pending = true;
    led_set_mode(MODE_BUTTON);
    PRESENCE_LOG("waiting");
}

void button_wait_poll(void) {
    if (!async_button_wait) {
        return;
    }
    bool pressed = picok_board_button_read();
    uint32_t now = board_millis();
    if (!async_button_pressed && pressed) {
        async_button_pressed = true;
    }
    button_event_t result = BUTTON_EV_NONE;
    if (cancel_button) {
        result = BUTTON_EV_CANCELLED;
    }
    else if ((uint32_t)(now - async_button_started) >= async_button_timeout ||
             (async_button_pressed && (uint32_t)(now - async_button_started) >= 15000)) {
        result = BUTTON_EV_TIMEOUT;
    }
    else if (async_button_pressed && !pressed) {
        result = BUTTON_EV_PRESSED;
    }
    if (result == BUTTON_EV_NONE) {
        return;
    }
    async_button_wait = false;
    req_button_pending = false;
    led_set_mode(async_button_led_mode);
    uint32_t flag = 0;
    if (result == BUTTON_EV_PRESSED) {
        suppress_otp_until_release = true;
        flag = EV_BUTTON_PRESSED;
        PRESENCE_LOG("confirmed");
    }
    else if (result == BUTTON_EV_TIMEOUT) {
        flag = EV_BUTTON_TIMEOUT;
        PRESENCE_LOG("timed out");
    }
    else if (result == BUTTON_EV_CANCELLED) {
        flag = EV_BUTTON_CANCELLED;
        PRESENCE_LOG("cancelled");
    }
    queue_try_add(&usb_to_card_q, &flag);
    if (result == BUTTON_EV_PRESSED) {
        signal_emit(SIGNAL_USER_PRESENCE_COMPLETED);
    }
    else if (result == BUTTON_EV_TIMEOUT) {
        signal_emit(SIGNAL_USER_PRESENCE_TIMEOUT);
    }
    else {
        signal_emit(SIGNAL_USER_PRESENCE_CANCELLED);
    }
}


uint32_t button_timeout_seconds(void) {
    /* Disabled by default. As LED may not be properly configured,
       it will not be possible to indicate button press unless it
       is commissioned. */
    return phy_data.up_btn_present ? phy_data.up_btn : 15;
}

void button_task(void) {
    uint32_t now = board_millis();
    if (now > 1000 && now - button_last_poll >= 10 && (async_button_wait || !is_busy())) { // wait 1 second to boot up
#if defined(PICO_PLATFORM) && !PICO_RP2350
        // RP2350 sampling already coordinates both cores in flash_safe_execute.
        // A second lockout here would nest the FIFO handshake and lose samples.
        if (!multicore_lockout_start_timeout_us(1000)) {
            return;
        }
#endif
        bool current_button_state = picok_board_button_read();
#if defined(PICO_PLATFORM) && !PICO_RP2350
        multicore_lockout_end_timeout_us(1000);
#endif
        button_last_poll = now;
        if (async_button_wait) {
            button_wait_poll();
            return;
        }
        if (suppress_otp_until_release) {
            if (!current_button_state) suppress_otp_until_release = false;
            button_pressed_state = current_button_state;
            button_pressed_time = button_press = 0;
            return;
        }
        if (current_button_state && !button_hold_state) {
            button_hold_started = now;
            button_pressed_duration = 0;
        }
        else if (!current_button_state && button_hold_state) {
            button_pressed_duration = button_hold_started == 0 ? 0 : now - button_hold_started;
            button_hold_started = 0;
        }
        else if (current_button_state && button_hold_state && button_hold_started != 0) {
            button_pressed_duration = now - button_hold_started;
        }
        button_hold_state = current_button_state;
        if (button_pressed_cb) {
            if (current_button_state != button_pressed_state) {
                if (current_button_state == false) { // unpressed
                    if (button_pressed_time == 0 || button_pressed_time + 1000 > now) {
                        button_press++;
                    }
                    button_pressed_time = now;
                }
                button_pressed_state = current_button_state;
            }
            if (button_pressed_time > 0 && button_press > 0 && button_pressed_time + 1000 < now && button_pressed_state == false) {
                if (button_pressed_cb != NULL) {
                    (*button_pressed_cb)(button_press);
                }
                button_pressed_time = button_press = 0;
            }
        }
    }
}
