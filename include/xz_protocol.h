#pragma once
#include "xz_http_client_request.h"
#include "xz_common.h"
#include <esp_websocket_client.h>
#include <mqtt_client.h>
#include "task_util.h"
typedef struct {
    esp_mqtt_client_config_t client_conf;
    char* pub_topic;
    struct {
        capped_task_config_t task_conf;
        int recv_buf_size;
    } udp_conf;
} xz_mqtt_prot_config_t;


typedef struct {
    esp_websocket_client_config_t client_conf;
    int version;
    char headers[200];
} xz_ws_prot_config_t;

