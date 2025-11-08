#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_netif.h"

#define TAG "buddy"

// ======== AUDIO CONFIG =========
#define SAMPLE_RATE 16000
#define I2S_PORT I2S_NUM_0
#define I2S_BCK 14
#define I2S_WS  15
#define I2S_SD  32

// ======== SD CARD CONFIG =========
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS    5

// ======== WIFI CONFIG =========
#define WIFI_SSID "M01s"
#define WIFI_PASS "9924760032"
#define SERVER_URL "http://192.168.34.32:8000/process"

// ======== STATE =========
static i2s_chan_handle_t rx_handle;
static sdmmc_card_t *card;

// ======== HELPERS =========
static void init_i2s(void) {
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
            .ws   = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_SD
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "✅ I2S initialized");
}

static void init_sdcard(void) {
    ESP_LOGI(TAG, "🪣 Initializing SD card...");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1
    };
    ESP_ERROR_CHECK(spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA));

    sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_cfg.gpio_cs = PIN_NUM_CS;
    dev_cfg.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5
    };

    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &dev_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "✅ SD card mounted");
}

static void init_wifi(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = { 0 };
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "📶 Wi-Fi connecting...");
}

static void upload_file(const char *path) {
    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "File open failed");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    esp_http_client_open(client, size);

    uint8_t buf[1024];
    size_t read;
    while ((read = fread(buf, 1, sizeof(buf), f)) > 0) {
        esp_http_client_write(client, (const char*)buf, read);
    }
    fclose(f);
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "📤 Upload done, HTTP %d", status);
}

void app_main(void) {
    init_i2s();
    init_sdcard();
    init_wifi();

    FILE *f = fopen("/sdcard/voice.raw", "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file");
        return;
    }

    int16_t buffer[1024];
    size_t bytes_read;
    ESP_LOGI(TAG, "🎙️ Recording...");
    for (int i = 0; i < 200; i++) { // record ~1.5 sec
        i2s_channel_read(rx_handle, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
        fwrite(buffer, 1, bytes_read, f);
    }

    fclose(f);
    ESP_LOGI(TAG, "💾 Saved /sdcard/voice.raw");
    upload_file("/sdcard/voice.raw");
}
