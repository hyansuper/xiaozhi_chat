#include "xz_http_client_request.h"
#include "xz_board_info.h"
#include "ext_mjson.h"
#include "esp_check.h"

const static char* const TAG = "xz_actv";

esp_err_t xz_http_client_set_headers(esp_http_client_handle_t client, const char* lang) {
    return esp_http_client_set_header(client, "actvation-Version", "1")
        || esp_http_client_set_header(client, "Device-Id", xz_board_info_mac())
        || esp_http_client_set_header(client, "Client-Id", xz_board_info_uuid())
        || esp_http_client_set_header(client, "User-Agent", XZ_BOARD_NAME "/" XZ_VER)
        || esp_http_client_set_header(client, "Accept-Language", lang)
        || esp_http_client_set_header(client, "Content-Type", "application/json");
}


esp_err_t xz_http_client_version_check(esp_http_client_handle_t client, const char* url, xz_prot_type_t prot_pref, char* buf, int len, xz_http_client_response_t* resp) {

    char* lang = NULL;
    esp_http_client_get_header(client, "Accept-Language", &lang);
    if(lang==NULL || *lang==0) lang="en-US";
    len = xz_board_info_printf(buf, len, lang);
    ESP_LOGD(TAG, "version_check post: %.*s", len, buf);
    ESP_RETURN_ON_ERROR(http_client_util_post(client, buf, &len, buf, len, url), TAG, "send post");
    ESP_LOGI(TAG, "version_check resp: %.*s", len, buf);

    const char* s; int n;
    if((resp->require_activation= mjson_find(buf, len, "$.activation", &s, &n)==MJSON_TOK_OBJECT)) {
        emjson_find_string_batch(s, n, "$.message", &resp->activation.message,
                                        "$.code", &resp->activation.code,
                                        "$.challenge", &resp->activation.challenge,
                                        NULL);
        emjson_get_i32(s, n, "$.timeout_ms", &resp->activation.timeout_ms);
    }

    resp->prot_type = XZ_PROT_TYPE_UNKOWN;
    if(prot_pref==XZ_PROT_TYPE_WS && mjson_find(buf, len, "$.websocket", &s, &n)==MJSON_TOK_OBJECT) {
        resp->ws.ver = 0;
        emjson_get_i32(s, n, "$.version", &resp->ws.ver);
        emjson_find_string_batch(s, n, "$.url", &resp->ws.url,
                                        "$.token", &resp->ws.tok,
                                        NULL);
        resp->prot_type = XZ_PROT_TYPE_WS;
    } else if(mjson_find(buf, len, "$.mqtt", &s, &n)==MJSON_TOK_OBJECT) {
        emjson_find_string_batch(s, n, "$.endpoint", &resp->mqtt.endpoint,
                                        "$.username", &resp->mqtt.uname,
                                        "$.client_id", &resp->mqtt.cid,
                                        "$.password", &resp->mqtt.pass,
                                        "$.publish_topic", &resp->mqtt.pub,
                                        NULL);
        resp->prot_type = XZ_PROT_TYPE_MQTT;
    }  
    if(resp->require_activation) {
        emjson_truncate_string_batch(resp->activation.message,
                                    resp->activation.code,
                                    resp->activation.challenge,
                                    NULL);
    }
    if(resp->prot_type==XZ_PROT_TYPE_MQTT) {
        emjson_truncate_string_batch(resp->mqtt.endpoint,
                                    resp->mqtt.uname,
                                    resp->mqtt.cid,
                                    resp->mqtt.pass,
                                    resp->mqtt.pub,
                                    NULL);
        resp->mqtt.port = 8883;
        char* p = strchr(resp->mqtt.endpoint, ':');
        if(p) { *p = 0; resp->mqtt.port = atoi(p+1); }
    } else if(resp->prot_type==XZ_PROT_TYPE_WS) {
        emjson_truncate_string_batch(resp->ws.url,
                                    resp->ws.tok,
                                    NULL);
    }

    return ESP_OK;
}


esp_err_t xz_http_client_activation_check(esp_http_client_handle_t client, const char* url) {
    char buf[128];
    int len = sizeof(buf);
    esp_err_t ret;
    ESP_RETURN_ON_ERROR(http_client_util_post(client, buf, &len, "{}", 2, url), TAG, "send post");
    ESP_LOGI(TAG, "activation_check resp: %.*s", len, buf);
    int status_code = esp_http_client_get_status_code(client);
    switch(status_code) {
        case 200: return ESP_OK;
        case 202: return ESP_ERR_TIMEOUT;
        default: return ESP_FAIL;
    }
}