#pragma once
#include "esp_err.h"

void network_init(void);
esp_err_t network_upload_file(const char *filepath);
const char* network_get_server_url(void);
