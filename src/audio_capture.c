#include "audio_capture.h"
#include <stdio.h>
#include <malloc.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"

#define TAG "AUDIO_CAPTURE"

// ======== AUDIO CONFIG =========
#define SAMPLE_RATE 16000
#define I2S_PORT I2S_NUM_0
#define I2S_BCK 14
#define I2S_WS 15
#define I2S_SD 32

// ======== AFE CONFIG =========
#define AFE_FRAME_SIZE (320)
#define AFE_SILENCE_TIMEOUT_MS 1000
#define AFE_MAX_RECORDING_MS 15000

#define MIN_SPEECH_FRAMES_TO_START 8 // 8 frames * 20ms/frame = 160ms
#define MAX_SILENT_FRAMES_TO_STOP 50 // 50 frames * 20ms/frame = 1000ms

// ======== SIMPLE VAD CONFIG =========
#define VAD_RMS_THRESHOLD 800     // adjust as needed
#define VAD_HANGOVER_FRAMES 10    // extra frames to keep recording after silence

// global handles
static i2s_chan_handle_t rx_handle = NULL;
static esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;
static bool use_simple_vad = false;

void audio_capture_init(void)
{
    // Init I2S
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 1024;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK,
            .ws = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_SD},
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "✅ I2S initialized");

    // Check memory before AFE init
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_internal < 250000) {
        ESP_LOGW(TAG, "⚠️ Not enough internal RAM (%d bytes) for AFE. Using simple VAD.", free_internal);
        use_simple_vad = true;
        return;
    }

    // Init AFE
    afe_config_t *afe_config = afe_config_init("M", NULL, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config) {
        ESP_LOGE(TAG, "Failed to init AFE config");
        use_simple_vad = true;
        return;
    }

    afe_config->vad_init = true;
    afe_config->vad_mode = VAD_MODE_3;
    afe_config->wakenet_init = false;   // save memory

    afe_handle = esp_afe_handle_from_config(afe_config);
    if (!afe_handle) {
        ESP_LOGE(TAG, "Failed to get AFE handle");
        use_simple_vad = true;
        afe_config_free(afe_config);
        return;
    }

    afe_data = afe_handle->create_from_config(afe_config);
    if (!afe_data) {
        ESP_LOGE(TAG, "Failed to create AFE");
        use_simple_vad = true;
        afe_config_free(afe_config);
        return;
    }

    afe_config_free(afe_config);
    ESP_LOGI(TAG, "✅ AFE initialized");
}

static float compute_rms(const int16_t *samples, size_t count)
{
    double sum_sq = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum_sq += samples[i] * samples[i];
    }
    return sqrt(sum_sq / count);
}

esp_err_t audio_capture_wait_for_speech_and_record(const char *file_path)
{
    int16_t *audio_buffer = (int16_t *)malloc(AFE_FRAME_SIZE * sizeof(int16_t));
    if (!audio_buffer) {
        ESP_LOGE(TAG, "malloc failed");
        return ESP_FAIL;
    }

    // Ring buffer to store pre-speech audio
    const int pre_buffer_frames = MIN_SPEECH_FRAMES_TO_START;
    int16_t *pre_buffer = (int16_t *)malloc(pre_buffer_frames * AFE_FRAME_SIZE * sizeof(int16_t));
    if (!pre_buffer) {
        ESP_LOGE(TAG, "pre_buffer malloc failed");
        free(audio_buffer);
        return ESP_FAIL;
    }
    int pre_buffer_idx = 0;

    typedef enum {
        STATE_WAITING,
        STATE_RECORDING
    } vad_state_t;

    vad_state_t state = STATE_WAITING;
    int consecutive_speech_frames = 0;
    int consecutive_silent_frames = 0;
    int64_t recording_start_time = 0;
    FILE *f = NULL;

    ESP_LOGI(TAG, "👂 Waiting for speech...");

    while (true)
    {
        size_t bytes_read = 0;
        esp_err_t r = i2s_channel_read(rx_handle, audio_buffer,
                                       AFE_FRAME_SIZE * sizeof(int16_t),
                                       &bytes_read, portMAX_DELAY);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "i2s read failed: %s", esp_err_to_name(r));
            break;
        }

        // Store current audio in pre-buffer
        memcpy(&pre_buffer[pre_buffer_idx * AFE_FRAME_SIZE], audio_buffer, bytes_read);
        pre_buffer_idx = (pre_buffer_idx + 1) % pre_buffer_frames;

        bool speech_detected = false;
        afe_fetch_result_t *res = NULL;
        if (!use_simple_vad) {
            afe_handle->feed(afe_data, audio_buffer);
            res = afe_handle->fetch(afe_data);
            if (res) {
                speech_detected = (res->vad_state == VAD_SPEECH);
            }
        } else {
            float rms = compute_rms(audio_buffer, AFE_FRAME_SIZE);
            speech_detected = (rms > VAD_RMS_THRESHOLD);
        }

        if (state == STATE_WAITING) {
            if (speech_detected) {
                consecutive_speech_frames++;
                if (consecutive_speech_frames >= MIN_SPEECH_FRAMES_TO_START) {
                    ESP_LOGI(TAG, "🎙️ Speech detected, starting recording...");
                    state = STATE_RECORDING;
                    consecutive_silent_frames = 0;
                    recording_start_time = esp_timer_get_time();
                    f = fopen(file_path, "wb");
                    if (!f) {
                        ESP_LOGE(TAG, "Failed to open '%s' for write", file_path);
                        break;
                    }
                    // Write the pre-buffer content
                    for (int i = 0; i < pre_buffer_frames; i++) {
                        int idx = (pre_buffer_idx + i) % pre_buffer_frames;
                        fwrite(&pre_buffer[idx * AFE_FRAME_SIZE], 1, bytes_read, f);
                    }
                }
            } else {
                consecutive_speech_frames = 0;
            }
        }
        
        if (state == STATE_RECORDING) {
            // Write current audio data
            if (f) {
                 if (!use_simple_vad && res) {
                    fwrite(res->data, 1, res->data_size, f);
                } else {
                    fwrite(audio_buffer, 1, bytes_read, f);
                }
            }

            if (speech_detected) {
                consecutive_silent_frames = 0;
            } else {
                consecutive_silent_frames++;
                if (consecutive_silent_frames >= MAX_SILENT_FRAMES_TO_STOP) {
                    ESP_LOGI(TAG, "🛑 Silence detected, stopping recording.");
                    break;
                }
            }

            if ((esp_timer_get_time() - recording_start_time) / 1000 > AFE_MAX_RECORDING_MS) {
                ESP_LOGW(TAG, "⚠️ Max recording time reached, stopping.");
                break;
            }
        }
    }

    free(pre_buffer);

    if (f) {
        fclose(f);

        struct stat st;
        if (stat(file_path, &st) == 0) {
            // Minimum audio size: 0.5 seconds of 16kHz, 16-bit audio
            const int min_audio_size = (SAMPLE_RATE * (16 / 8) * 500) / 1000;
            if (st.st_size >= min_audio_size) {
                ESP_LOGI(TAG, "💾 Saved %s (%ld bytes)", file_path, st.st_size);
                free(audio_buffer);
                return ESP_OK;
            } else {
                ESP_LOGI(TAG, "🗑️ Recording too short (%ld bytes), deleting.", st.st_size);
                unlink(file_path);
            }
        } else {
            ESP_LOGE(TAG, "Failed to get file stats for %s", file_path);
        }
    }

    free(audio_buffer);
    return ESP_FAIL;
}
