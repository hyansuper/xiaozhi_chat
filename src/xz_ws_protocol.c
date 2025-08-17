#include "xz_protocol_priv.h"
#include "xz_chat_priv.h"
#include "xz_util.h"
#include "xz_board_info.h"
#include "ext_mjson.h"
#include "esp_check.h"
#ifndef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    #include "esp_crt_bundle.h"
#endif


static const char* const TAG = "xz_ws";

#define OPUS_FRAME_DURATION_MS 60
#define OPUS_FRAME_DURATION_MS_PR "60"

#define WS_PROT_DEFAULT_VERSION 1
#define WS_PROT_DEFAULT_VERSION_PR "1"

struct BinaryProtocol2 {
    uint16_t version;
    uint16_t type;          // Message type (0: OPUS, 1: JSON)
    uint32_t reserved;      // Reserved for future use
    uint32_t timestamp;     // Timestamp in milliseconds (used for server-side AEC)
    uint32_t payload_size;  // Payload size in bytes
    uint8_t payload[];      // Payload data
} __attribute__((packed));

struct BinaryProtocol3 {
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size;
    uint8_t payload[];
} __attribute__((packed));

void xz_ws_prot_config_fill_rest_from_response(xz_ws_prot_config_t* conf, struct xz_http_client_resp_ws* resp) {
    char* headers = conf->headers;
    int hlen = 0;
    if(resp->tok) {
        hlen = sprintf(headers, "Authorization: %s%s\r\n", strchr(resp->tok, ' ')? "":"Bearer ", resp->tok);
    }
    conf->version = resp->ver? resp->ver: WS_PROT_DEFAULT_VERSION;
    sprintf(&headers[hlen], "Protocol-Version: %d\r\nDevice-Id: %s\r\nClient-Id: %s\r\n", conf->version, xz_board_info_mac(), xz_board_info_uuid());
    conf->client_conf.headers = headers;
    conf->client_conf.uri = resp->url;
}

void xz_ws_prot_config_set_default(xz_ws_prot_config_t* conf) {
    conf->client_conf = (esp_websocket_client_config_t) {
        .disable_auto_reconnect = true,
        // .enable_close_reconnect = false, // reconnect after server close
        .buffer_size = (15000+32), // 15000 for audio frame, add some more for payload header,
        #ifndef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
            .crt_bundle_attach = esp_crt_bundle_attach,
        #endif
        #ifdef CONFIG_XZ_PROT_SKIP_COMMON_NAME_CHECK
            .skip_cert_common_name_check = true,
        #endif
    };
}

static esp_err_t xz_ws_prot_send_msg(xz_chat_t* chat, const char* str, int len) {
    xz_ws_prot_ctx_t* ctx = (xz_ws_prot_ctx_t*)chat->prot_ctx;
    return esp_websocket_client_send_text(ctx->ws_hd, str, len, pdMS_TO_TICKS(2000))>=0? ESP_OK: ESP_FAIL;
}

static esp_err_t xz_ws_prot_send_data(xz_chat_t* chat, const void* data, int len) {
    xz_ws_prot_ctx_t* ctx = (xz_ws_prot_ctx_t*)chat->prot_ctx;
    int needed_size;
    switch(ctx->version) {
        case 2: needed_size = sizeof(struct BinaryProtocol2) + len; break;
        case 3: needed_size = sizeof(struct BinaryProtocol3) + len; break;
        default: needed_size = 0;
    }
    if(ctx->send_audio_buf_size < needed_size) {
        void* tmp = realloc(ctx->send_audio_buf, needed_size);
        if(tmp) {
            ctx->send_audio_buf = tmp;
            ctx->send_audio_buf_size = needed_size;
        } else {
            return ESP_ERR_NO_MEM;
        }
    }
    void* data_to_send;
    switch(ctx->version) {
        case 2:
            struct BinaryProtocol2* p2 = ctx->send_audio_buf;
            p2->version = htons(2);
            p2->type = 0;
            p2->reserved = 0;
            p2->timestamp = 0;
            p2->payload_size = htonl(len);
            memcpy(p2->payload, data, len);
            data_to_send = ctx->send_audio_buf;
            break;
        case 3:
            struct BinaryProtocol3* p3 = ctx->send_audio_buf;
            p3->type = 0;
            p3->reserved = 0;
            p3->payload_size = htons(len);
            memcpy(p3->payload, data, len);
            data_to_send = ctx->send_audio_buf;
            break;
        default:
            data_to_send = data;
            needed_size = len;
    }
    return esp_websocket_client_send_bin(ctx->ws_hd, data_to_send, needed_size, pdMS_TO_TICKS(2000))>=0? ESP_OK: ESP_FAIL;
}

esp_err_t xz_ws_prot_destroy(xz_ws_prot_ctx_t* ctx) {
    if(!ctx) return ESP_OK;
    esp_err_t ret = esp_websocket_client_destroy(ctx->ws_hd);
    if(!ret) ctx->ws_hd = NULL;
    return ret;
}

static void log_error_if_nonzero(const char *message, int error_code) {
    if (error_code)
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
}

static void websocket_event_handler(xz_chat_t *chat, esp_event_base_t base, int32_t event_id, esp_websocket_event_data_t *ev) {
    switch (event_id) {
    case WEBSOCKET_EVENT_DATA:{
            xz_ws_prot_ctx_t* ctx = (xz_ws_prot_ctx_t*) chat->prot_ctx;
            // ESP_LOGI(TAG, "Received opcode=%d, fin=%d", ev->op_code, ev->fin);

            /*
             even for non-fragmented payload, multiple events could be fired for the same message if client->rx_buffer is too small.
             for effiency,
                1. I prefer to setting client->rx_buffer large enough to load audio bin data.
                2. modification to esp_websocket_client is needed to allow cpp-move kind of op and avoid memcpy
            */
            if(ev->data_len < ev->payload_len) {  // multiple events for a message
                ESP_LOGE(TAG, "recv buf too small");
                return;
            }
            if( ev->fin==false || ev->op_code==0) { // fragments
                ESP_LOGE(TAG, "fragments handling not implemented");
                return;
            }
            if (ev->op_code == 0x2) { // bin // process audio ev->data_ptr, ev->data_len
                if(chat->audio_cb) {
                    uint8_t* audio_data; int audio_len;
                    switch(ctx->version) {
                    case 2:
                        struct BinaryProtocol2* p2 = (struct BinaryProtocol2*)ev->data_ptr;
                        // p2->version = ntohs(p2->version);
                        // p2->type = ntohs(p2->type);
                        // p2->timestamp = ntohl(p2->timestamp);
                        // p2->payload_size = ntohl(p2->payload_size);
                        audio_data = p2->payload;
                        audio_len = ntohl(p2->payload_size);
                        break;
                    case 3:
                        struct BinaryProtocol3* p3 = (struct BinaryProtocol3*)ev->data_ptr;
                        // p3->payload_size = ntohs(p3->payload_size);
                        audio_data = p3->payload;
                        audio_len = ntohs(p3->payload_size);
                        break;
                    default:
                        audio_data = ev->data_ptr;
                        audio_len = ev->data_len;
                    }
                    if(chat_has_any_flag(chat, XZ_FLAG_SESS_SPEAKING))
                        chat->audio_cb(audio_data, audio_len, chat);
                }

            } else if(ev->op_code == 0x1) { // txt
                int len = ev->data_len;
                char* data = ev->data_ptr;
                ESP_LOGI(TAG, "got msg %.*s", len, data);
                const char* s; int n;
                if(!emjson_locate_string(data, len, "$.type", &s, &n)) {
                    ESP_LOGE(TAG, "Message type is not specified");
                    return;
                }
                if(QESTREQL(s, "hello")) {
                    if(!(emjson_locate_string(data, len, "$.transport", &s, &n) && QESTREQL(s, "websocket"))) {
                        ESP_LOGE(TAG, "Unsupported transport");
                        return;
                    }
                    chat->session_buf = strndup(data, len);
                    data = chat->session_buf;
                    if(mjson_find(data, len, "$.audio_params", &s, &n) == MJSON_TOK_OBJECT) {
                        emjson_get_i32(s, n, "$.sample_rate", &chat->server_sample_rate);
                        emjson_get_i32(s, n, "$.frame_duration", &chat->server_frame_duration);
                    }
                    chat->session_id = "";
                    if(emjson_locate_string(data, len, "$.session_id", (const char**)&chat->session_id, &n)) {
                        chat->session_id[n] = 0;
                    }
                    xEventGroupSetBits(chat->eg, XZ_EG_SERVER_HELLO_BIT);
                } else {
                    xz_prot_process_json(chat, data, len, s, n);
                }
            }
        }

        // ESP_LOGW(TAG, "Total payload length=%d, data_len=%d, current payload offset=%d\r\n", ev->payload_len, ev->data_len, ev->payload_offset);

        return;
    case WEBSOCKET_EVENT_BEGIN:
        ESP_LOGI(TAG, "ev_begin");
        return;
    case WEBSOCKET_EVENT_CONNECTED:
        xEventGroupSetBits(chat->eg, XZ_EG_PROT_CONN_BIT);
        ESP_LOGI(TAG, "ev_conn");
        return;
    case WEBSOCKET_EVENT_FINISH: // normally FIN is received instead of DISCONN
        xEventGroupSetBits(chat->eg, XZ_EG_PROT_DISCONN_BIT);
        ESP_LOGI(TAG, "ev_fin");
        RELEASE(chat->session_buf); // release session_buf so client's attempt to send further msg will fail
        xz_chat_exit_session(chat);
        return;
    case WEBSOCKET_EVENT_DISCONNECTED:
        xEventGroupSetBits(chat->eg, XZ_EG_PROT_DISCONN_BIT);
        ESP_LOGI(TAG, "ev_disconn");
        RELEASE(chat->session_buf);
        xz_chat_exit_session(chat);
        goto check_err;
    case WEBSOCKET_EVENT_ERROR:
        xEventGroupSetBits(chat->eg, XZ_EG_PROT_ERR_BIT);
        ESP_LOGI(TAG, "ev_error");
        goto check_err;
    }

check_err:
    log_error_if_nonzero("HTTP status code",  ev->error_handle.esp_ws_handshake_status_code);
    if (ev->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
        log_error_if_nonzero("reported from esp-tls", ev->error_handle.esp_tls_last_esp_err);
        log_error_if_nonzero("reported from tls stack", ev->error_handle.esp_tls_stack_err);
        log_error_if_nonzero("captured as transport's socket errno",  ev->error_handle.esp_transport_sock_errno);
    }
}

esp_err_t xz_ws_prot_init(xz_ws_prot_ctx_t** ctx, xz_ws_prot_config_t* conf, xz_chat_t* chat) {
    xz_ws_prot_ctx_t* p = calloc(1, sizeof(xz_ws_prot_ctx_t));
    if(p == NULL) return ESP_ERR_NO_MEM;
    esp_err_t ret = ESP_OK;
    p->version = conf->version;
    ESP_GOTO_ON_FALSE((p->ws_hd=esp_websocket_client_init(&conf->client_conf)), ESP_ERR_NO_MEM, err, TAG, "create ws client");
    ESP_GOTO_ON_ERROR(esp_websocket_register_events(p->ws_hd, WEBSOCKET_EVENT_ANY, (esp_event_handler_t)websocket_event_handler, chat), err, TAG, "register event");
err:
    if(ret) {
        xz_ws_prot_destroy(p);
        p = NULL;
    }
    *ctx = p;
    return ret;
}

static esp_err_t do_nothing_wrong(xz_chat_t* chat) {
    return ESP_OK;
}


static esp_err_t xz_ws_prot_close_audio_chan(xz_chat_t* chat) {
    if(!chat) return ESP_ERR_INVALID_ARG;
    xz_ws_prot_ctx_t* ctx = (xz_ws_prot_ctx_t*)chat->prot_ctx;
    if(!ctx) return ESP_ERR_INVALID_STATE;
    RELEASE(chat->session_buf);
    RELEASE(ctx->send_audio_buf);
    ctx->send_audio_buf_size = 0;
    return esp_websocket_client_stop(ctx->ws_hd);
}

#define XZ_WS_PROT_OPEN_AUDIO_CMD "{\"type\":\"hello\",\"version\":"WS_PROT_DEFAULT_VERSION_PR",\"transport\":\"websocket\",\"audio_params\":{" \
                    "\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":"OPUS_FRAME_DURATION_MS_PR"}}"
static esp_err_t xz_ws_prot_open_audio_chan(xz_chat_t* chat) {
    if(!chat) return ESP_ERR_INVALID_ARG;
    xz_ws_prot_ctx_t* ctx = (xz_ws_prot_ctx_t*)chat->prot_ctx;
    if(!ctx) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_OK;
    ctx->send_audio_buf = NULL;
    ctx->send_audio_buf_size = 0;
    xEventGroupClearBits(chat->eg, XZ_EG_PROT_CONN_BIT|XZ_EG_PROT_DISCONN_BIT|XZ_EG_PROT_ERR_BIT);
    ESP_RETURN_ON_ERROR(esp_websocket_client_start(ctx->ws_hd), TAG, "start ws client");
    ESP_GOTO_ON_FALSE(XZ_EG_PROT_CONN_BIT&xEventGroupWaitBits(chat->eg, XZ_EG_PROT_CONN_BIT|XZ_EG_PROT_DISCONN_BIT|XZ_EG_PROT_ERR_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000)), ESP_ERR_TIMEOUT, err, TAG, "wait conn");

    xEventGroupClearBits(chat->eg, XZ_EG_PROT_DISCONN_BIT|XZ_EG_PROT_ERR_BIT|XZ_EG_SERVER_HELLO_BIT);
    ESP_GOTO_ON_ERROR(xz_ws_prot_send_msg(chat, XZ_WS_PROT_OPEN_AUDIO_CMD, sizeof(XZ_WS_PROT_OPEN_AUDIO_CMD)-1), err, TAG, "send hello");
    ESP_GOTO_ON_FALSE(XZ_EG_SERVER_HELLO_BIT&xEventGroupWaitBits(chat->eg, XZ_EG_SERVER_HELLO_BIT|XZ_EG_PROT_DISCONN_BIT|XZ_EG_PROT_ERR_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000)), ESP_ERR_TIMEOUT, err, TAG, "wait server hello");
err:
    if(ret) {
        xz_ws_prot_close_audio_chan(chat);
    }
    return ESP_OK;
}

const xz_prot_if_t xz_ws_prot_if = {
    .start = do_nothing_wrong,
    .stop = do_nothing_wrong,
    .open_audio_chan = xz_ws_prot_open_audio_chan,
    .close_audio_chan = xz_ws_prot_close_audio_chan,
    .send_msg = xz_ws_prot_send_msg,
    .send_data = xz_ws_prot_send_data,
};