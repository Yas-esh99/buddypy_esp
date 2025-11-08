#include "audio_capture.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define TAG "AUDIO_CAPTURE"

#define SAMPLE_RATE 16000
#define I2S_PORT I2S_NUM_0

static i2s_chan_handle_t rx_handle;

void audio_capture_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 1024;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = 14,
            .ws   = 15,
            .dout = I2S_GPIO_UNUSED,
            .din  = 32
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "I2S initialized using new std driver");
}

size_t audio_capture_read(int16_t *samples, size_t max_samples) {
    size_t bytes_read = 0;
    i2s_channel_read(rx_handle, samples, max_samples * sizeof(int16_t), &bytes_read, portMAX_DELAY);
    return bytes_read / sizeof(int16_t);
}
