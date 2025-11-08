#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

bool vad_is_speaking(int16_t *samples, size_t len);
