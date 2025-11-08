#include "audio_capture.h"
#include <stdio.h>
#include <malloc.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "AUDIO_CAPTURE"

// ======== AUDIO CONFIG =========
#define SAMPLE_RATE 16000
#define I2S_PORT I2S_NUM_0
#define I2S_BCK 14
#define I2S_WS 15
#define I2S_SD 32

// global i2s handle
static i2s_chan_handle_t rx_handle = NULL;

void audio_capture_init(void)
{
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
}

esp_err_t audio_capture_record_to_file(const char *file_path, int record_duration_ms)
{
    const size_t samples_per_read = 1024;
    size_t buffer_bytes = samples_per_read * sizeof(int16_t);
    int16_t *buffer = (int16_t *)malloc(buffer_bytes);
    if (!buffer)
    {
        ESP_LOGE(TAG, "malloc failed");
        return ESP_FAIL;
    }

    FILE *f = fopen(file_path, "wb");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open '%s' for write", file_path);
        free(buffer);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "🎙️ Recording to %s for %dms...", file_path, record_duration_ms);

    int64_t start_time = esp_timer_get_time();
    while ((esp_timer_get_time() - start_time) / 1000 < record_duration_ms)
    {
        size_t bytes_read = 0;
        esp_err_t r = i2s_channel_read(rx_handle, buffer, buffer_bytes, &bytes_read, portMAX_DELAY);
        if (r != ESP_OK)
        {
            ESP_LOGE(TAG, "i2s read failed: %s", esp_err_to_name(r));
            break;
        }
        fwrite(buffer, 1, bytes_read, f);
    }

    fclose(f);
    free(buffer);
    ESP_LOGI(TAG, "💾 Saved %s", file_path);
    return ESP_OK;
}
