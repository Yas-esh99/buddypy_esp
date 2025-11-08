#pragma once
#include "esp_err.h"

void audio_capture_init(void);
esp_err_t audio_capture_wait_for_speech_and_record(const char *file_path);
