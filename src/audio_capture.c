#include "audio_capture.h"
#include <stdio.h>
#include <malloc.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
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
#define AFE_FRAME_SIZE (320) // 20ms * 16kHz * 16bit / 8 / 2 channels = 320 bytes
#define AFE_SILENCE_TIMEOUT_MS 3000
#define AFE_MAX_RECORDING_MS 15000

// global handles
static i2s_chan_handle_t rx_handle = NULL;
static esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t *afe_data = NULL;

void audio_capture_init(void)
{
    // Init I2S
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 1024;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
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

    // Init AFE
    afe_config_t *afe_config = afe_config_init("M", NULL, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config) {
        ESP_LOGE(TAG, "Failed to init afe config");
        return;
    }
    afe_config->vad_init = true;
    afe_config->vad_mode = VAD_MODE_3;
    
    afe_handle = esp_afe_handle_from_config(afe_config);
    if (!afe_handle) {
        ESP_LOGE(TAG, "Failed to get afe handle");
        afe_config_free(afe_config);
        return;
    }

    afe_data = afe_handle->create_from_config(afe_config);
    if (!afe_data) {
        ESP_LOGE(TAG, "Failed to create AFE");
        afe_config_free(afe_config);
        return;
    }
    
    afe_config_free(afe_config);
    ESP_LOGI(TAG, "✅ AFE initialized");
}

esp_err_t audio_capture_wait_for_speech_and_record(const char *file_path)
{
    int16_t *audio_buffer = (int16_t *)malloc(AFE_FRAME_SIZE * sizeof(int16_t));
    if (!audio_buffer) {
        ESP_LOGE(TAG, "malloc failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "👂 Waiting for speech...");

    bool is_speaking = false;
    int64_t silence_start_time = 0;
    int64_t recording_start_time = 0;
    FILE *f = NULL;

    while (true)
    {
        size_t bytes_read = 0;
        esp_err_t r = i2s_channel_read(rx_handle, audio_buffer, AFE_FRAME_SIZE * sizeof(int16_t), &bytes_read, portMAX_DELAY);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "i2s read failed: %s", esp_err_to_name(r));
            break;
        }

        afe_handle->feed(afe_data, audio_buffer);
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (!res) {
            continue;
        }

        if (res->vad_state == VAD_SPEECH && !is_speaking) {
            is_speaking = true;
            recording_start_time = esp_timer_get_time();
            ESP_LOGI(TAG, "🎙️ Speech detected, starting recording...");
            f = fopen(file_path, "wb");
            if (!f) {
                ESP_LOGE(TAG, "Failed to open '%s' for write", file_path);
                break;
            }
        }

        if (is_speaking) {
            fwrite(res->data, 1, res->data_size, f);

            if (res->vad_state == VAD_SPEECH) {
                silence_start_time = 0; // reset silence timer
            } else {
                if (silence_start_time == 0) {
                    silence_start_time = esp_timer_get_time();
                }
                if ((esp_timer_get_time() - silence_start_time) / 1000 > AFE_SILENCE_TIMEOUT_MS) {
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

    if (f) {
        fclose(f);
        ESP_LOGI(TAG, "💾 Saved %s", file_path);
    }
    free(audio_buffer);

    return is_speaking ? ESP_OK : ESP_FAIL;
}