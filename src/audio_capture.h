#pragma once
#include <stdint.h>
#include <stddef.h>

void audio_capture_init(void);
size_t audio_capture_read(int16_t *samples, size_t max_samples);
