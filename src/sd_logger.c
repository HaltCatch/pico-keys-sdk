#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "sd_logger.h"
#include "sdmmc_cmd.h"

#define MOUNT_PATH "/sdcard"
#define LOG_QUEUE_BYTES 8192
#define LOG_BATCH_BYTES 2048
#define LOG_SYNC_MS 250

static RingbufHandle_t log_queue;
static vprintf_like_t console_vprintf = vprintf;
static uint64_t card_capacity;
static int log_fd;
static atomic_bool log_writable;
static atomic_bool log_capturing;
static atomic_uint queue_lost, format_lost;
static const char *const tag = "SD_LOG";

bool sd_logger_is_mounted(void) {
    return card_capacity != 0;
}

bool sd_logger_has_error(void) {
    return card_capacity && (!atomic_load(&log_writable) ||
        atomic_load(&queue_lost) || atomic_load(&format_lost));
}

uint64_t sd_logger_get_capacity(void) {
    return card_capacity;
}

static int sd_log_vprintf(const char *fmt, va_list args) {
    // Keep console output intact even when the card stops responding.
    if (atomic_load(&log_capturing)) {
        char small[256];
        va_list copy;
        va_copy(copy, args);
        int len = vsnprintf(small, sizeof(small), fmt, copy);
        va_end(copy);
        if (len < 0 || (size_t)len > xRingbufferGetMaxItemSize(log_queue)) {
            atomic_fetch_add(&format_lost, 1);
        } else if (len) {
            char *msg = small;
            if ((size_t)len >= sizeof(small)) {
                msg = malloc((size_t)len + 1);
                if (msg) {
                    va_copy(copy, args);
                    if (vsnprintf(msg, (size_t)len + 1, fmt, copy) != len) {
                        free(msg);
                        msg = NULL;
                    }
                    va_end(copy);
                }
            }
            if (!msg) {
                atomic_fetch_add(&format_lost, 1);
            } else if (xRingbufferSend(log_queue, msg, (size_t)len, 0) != pdTRUE) {
                atomic_fetch_add(&queue_lost, 1);
            }
            if (msg != small) free(msg);
        }
    }
    return console_vprintf(fmt, args);
}

static bool write_all(const char *buf, size_t length) {
    while (length) {
        ssize_t n = write(log_fd, buf, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n == 0) errno = ENOSPC;
            return false;
        }
        buf += n;
        length -= (size_t)n;
    }
    return true;
}

static void sd_logger_task(void *arg) {
    (void)arg;
    char batch[LOG_BATCH_BYTES];
    size_t buffered = 0;
    bool dirty = false;
    TickType_t since = 0;
    const TickType_t interval = pdMS_TO_TICKS(LOG_SYNC_MS);
    unsigned durable = 0, pending = 0, reported_queue = 0, reported_format = 0;
    const char *operation = "write";
    for (;;) {
        TickType_t elapsed = xTaskGetTickCount() - since;
        TickType_t wait = dirty && elapsed < interval ? interval - elapsed : (dirty ? 0 : interval);
        size_t length;
        char *record = xRingbufferReceive(log_queue, &length, wait);
        if (record) {
            if (!dirty) {
                since = xTaskGetTickCount();
                dirty = true;
            }
            ++pending;
            bool ok = true;
            if (length > sizeof(batch) - buffered) {
                ok = write_all(batch, buffered);
                buffered = 0;
            }
            if (ok) {
                if (length > sizeof(batch)) {
                    ok = write_all(record, length);
                } else {
                    memcpy(batch + buffered, record, length);
                    buffered += length;
                }
            }
            vRingbufferReturnItem(log_queue, record);
            if (!ok) break;
        }

        if (dirty && xTaskGetTickCount() - since < interval) continue;
        unsigned q = atomic_load(&queue_lost), f = atomic_load(&format_lost);
        bool loss = q != reported_queue || f != reported_format;
        if (!dirty && !loss) continue;
        if (!write_all(batch, buffered)) break;
        buffered = 0;
        if (loss) {
            char marker[112];
            int n = snprintf(marker, sizeof(marker),
                "\nSD_LOG LOSS total_queue=%u total_format=%u\n", q, f);
            if (!write_all(marker, (size_t)n)) break;
        }
        if (fsync(log_fd) != 0) {
            operation = "sync";
            break;
        }
        durable += pending;
        pending = 0;
        dirty = false;
        if (loss) {
            // Bypass the hook: the marker is already on the card.
            fprintf(stderr, "SD_LOG LOSS total_queue=%u total_format=%u\n", q, f);
            reported_queue = q;
            reported_format = f;
        }
    }

    int error = errno;
    atomic_store(&log_writable, false);
    atomic_store(&log_capturing, false);
    fprintf(stderr, "SD_LOG ERROR %s errno=%d durable=%u unsynced=%u queue_lost=%u format_lost=%u; further logs console only\n",
        operation, error, durable, pending, atomic_load(&queue_lost), atomic_load(&format_lost));
    if (close(log_fd) != 0) fprintf(stderr, "SD_LOG ERROR close errno=%d\n", errno);
    // The hook can still be returning from a concurrent call; keep the queue
    // allocated until reboot rather than race a producer and free its storage.
    vTaskDelete(NULL);
}

static int open_log(char *path, size_t size) {
    DIR *dir = opendir(MOUNT_PATH);
    if (!dir) return -1;
    unsigned last = 0;
    int scan_error;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            scan_error = errno;
            break;
        }
        const char *name = entry->d_name;
        if (strspn(name, "0123456789") != 8 || strcasecmp(name + 8, ".TXT")) continue;
        unsigned number = (unsigned)strtoul(name, NULL, 10);
        if (number > last) last = number;
    }
    if (closedir(dir) != 0 && !scan_error) scan_error = errno;
    if (scan_error) {
        errno = scan_error;
        return -1;
    }
    if (last == 99999999) {
        errno = EOVERFLOW;
        return -1;
    }
    snprintf(path, size, MOUNT_PATH "/%08u.TXT", last + 1);
    // Never append to or replace the logs from another boot.
    return open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
}

void sd_logger_early_init(void) {
    if (log_queue) return;
    log_queue = xRingbufferCreate(LOG_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (log_queue) {
        atomic_store(&log_capturing, true);
        console_vprintf = esp_log_set_vprintf(sd_log_vprintf);
    }
}

void sd_logger_init(void) {
    sd_logger_early_init();
    sd_logger_pins_t pins;
    if (!picokey_sd_logger_pins(&pins)) {
        atomic_store(&log_capturing, false);
        return;
    }
    const esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 1,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = pins.clk;
    slot.cmd = pins.cmd;
    slot.d0 = pins.d0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card;
    esp_err_t error = esp_vfs_fat_sdmmc_mount(MOUNT_PATH, &host, &slot, &config, &card);
    if (error != ESP_OK) {
        ESP_LOGW(tag, "microSD unavailable (%s); logs on console only", esp_err_to_name(error));
        atomic_store(&log_capturing, false);
        return;
    }
    card_capacity = (uint64_t)card->csd.capacity * card->csd.sector_size;
    if (!log_queue) {
        ESP_LOGE(tag, "Cannot allocate log queue; logs on console only");
        goto unmount;
    }

    char path[32];
    log_fd = open_log(path, sizeof(path));
    if (log_fd < 0) {
        ESP_LOGE(tag, "Cannot open boot log (errno=%d); logs on console only", errno);
        goto release_queue;
    }
    atomic_store(&log_writable, true);
    if (xTaskCreate(sd_logger_task, "sd_logger", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(tag, "Cannot start writer; logs on console only");
        if (close(log_fd) != 0) ESP_LOGE(tag, "Cannot close log (errno=%d)", errno);
        goto release_queue;
    }
    ESP_LOGI(tag, "Boot log %s, card capacity=%llu bytes, sync interval=%d ms",
        path, (unsigned long long)card_capacity, LOG_SYNC_MS);
    return;

release_queue:
    atomic_store(&log_writable, false);
    atomic_store(&log_capturing, false);
    // Keep the early queue until reboot: another producer may be inside the hook.
unmount:
    error = esp_vfs_fat_sdcard_unmount(MOUNT_PATH, card);
    if (error != ESP_OK) ESP_LOGE(tag, "Cannot unmount card: %s", esp_err_to_name(error));
}
