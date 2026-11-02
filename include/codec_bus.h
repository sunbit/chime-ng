#ifndef CHIME_CODEC_BUS_H
#define CHIME_CODEC_BUS_H
#include <stdbool.h>
#include <stdint.h>
#define AUDIO_DIAGNOSTIC_SIZE 384
bool codec_bus_init(void);
bool codec_reg_write(uint8_t reg,uint8_t value);
bool codec_reg_read(uint8_t reg,uint8_t *value);
bool codec_reg_verify(uint8_t reg,uint8_t expected);
void codec_bus_ready(void);
const char *codec_bus_diagnostic(void);
#endif
