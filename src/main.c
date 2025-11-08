#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "network.h"
#include "storage.h"
#include "audio_capture.h"

#define TAG "buddy"

// --------------------------------------------------
// Main task
// --------------------------------------------------
static void main_task(void *pv)
{
    ESP_LOGI(TAG, "main_task starting...");

    // Initialize modules
    storage_init();
    audio_capture_init();
    network_init();

    ESP_LOGI(TAG, "Initialization done, waiting for Wi-Fi...");
    vTaskDelay(pdMS_TO_TICKS(5000)); // wait for IP assignment

    const char *url = network_get_server_url();
    if (strlen(url) == 0)
    {
        ESP_LOGE(TAG, "⚠️ Failed to get server URL. Cannot continue.");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Connected to Wi-Fi. Server URL: %s", url);

    const char *out_path = "/sdcard/audio.raw";

    while (1)
    {
        // Record audio
        esp_err_t err = audio_capture_wait_for_speech_and_record(out_path);
        if (err != ESP_OK)
        {
            ESP_LOGI(TAG, "No speech detected, waiting again...");
            continue;
        }

        // Upload audio
        ESP_LOGI(TAG, "📤 Uploading %s to server...", out_path);
        int status = network_upload_file(out_path);
        if (status != 200)
        {
            ESP_LOGE(TAG, "Upload failed with status %d", status);
            vTaskDelay(pdMS_TO_TICKS(5000)); // wait before retrying
        }
        else
        {
            ESP_LOGI(TAG, "✅ Upload successful");
        }
    }

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
