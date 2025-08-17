#pragma once
#include "http_client_util.h"
#include "xz_common.h"

struct xz_http_client_resp_mqtt {
    char* endpoint;
    int port;
    char* pub;
    char* cid;
    char* pass;
    char* uname;
};
struct xz_http_client_resp_ws {
    char* url;
    char* tok;
    int ver;
};

typedef struct {
    xz_prot_type_t prot_type;
    union {
        struct xz_http_client_resp_mqtt mqtt;
        struct xz_http_client_resp_ws ws;
    };

    bool require_activation;
    struct {
        char* code;
        char* challenge;
        char* message;
        uint32_t timeout_ms;
    } activation;
    
    char buf[];
} xz_http_client_response_t;


esp_err_t xz_http_client_set_headers(esp_http_client_handle_t client, const char* lang);

/*
 client must have headers set
*/
esp_err_t xz_http_client_version_check(esp_http_client_handle_t client, const char* url, xz_prot_type_t preferred_protocol, char* buf, int len, xz_http_client_response_t* resp);

/*
 client must have headers set.
 returning ESP_OK if device is activated,
 ESP_ERR_TIMEOUT or ESP_FAIL if not.
*/
esp_err_t xz_http_client_activation_check(esp_http_client_handle_t client, const char* url);