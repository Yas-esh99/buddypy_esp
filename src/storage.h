#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

void storage_init(void);
FILE* storage_open_wav(const char *filename, uint32_t sample_rate);
void storage_write_samples(FILE *f, int16_t *samples, size_t len);
void storage_close_wav(FILE *f, uint32_t sample_rate, size_t total_samples);
