#pragma once
#include "esp_err.h"

void audio_capture_init(void);
esp_err_t audio_capture_record_to_file(const char *file_path, int record_duration_ms);
