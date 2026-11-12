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
#include "led/led.h"
#ifdef PICO_PLATFORM
#include "hardware/gpio.h"
#endif

#ifdef CYW43_WL_GPIO_LED_PIN

#include "pico/cyw43_arch.h"

void led_driver_init_cyw43(void);
void led_driver_color_cyw43(uint8_t color, uint32_t led_brightness, float progress);
static bool led_render_cyw43_status(uint32_t now);

void led_driver_init_cyw43(void) {
    cyw43_arch_init();
}

void led_driver_color_cyw43(uint8_t color, uint32_t led_brightness, float progress) {
    static int previous_gpio = -1, previous_state = -1;
    (void)led_brightness;
    (void)color;
    uint8_t gpio = CYW43_WL_GPIO_LED_PIN;
    if (phy_data.led_gpio_present) {
        gpio = phy_data.led_gpio;
    }
    bool on = progress >= 0.5;
    if (gpio != previous_gpio || on != previous_state) {
        cyw43_arch_gpio_put(gpio, on);
        previous_gpio = gpio;
        previous_state = on;
    }
}

led_driver_t led_driver_cyw43 = {
    .init = led_driver_init_cyw43,
    .set_color = led_driver_color_cyw43,
    .render_status = led_render_cyw43_status,
};

static bool led_render_cyw43_status(uint32_t now) {
    if (led_driver != &led_driver_cyw43) return false;
    static uint32_t previous_mode = UINT32_MAX, started;
    uint32_t mode = led_get_mode();
    if (mode != previous_mode) {
        previous_mode = mode;
        started = now;
    }
    uint32_t elapsed = now - started;
    bool on;
    if (mode == MODE_BUTTON) {
        // A monochrome LED cannot express the RGB board's confirmation color.
        uint32_t phase = elapsed % 1000;
        on = phase < 150 || (phase >= 300 && phase < 450);
    } else if (mode == MODE_MOUNTED) {
        on = (phy_data.opts & PHY_OPT_LED_STEADY) || elapsed % 3000 < 40;
    } else if (mode == MODE_PROCESSING) {
        on = elapsed % 500 < 80;
    } else if (mode == MODE_NOT_MOUNTED) {
        on = elapsed % 1000 < 125;
    } else if (mode == MODE_SUSPENDED || mode == MODE_ALWAYS_OFF) {
        on = false;
    } else if (mode == MODE_ALWAYS_ON) {
        on = true;
    } else return false;
    led_driver_color_cyw43(LED_COLOR_GREEN, MAX_BTNESS, on ? 1.0f : 0.0f);
    return true;
}

#endif
