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
#include "emulation.h"
#include "apdu.h"
#include "button.h"
#include "usb.h"
#include "ccid/ccid.h"
#include "hid/ctap_hid.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#ifndef _MSC_VER
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#else
#include <ws2tcpip.h>
typedef SOCKET socket_t;
typedef int socklen_t;
#define close closesocket
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

socket_t ccid_sock = INVALID_SOCKET;
socket_t hid_server_sock = INVALID_SOCKET;
socket_t hid_client_sock = INVALID_SOCKET;
static socket_t ccid_server_sock = INVALID_SOCKET;
static socket_t otp_server_sock = INVALID_SOCKET;
static pthread_t otp_control_thread;
static void *otp_control_loop(void *unused);
pthread_t hcore0, hcore1;
uint8_t emul_rx[USB_BUFFER_SIZE], emul_tx[USB_BUFFER_SIZE];
uint16_t emul_rx_size = 0, emul_tx_size = 0;

typedef struct {
    uint8_t header[2], payload[USB_BUFFER_SIZE];
    uint16_t header_size, size, received;
} frame_reader_t;
static frame_reader_t hid_reader, ccid_reader;
static bool hid_disconnected;
extern void hid_discard_disconnected_transaction(void);

static void nonblocking(socket_t fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
#ifdef _MSC_VER
    unsigned long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}
static int ready(socket_t fd, bool write) {
    if (fd == INVALID_SOCKET) return 0;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval timeout = {.tv_sec = write ? 2 : 0, .tv_usec = 0};
    return select((int)fd + 1, write ? NULL : &set, write ? &set : NULL, NULL, &timeout) > 0;
}
static socket_t listener(const char *host, uint16_t port) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return INVALID_SOCKET;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return INVALID_SOCKET;
    }
    nonblocking(fd);
    return fd;
}
int emul_init(const char *host, uint16_t port) {
#ifdef _MSC_VER
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
#endif
    const char *listen_ccid = getenv("PICOKEYS_EMULATION_CCID_LISTEN");
    if (listen_ccid && strcmp(listen_ccid, "1") == 0) {
        ccid_server_sock = listener(host, port);
        if (ccid_server_sock == INVALID_SOCKET) return -1;
    } else {
        ccid_sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
        if (ccid_sock == INVALID_SOCKET || inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
            connect(ccid_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            fprintf(stderr, "Virtual PC/SC unavailable; start vpcd or set PICOKEYS_EMULATION_CCID_LISTEN=1\n");
            if (ccid_sock != INVALID_SOCKET) close(ccid_sock);
            ccid_sock = INVALID_SOCKET;
            return -1;
        }
        nonblocking(ccid_sock);
    }
    hid_server_sock = listener(host, port - 1);
    if (hid_server_sock == INVALID_SOCKET) return -1;
    otp_server_sock = listener(host, port + 1);
    if (otp_server_sock == INVALID_SOCKET || pthread_create(&otp_control_thread, NULL, otp_control_loop, NULL) != 0) return -1;
    fprintf(stderr, "Emulation on loopback: FIDO %u, CCID %u; test credentials only\n", port - 1, port);
    return 0;
}
static socket_t get_sock_itf(uint8_t itf) {
    if (itf == ITF_CCID) return ccid_sock;
    if (itf == ITF_HID) return hid_client_sock;
    return INVALID_SOCKET;
}
static bool send_all(socket_t fd, const uint8_t *data, size_t len) {
    while (len) {
        if (!ready(fd, true)) return false;
        int n = send(fd, (const char *)data, (int)len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        data += n;
        len -= n;
    }
    return true;
}
uint16_t driver_write_emul_raw(uint8_t itf, const uint8_t *buffer, uint16_t size) {
    socket_t fd = get_sock_itf(itf);
    uint16_t wire_size = htons(size);
    if (fd == INVALID_SOCKET || !send_all(fd, (const uint8_t *)&wire_size, 2) ||
        (size && !send_all(fd, buffer, size))) return 0;
    emul_tx_size = size;
    return size;
}
void driver_exec_finished_cont_emul(uint8_t itf, uint16_t size, uint16_t offset) {
    if (itf == ITF_HID) driver_exec_finished_cont_hid(ITF_HID_CTAP, size, offset);
    else if (itf == ITF_CCID) driver_write_emul_raw(itf, emul_tx + offset, size);
}
uint16_t driver_write_emul(uint8_t itf, const_byte_array_t buffer) {
    return driver_write_emul_raw(itf, buffer.data, (uint16_t)buffer.len);
}
uint32_t tud_vendor_n_write(uint8_t itf, const uint8_t *buffer, uint32_t n) {
    uint16_t written = driver_write_emul_raw(ITF_CCID, buffer, (uint16_t)n);
    tud_vendor_tx_cb(itf, written);
    return written;
}
extern void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t len);
bool tud_hid_n_report(uint8_t itf, uint8_t id, const uint8_t *buffer, uint32_t n) {
    (void)itf;
    (void)id;
    bool written = driver_write_emul_raw(ITF_HID, buffer, (uint16_t)n) == n;
    // A native USB task can finish before tud_hid_n_report returns. Model
    // that ordering to catch callers overwriting the completion state.
    if (written) tud_hid_report_complete_cb(itf, buffer, (uint16_t)n);
    return written;
}
static void accept_client(socket_t server, socket_t *client, frame_reader_t *reader) {
    if (!ready(server, false)) return;
    socket_t next = accept(server, NULL, NULL);
    if (next == INVALID_SOCKET) return;
    if (*client != INVALID_SOCKET) close(*client);
    *client = next;
    nonblocking(next);
    memset(reader, 0, sizeof(*reader));
}
/* TCP is a byte stream: preserve partial headers and payloads across iterations. */
static int read_frame(socket_t fd, frame_reader_t *r) {
    while (ready(fd, false)) {
        uint8_t *dest = r->header_size < 2 ? r->header + r->header_size : r->payload + r->received;
        size_t left = r->header_size < 2 ? 2 - r->header_size : r->size - r->received;
        int n = recv(fd, (char *)dest, (int)left, 0);
        if (n <= 0) return -1;
        if (r->header_size < 2) {
            r->header_size += n;
            if (r->header_size == 2) {
                r->size = ((uint16_t)r->header[0] << 8) | r->header[1];
                if (r->size == 0 || r->size > USB_BUFFER_SIZE) return -1;
            }
        } else {
            r->received += n;
            if (r->received == r->size) return 1;
        }
    }
    return 0;
}
/* Model the device USB control task independently of the protocol dispatcher.
 * 8-byte payloads are OTP SET_REPORT; a single zero byte is GET_REPORT.
 * Responses retain the emulator's two-byte network-order length framing. */
extern void tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t, const uint8_t *, uint16_t);
extern uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t);
static void *otp_control_loop(void *unused) {
    (void)unused;
    socket_t client = INVALID_SOCKET;
    frame_reader_t reader = {0};
    while (1) {
        accept_client(otp_server_sock, &client, &reader);
        if (client != INVALID_SOCKET) {
            int complete = read_frame(client, &reader);
            if (complete < 0) {
                close(client);
                client = INVALID_SOCKET;
                memset(&reader, 0, sizeof(reader));
            } else if (complete) {
                uint8_t reply[8] = {0};
                uint16_t size = 0;
                if (reader.size == 8) {
                    tud_hid_set_report_cb(ITF_HID_KB, 0, 3, reader.payload, 8);
                } else if (reader.size == 1 && reader.payload[0] == 0) {
                    size = tud_hid_get_report_cb(ITF_HID_KB, 0, 3, reply, sizeof(reply));
                } else if (reader.size == 1 && reader.payload[0] == 2) {
                    usb_session_reset_request(); // test-only whole USB bus reset
                } else {
                    close(client);
                    client = INVALID_SOCKET;
                }
                if (client != INVALID_SOCKET) {
                    uint16_t wire_size = htons(size);
                    if (!send_all(client, (uint8_t *)&wire_size, 2) ||
                        (size && !send_all(client, reply, size))) {
                        close(client);
                        client = INVALID_SOCKET;
                    }
                }
                memset(&reader, 0, sizeof(reader));
            }
        }
#ifdef _MSC_VER
        Sleep(1);
#else
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
#endif
    }
    return NULL;
}

uint16_t emul_read(uint8_t itf) {
    socket_t *fd = itf == ITF_HID ? &hid_client_sock : &ccid_sock;
    frame_reader_t *r = itf == ITF_HID ? &hid_reader : &ccid_reader;
    // Never deliver an old client's queued response to its replacement, or
    // cancel a different transport merely because a HID client reconnects.
    if (itf == ITF_HID && *fd != INVALID_SOCKET && ready(hid_server_sock, false)) {
        close(*fd);
        *fd = INVALID_SOCKET;
        memset(r, 0, sizeof(*r));
        hid_disconnected = true;
    }
    if (itf == ITF_HID && hid_disconnected) {
        if (is_busy()) {
            if (!card_busy_on_other_interface(ITF_HID)) cancel_button = true;
            return 0;
        }
        hid_discard_disconnected_transaction();
        hid_disconnected = false;
    }
    accept_client(itf == ITF_HID ? hid_server_sock : ccid_server_sock, fd, r);
    if (*fd == INVALID_SOCKET) return 0;
    int complete = read_frame(*fd, r);
    if (complete < 0 || (complete && itf == ITF_HID && r->size != 64)) {
        close(*fd);
        *fd = INVALID_SOCKET;
        memset(r, 0, sizeof(*r));
        if (itf == ITF_HID) {
            hid_disconnected = true;
            if (is_busy() && !card_busy_on_other_interface(ITF_HID)) cancel_button = true;
        }
        return 0;
    }
    if (!complete) return 0;
    uint16_t size = r->size;
    r->header_size = r->received = r->size = 0;
    if (itf == ITF_HID) {
        memcpy(emul_rx, r->payload, size);
        return emul_rx_size = size;
    }
    if (size == 1) {
        // vpcd OFF / ON / RESET reset the card session, unlike closing a
        // PC/SC connection with LEAVE_CARD. Match physical IccPowerOn/Off.
        if (r->payload[0] <= 2) card_reset_app(itf);
        if (r->payload[0] == 4)
            driver_write_emul_raw(itf, ccid_atr ? ccid_atr + 1 : NULL, ccid_atr ? ccid_atr[0] : 0);
    } else {
        apdu_select_transport(itf);
        apdu.rdata = emul_tx;
        if (apdu_process(itf, CONST_BYTE_ARRAY(r->payload, size)) > 0) {
            cancel_button = false;
            card_start(itf, apdu_thread);
            usb_send_event(EV_CMD_AVAILABLE);
        }
    }
    return 0;
}
void emul_task(void) {
    // The two transports share the APDU state and response buffer.
    if (card_busy_on_other_interface(ITF_CCID)) return;
    if (!is_busy()) {
        emul_read(ITF_CCID);
        return;
    }

    if (ready(ccid_sock, false)) {
        char pending;
        if (recv(ccid_sock, &pending, 1, MSG_PEEK) == 0) {
            close(ccid_sock);
            ccid_sock = INVALID_SOCKET;
            memset(&ccid_reader, 0, sizeof(ccid_reader));
        }
    }
    int status = card_status(ITF_CCID);
    if (ccid_sock == INVALID_SOCKET) cancel_button = true;
    if (status == PICOKEYS_OK) {
        driver_write_emul_raw(ITF_CCID, apdu.rdata, finished_data_size);
    }
}

// Model USB re-enumeration, not an ordinary PC/SC LEAVE_CARD disconnect.
void emul_session_reset(void) {
    if (hid_client_sock != INVALID_SOCKET) close(hid_client_sock);
    if (ccid_sock != INVALID_SOCKET) close(ccid_sock);
    hid_client_sock = ccid_sock = INVALID_SOCKET;
    memset(&hid_reader, 0, sizeof(hid_reader));
    memset(&ccid_reader, 0, sizeof(ccid_reader));
    hid_disconnected = false;
}
