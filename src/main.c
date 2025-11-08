#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_err.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"

// storage init function implemented in storage.c
extern void storage_init(void);

#define TAG "buddy"

// ======== AUDIO CONFIG =========
#define SAMPLE_RATE 16000
#define I2S_PORT I2S_NUM_0
#define I2S_BCK 14
#define I2S_WS 15
#define I2S_SD 32

// ======== WIFI CONFIG =========
#define WIFI_SSID "M01s"
#define WIFI_PASS "9924760032"

// Globals for dynamic URL
static char device_ip[32] = {0};
static char server_url[128] = {0};

#define SERVER_IP "192.168.122.32" // your PC's fixed IP
#define SERVER_PORT 8000

// global i2s handle
static i2s_chan_handle_t rx_handle = NULL;

// --------------------------------------------------
// I2S initialization
// --------------------------------------------------
static void init_i2s(void)
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

// --------------------------------------------------
// WiFi event handler
// --------------------------------------------------
// --------------------------------------------------
// WiFi event handler (auto-detects gateway IP)
// --------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(device_ip, sizeof(device_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "✅ Got IP: %s", device_ip);

        snprintf(server_url, sizeof(server_url), "http://%s:%d/process", SERVER_IP, SERVER_PORT);
        ESP_LOGI(TAG, "🌐 Server URL set to: %s", server_url);
    }
}

// --------------------------------------------------
// Initialize Wi-Fi
// --------------------------------------------------
static void init_wifi(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "📶 Wi-Fi started, connecting to '%s'", WIFI_SSID);
}

// --------------------------------------------------
// Upload file using HTTP POST (multipart/form-data)
// --------------------------------------------------
static esp_err_t upload_file_http(const char *url, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGE(TAG, "Cannot open file %s", path);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    const char *boundary = "----ESP32FormBoundary";
    char start_part[512];
    char end_part[128];

    snprintf(start_part, sizeof(start_part),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.raw\"\r\n"
             "Content-Type: application/octet-stream\r\n"
             "\r\n",
             boundary);

    snprintf(end_part, sizeof(end_part),
             "\r\n--%s--\r\n", boundary);

    long total_size = strlen(start_part) + file_size + strlen(end_part);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        fclose(f);
        return ESP_FAIL;
    }

    char content_type[128];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, total_size);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_http_client_open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(f);
        return err;
    }

    esp_http_client_write(client, start_part, strlen(start_part));

    uint8_t buffer[1024];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0)
    {
        int written = esp_http_client_write(client, (const char *)buffer, read);
        if (written < 0)
        {
            ESP_LOGE(TAG, "esp_http_client_write failed");
            break;
        }
    }

    esp_http_client_write(client, end_part, strlen(end_part));

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Upload finished, HTTP status: %d", status);

    esp_http_client_cleanup(client);
    fclose(f);
    return ESP_OK;
}

// --------------------------------------------------
// Main task
// --------------------------------------------------
static void main_task(void *pv)
{
    ESP_LOGI(TAG, "main_task starting...");

    storage_init(); // mount SD card
    init_i2s();
    init_wifi();

    ESP_LOGI(TAG, "Initialization done, waiting for Wi-Fi...");
    vTaskDelay(pdMS_TO_TICKS(4000)); // wait for IP assignment

    if (strlen(server_url) == 0)
    {
        ESP_LOGW(TAG, "⚠️ No IP yet — using fallback endpoint");
        strcpy(server_url, "http://192.168.178.32:8000/process");
    }

    const char *out_path = "/sdcard/audio.raw";

    const size_t samples_per_read = 1024;
    size_t buffer_bytes = samples_per_read * sizeof(int16_t);
    int16_t *buffer = (int16_t *)malloc(buffer_bytes);
    if (!buffer)
    {
        ESP_LOGE(TAG, "malloc failed");
        vTaskDelete(NULL);
        return;
    }

    FILE *f = fopen(out_path, "wb");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open '%s' for write", out_path);
        free(buffer);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "🎙️ Recording to %s ...", out_path);

    const int reads = 75; // ~19s
    for (int i = 0; i < reads; ++i)
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
    ESP_LOGI(TAG, "💾 Saved %s", out_path);

    ESP_LOGI(TAG, "📤 Uploading to server %s...", server_url);
    upload_file_http(server_url, out_path);

    free(buffer);
    ESP_LOGI(TAG, "main_task finished");
    vTaskDelete(NULL);
}

// --------------------------------------------------
// app_main entry
// --------------------------------------------------
void app_main(void)
{
    const int stack_size = 8192;
    xTaskCreatePinnedToCore(main_task, "main_task", stack_size, NULL, 5, NULL, tskNO_AFFINITY);
}
