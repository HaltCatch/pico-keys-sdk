#ifndef PICOKEYS_LOG_H
#define PICOKEYS_LOG_H
#ifdef ESP_PLATFORM
#include "esp_log.h"
#define PICOKEYS_LOG_ERROR(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#else
#include <stdio.h>
#define PICOKEYS_LOG_ERROR(tag, ...) do { fprintf(stderr, "%s: ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif
#endif
