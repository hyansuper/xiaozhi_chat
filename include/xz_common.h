#pragma once

#include "esp_err.h"

typedef enum {
    XZ_PROT_TYPE_UNKOWN = 0,
    XZ_PROT_TYPE_MQTT,
    XZ_PROT_TYPE_WS,
} xz_prot_type_t;

// this function must be called first before any xz api
esp_err_t xz_board_info_load();