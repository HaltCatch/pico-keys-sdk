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

#include <stdio.h>
#include <stdatomic.h>
#include "picokeys.h"
#include "serial.h"
#include "pico_time.h"
#ifndef ENABLE_EMULATION
#include "tusb.h"
#if defined(ESP_PLATFORM)
#include "esp_log.h"
static portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
#endif
#else
#include "emulation.h"
#endif
#include "ctap_hid.h"
#include "picokeys_version.h"
#include "apdu.h"
#include "usb.h"
#include "button.h"

extern void init_fido(void);
bool is_nk = false;
uint8_t (*get_version_major)(void) = NULL;
uint8_t (*get_version_minor)(void) = NULL;

#define CTAPHID_KEEPALIVE_CANCEL_STATUS 0x2D

static usb_buffer_t *hid_rx = NULL, *hid_tx = NULL;
// Continuations carry 59 payload bytes, not 64. Hold a complete supported
// command plus a control report while OTP/CCID owns the shared worker.
#define HID_REPORT_QUEUE_DEPTH (2u + (USB_BUFFER_SIZE + HID_RPT_SIZE - 6u) / (HID_RPT_SIZE - 5u))
static queue_t hid_reports;
static bool hid_reports_ready;

PACK(
typedef struct msg_packet {
    uint16_t len;
    uint16_t current_len;
    uint8_t data[CTAP_MAX_PACKET_SIZE];
}) msg_packet_t;

msg_packet_t msg_packet = { 0 };

static uint16_t *send_buffer_size = NULL;
static write_status_t *last_write_result = NULL;
/* A refusal uses separate backing and must not advance the active reply. */
static atomic_bool hid_sideband_pending;
static bool hid_resync_pending;
static CTAPHID_FRAME hid_resync_request;

CTAPHID_FRAME *ctap_req = NULL, *ctap_resp = NULL;
static void send_keepalive(void);
int driver_process_usb_packet_hid(uint16_t read);
int driver_write_hid(uint8_t itf, const_byte_array_t buffer);
static int driver_process_usb_nopacket_hid(void);
void hid_init(void);
void hid_task(void);
#ifdef ENABLE_EMULATION
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen);
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize);
#endif

void hid_init(void) {
    if (ITF_HID_TOTAL == 0) {
        return;
    }
    if (send_buffer_size == NULL) {
        send_buffer_size = (uint16_t *)calloc(ITF_HID_TOTAL, sizeof(uint16_t));
    }
    if (last_write_result == NULL) {
        last_write_result = (write_status_t *)calloc(ITF_HID_TOTAL, sizeof(write_status_t));
    }
    if (hid_rx == NULL) {
        hid_rx = (usb_buffer_t *)calloc(ITF_HID_TOTAL, sizeof(usb_buffer_t));
    }
    if (hid_tx == NULL) {
        hid_tx = (usb_buffer_t *)calloc(ITF_HID_TOTAL, sizeof(usb_buffer_t));
    }
    if (!send_buffer_size || !last_write_result || !hid_rx || !hid_tx) abort();
    if (ITF_HID_CTAP != ITF_INVALID && !hid_reports_ready) {
        queue_init(&hid_reports, HID_RPT_SIZE, HID_REPORT_QUEUE_DEPTH);
        hid_reports_ready = true;
    }
}

int driver_init_hid(void) {
#ifndef ENABLE_EMULATION
    static bool _init = false;
    if (_init == false) {
#if defined(ESP_PLATFORM)
        const tusb_rhport_init_t rh_init = {
            .role = TUSB_ROLE_DEVICE,
            .speed = TUSB_SPEED_AUTO,
        };

        tusb_init(BOARD_TUD_RHPORT, &rh_init);
#else
        tud_init(BOARD_TUD_RHPORT);
#endif
        _init = true;
    }
#endif
    ctap_req = (CTAPHID_FRAME *) (hid_rx[ITF_HID_CTAP].buffer + hid_rx[ITF_HID_CTAP].r_ptr);
    apdu.header = ctap_req->init.data;

    ctap_resp = (CTAPHID_FRAME *) (hid_tx[ITF_HID_CTAP].buffer);
    apdu.rdata = ctap_resp->init.data;
    memset(ctap_resp, 0, sizeof(CTAPHID_FRAME));

    usb_set_timeout_counter(ITF_HID, 200);

    is_nk = false;

    hid_tx[ITF_HID_CTAP].w_ptr = hid_tx[ITF_HID_CTAP].r_ptr = 0;
    send_buffer_size[ITF_HID_CTAP] = 0;
    return 0;
}

//--------------------------------------------------------------------+
// USB HID
//--------------------------------------------------------------------+

uint16_t (*hid_get_report_cb)(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t) = NULL;
// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    if (usb_session_reset_pending()) return 0;
    // TODO not Implemented
    (void) itf;
    (void) report_id;
    (void) report_type;
    memset(buffer, 0, reqlen);
    DEBUG_PAYLOAD(buffer, reqlen);
    if (hid_get_report_cb) {
        uint16_t report_size = hid_get_report_cb(itf, report_id, report_type, buffer, reqlen);
#ifdef ESP_PLATFORM
        if (report_size == 0 || report_size > reqlen) {
            ESP_LOGW("USB_HID", "GET_REPORT rejected: interface=%u id=%u type=%u requested=%u returned=%u",
                (unsigned)itf, (unsigned)report_id, (unsigned)report_type,
                (unsigned)reqlen, (unsigned)report_size);
        }
#endif
        return report_size <= reqlen ? report_size : 0;
    }
    return reqlen;
}

static uint32_t hid_write_offset(uint16_t size, uint16_t offset) {
    if (hid_tx[ITF_HID_CTAP].buffer[offset] != 0x81) {
        DEBUG_PAYLOAD(&hid_tx[ITF_HID_CTAP].buffer[offset], size);
    }
    hid_tx[ITF_HID_CTAP].w_ptr += size + offset;
    hid_tx[ITF_HID_CTAP].r_ptr += offset;
    return size;
}

static uint32_t hid_write(uint16_t size) {
    return hid_write_offset(size, 0);
}

#ifndef ENABLE_EMULATION
static uint8_t keyboard_buffer[256];
static uint16_t keyboard_buffer_len = 0;
static const uint8_t conv_table[128][2] =  { HID_ASCII_TO_KEYCODE };
static uint16_t keyboard_w = 0;
static bool sent_key = false;
static bool keyboard_encode = false;

void add_keyboard_buffer(const_byte_array_t data, bool encode) {
    keyboard_buffer_len = (uint16_t)MIN(sizeof(keyboard_buffer), data.len);
    keyboard_w = 0;
    sent_key = false;
    memcpy(keyboard_buffer, data.data, keyboard_buffer_len);
    keyboard_encode = encode;
}

void append_keyboard_buffer(const_byte_array_t data) {
    if (data.len <= sizeof(keyboard_buffer) - keyboard_buffer_len) {
        memcpy(keyboard_buffer + keyboard_buffer_len, data.data, MIN(sizeof(keyboard_buffer) - keyboard_buffer_len, data.len));
        keyboard_buffer_len += (uint16_t)MIN(sizeof(keyboard_buffer) - keyboard_buffer_len, data.len);
    }
}

static void send_hid_report(uint8_t report_id) {
    if (!tud_hid_ready()) {
        return;
    }

    switch (report_id) {
        case REPORT_ID_KEYBOARD: {
            if (keyboard_w < keyboard_buffer_len) {
                if (sent_key == false) {
                    uint8_t keycode[6] = { 0 };
                    uint8_t modifier = 0;
                    uint8_t chr = keyboard_buffer[keyboard_w];
                    if (keyboard_encode) {
                        if (chr >= sizeof(conv_table) / sizeof(conv_table[0])) {
                            ++keyboard_w;
                            return; // ASCII conversion has no mapping for this byte.
                        }
                        if (conv_table[chr][0]) {
                            modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
                        }
                        keycode[0] = conv_table[chr][1];
                    }
                    else {
                        if (chr & 0x80) {
                            modifier = KEYBOARD_MODIFIER_LEFTSHIFT;
                        }
                        keycode[0] = chr & 0x7f;
                    }
                    if (tud_hid_n_keyboard_report(ITF_HID_KB, REPORT_ID_KEYBOARD, modifier, keycode) == true) {
                        sent_key = true;
                    }
                }
                else {
                    if (tud_hid_n_keyboard_report(ITF_HID_KB, REPORT_ID_KEYBOARD, 0, NULL) == true) {
                        keyboard_w++;
                        sent_key = false;

                    }
                }
            }
            else if (keyboard_w == keyboard_buffer_len && keyboard_buffer_len > 0) {
                keyboard_w = keyboard_buffer_len = 0;
            }
        }
        break;

        default: break;
    }
}
#endif

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    if (usb_session_reset_pending()) return;
    //printf("report_complete %d %d %d\n", instance, len, send_buffer_size[instance]);
    if (instance == ITF_HID_CTAP && len == 64) {
#ifdef ESP_PLATFORM
        taskENTER_CRITICAL(&mutex);
#endif
        if (atomic_exchange(&hid_sideband_pending, false)) {
            last_write_result[instance] = WRITE_SUCCESS;
#ifdef ESP_PLATFORM
            taskEXIT_CRITICAL(&mutex);
#endif
            return;
        }
        CTAPHID_FRAME *req = (CTAPHID_FRAME *) report;
        if (last_write_result[instance] == WRITE_PENDING) {
            last_write_result[instance] = WRITE_SUCCESS;
            if (FRAME_TYPE(req) == TYPE_INIT) {
                if (req->init.cmd != CTAPHID_KEEPALIVE) {
                    send_buffer_size[instance] -= MIN(64 - 7, send_buffer_size[instance]);
                }
            }
            else {
                send_buffer_size[instance] -= MIN(64 - 5, send_buffer_size[instance]);
            }
        }
        if (last_write_result[instance] == WRITE_SUCCESS) {
            if (FRAME_TYPE(req) != TYPE_INIT || req->init.cmd != CTAPHID_KEEPALIVE) {
                if (send_buffer_size[instance] > 0) {
                    ctap_resp = (CTAPHID_FRAME *) ((uint8_t *) ctap_resp + 64 - 5);
                    uint8_t seq = FRAME_TYPE(req) == TYPE_INIT ? 0 : FRAME_SEQ(req) + 1;
                    ctap_resp->cid = req->cid;
                    ctap_resp->cont.seq = seq;

                    hid_tx[ITF_HID_CTAP].r_ptr += 64 - 5;
                }
                else {
                    hid_tx[ITF_HID_CTAP].r_ptr += 64;
                }
            }
        }
        if (hid_tx[ITF_HID_CTAP].r_ptr >= hid_tx[ITF_HID_CTAP].w_ptr) {
            hid_tx[ITF_HID_CTAP].r_ptr = hid_tx[ITF_HID_CTAP].w_ptr = 0;
        }
#ifdef ESP_PLATFORM
        taskEXIT_CRITICAL(&mutex);
#endif
    }
}

int driver_write_hid(uint8_t itf, const_byte_array_t buffer) {
    if ((!buffer.data && buffer.len > 0) || buffer.len > UINT16_MAX) {
        return 0;
    }
    uint16_t buffer_len = (uint16_t)buffer.len;
    if (last_write_result[itf] == WRITE_PENDING) {
        return 0;
    }
    // The USB task may complete the transfer before this call returns.
    last_write_result[itf] = WRITE_PENDING;
    bool r = tud_hid_n_report(itf, 0, buffer.data, buffer_len);
    if (!r) {
        last_write_result[itf] = WRITE_FAILED;
        return 0;
    }
    return buffer_len > 64 ? 64 : buffer_len;
}

int (*hid_set_report_cb)(uint8_t, uint8_t, hid_report_type_t, uint8_t const *, uint16_t) = NULL;

static void process_hid_report(const uint8_t *buffer) {
    usb_buffer_t *rx = &hid_rx[ITF_HID_CTAP];
    memcpy(rx->buffer + rx->w_ptr, buffer, HID_RPT_SIZE);
    rx->w_ptr += HID_RPT_SIZE;
    if (driver_process_usb_packet_hid(HID_RPT_SIZE) == 0) {
        driver_process_usb_nopacket_hid();
    }
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    if (usb_session_reset_pending()) return;
    // This example doesn't use multiple report and report ID
    (void) itf;
    (void) report_id;
    (void) report_type;
    if (!hid_set_report_cb || hid_set_report_cb(itf, report_id, report_type, buffer, bufsize) == 0) {
        if (itf == ITF_HID_CTAP && bufsize == HID_RPT_SIZE) {
            // Every platform shares the same APDU worker. USB reception must
            // never replace an OTP/CCID command that the worker still owns.
            if (hid_reports_ready) queue_try_add(&hid_reports, buffer);
        }
    }
}

uint32_t last_cmd_time = 0, last_packet_time = 0;
int ctap_error(uint8_t error) {
    memset((uint8_t *)ctap_resp, 0, sizeof(CTAPHID_FRAME));
    ctap_resp->cid = ctap_req->cid;
    ctap_resp->init.cmd = CTAPHID_ERROR;
    ctap_resp->init.bcntl = 1;
    ctap_resp->init.data[0] = error;
    hid_write(64);
    last_packet_time = 0;
    msg_packet.len = msg_packet.current_len = 0;
    return 0;
}

uint8_t last_cmd = 0;
uint8_t last_seq = 0;
CTAPHID_FRAME last_req = { 0 };
uint32_t lock = 0;
uint32_t lock_cid = 0;
static uint32_t next_cid = 1;

static uint32_t allocate_cid(void) {
    uint32_t cid;
    do {
        cid = next_cid++;
        if (next_cid == 0 || next_cid == CID_BROADCAST) {
            next_cid = 1;
        }
    } while (cid == 0 || cid == CID_BROADCAST);
    return cid;
}

uint8_t thread_type = 0; //1 is APDU, 2 is CBOR
extern int cbor_process(uint8_t last_cmd, const uint8_t *data, size_t len);
static uint32_t last_keepalive_time = 0;

int driver_process_usb_nopacket_hid(void) {
    if (msg_packet.len && last_packet_time > 0 &&
        (uint32_t)(board_millis() - last_packet_time) >= 500) {
        ctap_error(CTAP1_ERR_MSG_TIMEOUT);
        last_packet_time = 0;
        msg_packet.len = msg_packet.current_len = 0;
    }
    return 0;
}

extern const uint8_t fido_aid[], u2f_aid[], oath_aid[];
extern void *cbor_thread(void *);

uint16_t *get_send_buffer_size(uint8_t itf) {
    return &send_buffer_size[itf];
}

int driver_process_usb_packet_hid(uint16_t read) {
    int apdu_sent = 0;
    if (read == HID_RPT_SIZE) {
        CTAPHID_FRAME *incoming = (CTAPHID_FRAME *)(hid_rx[ITF_HID_CTAP].buffer + hid_rx[ITF_HID_CTAP].r_ptr);
        if (FRAME_TYPE(incoming) == TYPE_INIT && incoming->init.cmd == CTAPHID_CANCEL) {
            // Preserve the worker's active APDU/CBOR response buffer and command.
            if (MSG_LEN(incoming) == 0 && is_busy() && incoming->cid == last_req.cid)
                cancel_button = true;
            hid_rx[ITF_HID_CTAP].r_ptr += HID_RPT_SIZE;
            if (hid_rx[ITF_HID_CTAP].r_ptr >= hid_rx[ITF_HID_CTAP].w_ptr)
                hid_rx[ITF_HID_CTAP].r_ptr = hid_rx[ITF_HID_CTAP].w_ptr = 0;
            return 0;
        }
        apdu_select_transport(ITF_HID);
        driver_init_hid();

        hid_rx[ITF_HID_CTAP].r_ptr += HID_RPT_SIZE;
        if (hid_rx[ITF_HID_CTAP].r_ptr >= hid_rx[ITF_HID_CTAP].w_ptr) {
            hid_rx[ITF_HID_CTAP].r_ptr = hid_rx[ITF_HID_CTAP].w_ptr = 0;
        }
        last_packet_time = board_millis();
        DEBUG_PAYLOAD((uint8_t *)ctap_req, HID_RPT_SIZE);
        if (FRAME_TYPE(ctap_req) == TYPE_CONT && msg_packet.len == 0) {
            last_packet_time = 0;
            return 0;
        }
        if (ctap_req->cid == 0x0 ||
            (ctap_req->cid == CID_BROADCAST && (FRAME_TYPE(ctap_req) != TYPE_INIT || ctap_req->init.cmd != CTAPHID_INIT))) {
            return ctap_error(CTAP1_ERR_INVALID_CHANNEL);
        }
        if (board_millis() < lock && ctap_req->cid != lock_cid &&
            !(ctap_req->cid == CID_BROADCAST && ctap_req->init.cmd == CTAPHID_INIT)) {
            return ctap_error(CTAP1_ERR_CHANNEL_BUSY);
        }

        if (FRAME_TYPE(ctap_req) == TYPE_INIT) {
            if (MSG_LEN(ctap_req) > CTAP_MAX_PACKET_SIZE) {
                return ctap_error(CTAP1_ERR_INVALID_LEN);
            }
            // Fixed-size control commands must be validated before consuming
            // their payload or replacing the current receive transaction.
            uint16_t command_len = MSG_LEN(ctap_req);
            if ((ctap_req->init.cmd == CTAPHID_INIT && command_len != 8) ||
                (ctap_req->init.cmd == CTAPHID_LOCK && command_len != 1) ||
                ((ctap_req->init.cmd == CTAPHID_WINK ||
                  ctap_req->init.cmd == CTAPHID_UUID ||
                  ctap_req->init.cmd == CTAPHID_VERSION) && command_len != 0) ||
                (ctap_req->init.cmd == CTAPHID_ADMIN && command_len != 1)) {
                return ctap_error(CTAP1_ERR_INVALID_LEN);
            }
#if DEBUG_APDU
            printf("command %x\n", FRAME_CMD(ctap_req));
            printf("len %d\n", MSG_LEN(ctap_req));
#endif
            msg_packet.len = msg_packet.current_len = 0;
            if (MSG_LEN(ctap_req) > 64 - 7) {
                msg_packet.len = MSG_LEN(ctap_req);
                memcpy(msg_packet.data + msg_packet.current_len, ctap_req->init.data, 64 - 7);
                msg_packet.current_len += 64 - 7;
            }
            memcpy(&last_req, ctap_req, sizeof(CTAPHID_FRAME));
            last_cmd = ctap_req->init.cmd;
            last_seq = 0;
            last_cmd_time = board_millis();
        }
        else {
            if (last_seq != ctap_req->cont.seq) {
                return ctap_error(CTAP1_ERR_INVALID_SEQ);
            }
            if (last_req.cid == ctap_req->cid) {
                memcpy(msg_packet.data + msg_packet.current_len, ctap_req->cont.data,
                       MIN(64 - 5, msg_packet.len - msg_packet.current_len));
                msg_packet.current_len += MIN(64 - 5, msg_packet.len - msg_packet.current_len);
                memcpy(&last_req, ctap_req, sizeof(CTAPHID_FRAME));
                last_seq++;
            }
            else {
                return ctap_error(CTAP1_ERR_CHANNEL_BUSY);
            }
        }
        if (ctap_req->init.cmd == CTAPHID_INIT) {
            card_exit();
            card_reset_app(ITF_HID);
            hid_tx[ITF_HID_CTAP].r_ptr = hid_tx[ITF_HID_CTAP].w_ptr = 0;
            init_fido();
            CTAPHID_INIT_REQ *req = (CTAPHID_INIT_REQ *) ctap_req->init.data;
            CTAPHID_INIT_RESP *resp = (CTAPHID_INIT_RESP *) ctap_resp->init.data;
            memcpy(resp->nonce, req->nonce, sizeof(resp->nonce));
            resp->cid = ctap_req->cid == CID_BROADCAST ? allocate_cid() : ctap_req->cid;
            resp->versionInterface = CTAPHID_IF_VERSION;
            resp->versionMajor = get_version_major ? get_version_major() : PICOKEYS_SDK_VERSION_MAJOR;
            resp->versionMinor = get_version_minor ? get_version_minor() : PICOKEYS_SDK_VERSION_MINOR;
            resp->capFlags = CAPFLAG_WINK | CAPFLAG_CBOR;

            ctap_resp->cid = ctap_req->cid;
            ctap_resp->init.cmd = CTAPHID_INIT;
            ctap_resp->init.bcntl = 17;
            ctap_resp->init.bcnth = 0;
            driver_write_hid(ITF_HID_CTAP, CONST_BYTE_ARRAY((const uint8_t *)ctap_resp, 64));
            msg_packet.len = msg_packet.current_len = 0;
            last_packet_time = 0;
        }
        else if (ctap_req->init.cmd == CTAPHID_WINK) {
            if (MSG_LEN(ctap_req) != 0) {
                return ctap_error(CTAP1_ERR_INVALID_LEN);
            }
            last_packet_time = 0;
            memcpy(ctap_resp, ctap_req, sizeof(CTAPHID_FRAME));
#if defined(PICO_PLATFORM) || defined(ESP_PLATFORM)
            sleep_ms(1000); //For blinking the device during 1 seg
#endif
            driver_write_hid(ITF_HID_CTAP, CONST_BYTE_ARRAY((const uint8_t *)ctap_resp, 64));
            msg_packet.len = msg_packet.current_len = 0;
        }
        else if ((last_cmd == CTAPHID_PING || last_cmd == CTAPHID_SYNC) &&
                 (msg_packet.len == 0 ||
                  (msg_packet.len == msg_packet.current_len && msg_packet.len > 0))) {
            if (msg_packet.current_len == msg_packet.len && msg_packet.len > 0) {
                // Leave room for the last padded report in the transmit buffer.
                if (msg_packet.len > sizeof(hid_tx[ITF_HID_CTAP].buffer) - HID_RPT_SIZE) {
                    msg_packet.len = msg_packet.current_len = 0;
                    return ctap_error(CTAP1_ERR_INVALID_LEN);
                }
                memcpy(ctap_resp->init.data, msg_packet.data, msg_packet.len);
                // A transport echo has no APDU status, including after CCID.
                driver_exec_finished_cont_hid(ITF_HID_CTAP, msg_packet.len, 7);
            }
            else {
                memcpy(ctap_resp->init.data, ctap_req->init.data, MSG_LEN(ctap_req));
                ctap_resp->cid = ctap_req->cid;
                ctap_resp->init.cmd = last_cmd;
                ctap_resp->init.bcnth = MSG_LEN(ctap_req) >> 8;
                ctap_resp->init.bcntl = MSG_LEN(ctap_req) & 0xff;
                driver_write_hid(ITF_HID_CTAP, CONST_BYTE_ARRAY((const uint8_t *)ctap_resp, 64));
            }
            msg_packet.len = msg_packet.current_len = 0;
            last_packet_time = 0;
        }
        else if (ctap_req->init.cmd == CTAPHID_LOCK) {
            if (MSG_LEN(ctap_req) != 1) {
                return ctap_error(CTAP1_ERR_INVALID_LEN);
            }
            if (ctap_req->init.data[0] > 10) {
                return ctap_error(CTAP1_ERR_INVALID_PARAMETER);
            }
            if (board_millis() < lock && ctap_req->cid != lock_cid) {
                return ctap_error(CTAP1_ERR_CHANNEL_BUSY);
            }
            if (ctap_req->init.data[0] == 0) {
                lock = 0;
                lock_cid = 0;
            }
            else {
                lock = board_millis() + ctap_req->init.data[0] * 1000;
                lock_cid = ctap_req->cid;
            }
            ctap_resp->cid = ctap_req->cid;
            ctap_resp->init.cmd = ctap_req->init.cmd;
            driver_write_hid(ITF_HID_CTAP, CONST_BYTE_ARRAY((const uint8_t *)ctap_resp, 64));
            msg_packet.len = msg_packet.current_len = 0;
            last_packet_time = 0;
        }
        else if (ctap_req->init.cmd == CTAPHID_UUID) {
            ctap_resp->cid = ctap_req->cid;
            ctap_resp->init.cmd = ctap_req->init.cmd;
            memcpy(ctap_resp->init.data, pico_serial.id, sizeof(pico_serial.id));
            ctap_resp->init.bcntl = 16;
            driver_write_hid(ITF_HID_CTAP, CONST_BYTE_ARRAY((const uint8_t *)ctap_resp, 64));
            msg_packet.len = msg_packet.current_len = 0;
            last_packet_time = 0;
        }
        else if (ctap_req->init.cmd == CTAPHID_VERSION) {
            ctap_resp->cid = ctap_req->cid;
            ctap_resp->init.cmd = ctap_req->init.cmd;
            ctap_resp->init.data[0] = PICOKEYS_SDK_VERSION_MAJOR;
            ctap_resp->init.data[1] = PICOKEYS_SDK_VERSION_MINOR;
            ctap_resp->init.bcntl = 4;
            driver_write_hid(ITF_HID_CTAP, CONST_BYTE_ARRAY((const uint8_t *)ctap_resp, 64));
            msg_packet.len = msg_packet.current_len = 0;
            last_packet_time = 0;
        }
        else if (ctap_req->init.cmd == CTAPHID_ADMIN) {
            ctap_resp->cid = ctap_req->cid;
            ctap_resp->init.cmd = ctap_req->init.cmd;
            if (ctap_req->init.data[0] == 0x80) { // Status
                memcpy(ctap_resp->init.data, "\x00\xff\xff\xff\x00", 5);
                ctap_resp->init.bcntl = 5;
            }
            driver_write_hid(ITF_HID_CTAP, CONST_BYTE_ARRAY((const uint8_t *)ctap_resp, 64));
            msg_packet.len = msg_packet.current_len = 0;
            last_packet_time = 0;
        }
        else if ((last_cmd == CTAPHID_MSG || last_cmd == CTAPHID_OTP) &&
                 (msg_packet.len == 0 ||
                  (msg_packet.len == msg_packet.current_len && msg_packet.len > 0))) {
            if (last_cmd == CTAPHID_OTP && !card_claim_apdu_context(ITF_HID)) {
                msg_packet.len = msg_packet.current_len = 0;
                last_packet_time = 0;
                return ctap_error(CTAP1_ERR_CHANNEL_BUSY);
            }
            card_start(ITF_HID, apdu_thread);
            if (last_cmd == CTAPHID_OTP) {
                is_nk = true;
#ifdef ENABLE_OATH_APP
                select_app(CONST_BYTE_ARRAY(oath_aid + 1, oath_aid[0]));
#endif
            }
            else {
                select_app(CONST_BYTE_ARRAY(u2f_aid + 1, u2f_aid[0]));
            }

            thread_type = 1;

            if (msg_packet.current_len == msg_packet.len && msg_packet.len > 0) {
                apdu_sent = apdu_process(ITF_HID, CONST_BYTE_ARRAY(msg_packet.data, msg_packet.len));
            }
            else {
                apdu_sent = apdu_process(ITF_HID, CONST_BYTE_ARRAY(ctap_req->init.data, MSG_LEN(ctap_req)));
            }
            DEBUG_PAYLOAD(apdu.data, (int) apdu.nc);
            msg_packet.len = msg_packet.current_len = 0;
            last_packet_time = 0;
            if (apdu_sent <= 0) timeout_stop();
        }
        else if ((last_cmd == CTAPHID_CBOR || last_cmd >= CTAPHID_VENDOR_FIRST) &&
                 (msg_packet.len == 0 || (msg_packet.len == msg_packet.current_len && msg_packet.len > 0))) {
            card_start(ITF_HID, cbor_thread);
            thread_type = 2;
            cancel_button = false;
            select_app(CONST_BYTE_ARRAY(fido_aid + 1, fido_aid[0]));
            if (msg_packet.current_len == msg_packet.len && msg_packet.len > 0) {
                apdu_sent = cbor_process(last_cmd, msg_packet.data, msg_packet.len);
            }
            else {
                apdu_sent = cbor_process(last_cmd, ctap_req->init.data, MSG_LEN(ctap_req));
            }
            msg_packet.len = msg_packet.current_len = 0;
            last_packet_time = 0;
            if (apdu_sent < 0) {
                timeout_stop();
                return ctap_error((uint8_t)(-apdu_sent));
            }
            last_keepalive_time = 0;
            send_keepalive();
        }
        else {
            if (msg_packet.len == 0) {
                return ctap_error(CTAP1_ERR_INVALID_CMD);
            }
        }
        // echo back anything we received from host
        //tud_hid_report(0, buffer, bufsize);
        //printf("END\n");
        if (apdu_sent > 0) {
            usb_send_event(EV_CMD_AVAILABLE);
        }
    }
    return apdu_sent;
}

static void send_keepalive(void) {
    if (thread_type == 1 || cancel_button) {
        return;
    }
    uint32_t now = board_millis();
    if (last_keepalive_time != 0 && now - last_keepalive_time < 250) {
        return;
    }
    CTAPHID_FRAME *resp = (CTAPHID_FRAME *) (hid_tx[ITF_HID_CTAP].buffer + sizeof(hid_tx[ITF_HID_CTAP].buffer) - 64);
    //memset(ctap_resp, 0, sizeof(CTAPHID_FRAME));
    resp->cid = ctap_req->cid;
    resp->init.cmd = CTAPHID_KEEPALIVE;
    resp->init.bcntl = 1;
    resp->init.data[0] = is_req_button_pending() ? 2 : 1;
    //send_buffer_size[ITF_HID_CTAP] = 0;
    if (driver_write_hid(ITF_HID_CTAP, CONST_BYTE_ARRAY((const uint8_t *)resp, 64)) > 0) {
        last_keepalive_time = now;
    }
}

void driver_exec_finished_hid(uint16_t size_next) {
    if (size_next > 0) {
        if (thread_type == 2 && apdu.sw != 0) {
            ctap_error(apdu.sw & 0xff);
        }
        else {
            if (is_nk) {
                memmove(apdu.rdata + 2, apdu.rdata, size_next - 2);
                put_uint16_be(apdu.sw, apdu.rdata);
            }
            driver_exec_finished_cont_hid(ITF_HID_CTAP, size_next, 7);
        }
    }
    apdu.sw = 0;
}

void driver_exec_finished_cont_hid(uint8_t itf, uint16_t size_next, uint16_t offset) {
    offset -= 7;
    ctap_resp = (CTAPHID_FRAME *) (hid_tx[itf].buffer + offset);
    ctap_resp->cid = ctap_req->cid;
    ctap_resp->init.bcnth = size_next >> 8;
    ctap_resp->init.bcntl = size_next & 0xff;
    send_buffer_size[itf] = size_next;
    ctap_resp->init.cmd = last_cmd;
    if (hid_write_offset(size_next+7, offset) > 0) {
        //ctap_resp = (CTAPHID_FRAME *) ((uint8_t *) ctap_resp + 64 - 5);
        //send_buffer_size[ITF_HID_CTAP] -= MIN(64 - 7, send_buffer_size[ITF_HID_CTAP]);
    }
}

static bool hid_response_pending(void) {
    return last_write_result[ITF_HID_CTAP] == WRITE_PENDING ||
        hid_tx[ITF_HID_CTAP].w_ptr != hid_tx[ITF_HID_CTAP].r_ptr ||
        send_buffer_size[ITF_HID_CTAP] != 0;
}

/* Dispatch-loop only, after the old worker has finished. The TCP simulator
 * has no endpoint-reset callback to discard a disconnected client's reply. */
void hid_discard_disconnected_transaction(void) {
    if (ITF_HID_CTAP == ITF_INVALID || is_busy()) return;
    uint8_t report[HID_RPT_SIZE];
    while (queue_try_remove(&hid_reports, report)) {}
    memset(&hid_rx[ITF_HID_CTAP], 0, sizeof(*hid_rx));
    memset(&hid_tx[ITF_HID_CTAP], 0, sizeof(*hid_tx));
    msg_packet.len = msg_packet.current_len = 0;
    last_packet_time = last_cmd_time = last_keepalive_time = 0;
    last_write_result[ITF_HID_CTAP] = WRITE_SUCCESS;
    send_buffer_size[ITF_HID_CTAP] = 0;
    hid_resync_pending = false;
    atomic_store(&hid_sideband_pending, false);
    memset(&last_req, 0, sizeof(last_req));
    lock = lock_cid = 0;
    card_reset_app(ITF_HID);
}

void hid_session_reset(void) {
    hid_discard_disconnected_transaction();
    for (uint8_t i = 0; i < ITF_HID_TOTAL; ++i) {
        memset(&hid_rx[i], 0, sizeof(*hid_rx));
        memset(&hid_tx[i], 0, sizeof(*hid_tx));
        send_buffer_size[i] = 0;
        last_write_result[i] = WRITE_SUCCESS;
    }
#ifndef ENABLE_EMULATION
    memset(keyboard_buffer, 0, sizeof(keyboard_buffer));
    keyboard_buffer_len = keyboard_w = 0;
    sent_key = false;
#endif
}

static void hid_refuse(const CTAPHID_FRAME *request, uint8_t error) {
    if (FRAME_TYPE(request) != TYPE_INIT || hid_response_pending()) return;
    CTAPHID_FRAME response = {0};
    response.cid = request->cid;
    response.init.cmd = CTAPHID_ERROR;
    response.init.bcntl = 1;
    response.init.data[0] = error;
    atomic_store(&hid_sideband_pending, true);
    if (!driver_write_hid(ITF_HID_CTAP,
            CONST_BYTE_ARRAY((const uint8_t *)&response, sizeof(response))))
        atomic_store(&hid_sideband_pending, false);
}

/* Called only on the dispatch loop, before incoming data can overwrite live
 * command/reply backing. Like CanoKey, reserve through RX, execution and TX;
 * competing traffic cannot extend the owner's receive timeout. */
static bool hid_report_is_control_or_competitor(const CTAPHID_FRAME *request) {
    bool executing = is_busy() && !card_busy_on_other_interface(ITF_HID);
    bool init = FRAME_TYPE(request) == TYPE_INIT;
    if (init && request->init.cmd == CTAPHID_CANCEL) {
        if (executing && request->cid == last_req.cid && MSG_LEN(request) == 0)
            cancel_button = true;
        return true;
    }
    if (hid_resync_pending || executing || hid_response_pending()) {
        if (!hid_resync_pending && init && request->cid == last_req.cid &&
            request->init.cmd == CTAPHID_INIT && MSG_LEN(request) == 8) {
            hid_resync_request = *request;
            hid_resync_pending = true;
            // Let the worker unwind before invoking card_exit/reset.
            if (executing) cancel_button = true;
        } else {
            hid_refuse(request, CTAP1_ERR_CHANNEL_BUSY);
        }
        return true;
    }
    if (msg_packet.len) {
        if (request->cid != last_req.cid) {
            hid_refuse(request, CTAP1_ERR_CHANNEL_BUSY);
            return true;
        }
        if (init && request->init.cmd != CTAPHID_INIT) {
            hid_refuse(request, CTAP1_ERR_INVALID_SEQ);
            msg_packet.len = msg_packet.current_len = 0;
            last_packet_time = 0;
            return true;
        }
    }
    return false;
}

static void reject_hid_during_other_transport_work(void) {
    static bool waiting;
    static uint32_t waiting_since;
    if (!hid_reports_ready || !card_busy_on_other_interface(ITF_HID) ||
        queue_is_empty(&hid_reports)) {
        waiting = false;
        return;
    }
    uint32_t now = board_millis();
    if (!waiting) {
        waiting = true;
        waiting_since = now;
    }
    // Let short work finish normally. Bound only the undispatched report's
    // wait, never the other worker's ownership, lifetime or PIN context.
    if (!is_req_button_pending() && (uint32_t)(now - waiting_since) < 250) return;
    CTAPHID_FRAME request;
    while (last_write_result[ITF_HID_CTAP] != WRITE_PENDING &&
           hid_tx[ITF_HID_CTAP].w_ptr == hid_tx[ITF_HID_CTAP].r_ptr &&
           queue_try_peek(&hid_reports, &request)) {
        if (FRAME_TYPE(&request) == TYPE_INIT && request.init.cmd != CTAPHID_CANCEL) {
            // Do not borrow the active OTP/CCID APDU or its response buffer.
            // A client can retry CHANNEL_BUSY. Both a human confirmation and
            // long crypto (for example RSA generation) can exceed the host
            // read timeout; never silently queue a new HID transaction there.
            hid_refuse(&request, CTAP1_ERR_CHANNEL_BUSY);
        }
        // Continuations of a rejected request have no active transaction.
        // CANCEL on this inactive HID channel must not cancel the other app.
        queue_try_remove(&hid_reports, &request);
    }
}

void hid_task(void) {
    const uint32_t status_poll_interval_ms = 1;
    static uint32_t last_status_poll_ms = 0;
    if (ITF_HID_CTAP == ITF_INVALID) return;
#ifdef ENABLE_EMULATION
    // Receive while another transport is executing, just like TinyUSB. The
    // common queue below defers dispatch, not reception.
    for (size_t i = 0; i < HID_REPORT_QUEUE_DEPTH && !queue_is_full(&hid_reports); ++i) {
        uint16_t rx_len = emul_read(ITF_HID);
        if (!rx_len) break;
        tud_hid_set_report_cb(ITF_HID, 0, 0, emul_rx, HID_RPT_SIZE);
        emul_rx_size = 0;
    }
#endif
    reject_hid_during_other_transport_work();
    uint8_t report[HID_RPT_SIZE];
    for (size_t i = 0; i < HID_REPORT_QUEUE_DEPTH && hid_reports_ready &&
         !card_busy_on_other_interface(ITF_HID) &&
         !atomic_load(&hid_sideband_pending) &&
         queue_try_peek(&hid_reports, report); i++) {
        const CTAPHID_FRAME *incoming = (const CTAPHID_FRAME *)report;
        bool control = FRAME_TYPE(incoming) == TYPE_INIT &&
            (incoming->init.cmd == CTAPHID_CANCEL ||
             (incoming->init.cmd == CTAPHID_INIT && incoming->cid == last_req.cid && MSG_LEN(incoming) == 8));
        // The current endpoint transfer owns its response backing. Retain a
        // new ordinary report until the reply drains instead of dropping it
        // when CHANNEL_BUSY itself cannot yet be transmitted.
        if (hid_response_pending() && !control) break;
        if (!queue_try_remove(&hid_reports, report)) break;
        if (hid_report_is_control_or_competitor((const CTAPHID_FRAME *)report)) continue;
        process_hid_report(report);
    }
    int proc_pkt = 0;
    if (hid_rx[ITF_HID_CTAP].w_ptr - hid_rx[ITF_HID_CTAP].r_ptr >= 64) {
        //proc_pkt = driver_process_usb_packet_hid(64);
    }
    if (proc_pkt == 0 && !hid_response_pending()) {
        driver_process_usb_nopacket_hid();
    }
    uint32_t now_ms = board_millis();
    if (now_ms - last_status_poll_ms >= status_poll_interval_ms) {
        last_status_poll_ms = now_ms;
        int status = card_status(ITF_HID);
        if (status == PICOKEYS_OK) {
            if (!hid_resync_pending) driver_exec_finished_hid(finished_data_size);
        }
        else if (status == PICOKEYS_ERR_BLOCKED) {
            send_keepalive();
        }
    }
    if (hid_resync_pending && !is_busy() && last_write_result[ITF_HID_CTAP] != WRITE_PENDING) {
        msg_packet.len = msg_packet.current_len = 0;
        last_packet_time = 0;
        hid_resync_pending = false;
        process_hid_report((const uint8_t *)&hid_resync_request);
    }
    // Drain completed reports without inserting a scheduler sleep per packet.
    // Native USB stops at WRITE_PENDING; loopback completes synchronously.
    // Bound the burst so other transports retain their dispatch opportunity.
    for (size_t i = 0; i < HID_REPORT_QUEUE_DEPTH &&
         hid_tx[ITF_HID_CTAP].w_ptr > hid_tx[ITF_HID_CTAP].r_ptr &&
         last_write_result[ITF_HID_CTAP] != WRITE_PENDING; ++i) {
        if (!driver_write_hid(ITF_HID_CTAP, CONST_BYTE_ARRAY(
                hid_tx[ITF_HID_CTAP].buffer + hid_tx[ITF_HID_CTAP].r_ptr, HID_RPT_SIZE))) break;
    }
#ifndef ENABLE_EMULATION
    /* Keyboard ITF */
    // Poll every 10ms
    const uint32_t interval_ms = 10;
    static uint32_t start_ms = 0;

    if (board_millis() - start_ms < interval_ms) {
        return;
    }
    start_ms += interval_ms;

    // Remote wakeup
    if (tud_suspended() && keyboard_buffer_len > 0) {
        tud_remote_wakeup();
    }
    else {
        send_hid_report(REPORT_ID_KEYBOARD);
    }
#endif
}
