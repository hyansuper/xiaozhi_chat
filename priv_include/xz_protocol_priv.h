#pragma once
#include "xz_chat.h"
#include "xz_protocol.h"
#include "xz_http_client_request.h"
#include <mbedtls/aes.h>
#include "task_util.h"

typedef esp_err_t (*xz_prot_fn_t)(xz_chat_t* chat);
typedef esp_err_t (*xz_prot_send_msg_fn_t)(xz_chat_t* chat, const char* msg, int len);
typedef esp_err_t (*xz_prot_send_data_fn_t)(xz_chat_t* chat, const void* data, int len);


typedef struct {
    xz_prot_fn_t start, stop, open_audio_chan, close_audio_chan;
    xz_prot_send_msg_fn_t send_msg;
    xz_prot_send_data_fn_t send_data;
} xz_prot_if_t;

void xz_prot_process_json( xz_chat_t* chat, char*  json,  int len,  char*  type,  int tlen);


/* ws */

typedef struct {
    esp_websocket_client_handle_t ws_hd;
    int version;
    void* send_audio_buf;
    int send_audio_buf_size;
} xz_ws_prot_ctx_t;


void xz_ws_prot_config_set_default(xz_ws_prot_config_t* conf);
void xz_ws_prot_config_fill_rest_from_response(xz_ws_prot_config_t* conf, struct xz_http_client_resp_ws* resp);
esp_err_t xz_ws_prot_init(xz_ws_prot_ctx_t** ctx, xz_ws_prot_config_t* conf, xz_chat_t* chat);
esp_err_t xz_ws_prot_destroy(xz_ws_prot_ctx_t* ctx);

extern const xz_prot_if_t xz_ws_prot_if;

/* mqtt */

typedef struct {
    esp_mqtt_client_handle_t mqtt_hd;
    char* pub_topic;
    struct {
        capped_task_config_t task_conf;
        int sock;
        char* server;
        int port;
        char* aes_nonce;
        int aes_nonce_len;
        uint8_t* encrypted_buf;
        int encrypted_buf_size;
        mbedtls_aes_context aes_ctx;
        uint32_t remote_sequence;
        uint32_t local_sequence;
        TaskHandle_t task_hd;
        uint8_t* recv_buf;
        int recv_buf_size;
    } udp;

} xz_mqtt_prot_ctx_t;

void xz_mqtt_prot_config_set_default(xz_mqtt_prot_config_t* conf);
void xz_mqtt_prot_config_fill_rest_from_response(xz_mqtt_prot_config_t* conf, struct xz_http_client_resp_mqtt* resp);
esp_err_t xz_mqtt_prot_init(xz_mqtt_prot_ctx_t** ctx, xz_mqtt_prot_config_t* conf, xz_chat_t* chat);
esp_err_t xz_mqtt_prot_destroy(xz_mqtt_prot_ctx_t* ctx);

extern const xz_prot_if_t xz_mqtt_prot_if;