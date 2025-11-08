#include "vad.h"
#include <stdlib.h>

#define ENERGY_THRESHOLD 500.0f

bool vad_is_speaking(int16_t *samples, size_t len) {
    double energy = 0;
    for (size_t i = 0; i < len; i++) {
        energy += abs(samples[i]);
    }
    energy /= len;
    return energy > ENERGY_THRESHOLD;
}
