#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { int clk, cmd, d0; } sd_logger_pins_t;
/* Optional board hook. An absent slot must not probe arbitrary GPIOs. */
bool picokey_sd_logger_pins(sd_logger_pins_t *pins);
void sd_logger_init(void);
void sd_logger_early_init(void);
bool sd_logger_is_mounted(void);
uint64_t sd_logger_get_capacity(void);
bool sd_logger_has_error(void);

#endif /* SD_LOGGER_H */
