#pragma once
#include "esp_err.h"

void network_init(const char *ssid, const char *pass);
esp_err_t network_upload_file(const char *url, const char *filepath);
