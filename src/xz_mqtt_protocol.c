#include "xz_protocol_priv.h"
#include "xz_chat_priv.h"
#include "xz_util.h"
#include "ext_mjson.h"
#include "esp_check.h"
#ifndef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    #include "esp_crt_bundle.h"
#endif
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "task_util.h"
static const char* const TAG = "xz_mqtt";

#define OPUS_FRAME_DURATION_MS 60
#define OPUS_FRAME_DURATION_MS_PR "60"

void xz_mqtt_prot_config_fill_rest_from_response(xz_mqtt_prot_config_t* conf, struct xz_http_client_resp_mqtt* resp) {
    conf->pub_topic = resp->pub;
    conf->client_conf.broker.address.hostname = resp->endpoint;
    conf->client_conf.broker.address.port = resp->port;
    conf->client_conf.broker.address.transport = resp->port==8883? MQTT_TRANSPORT_OVER_SSL: MQTT_TRANSPORT_OVER_TCP;
    conf->client_conf.credentials.username = resp->uname;
    conf->client_conf.credentials.client_id = resp->cid;
    conf->client_conf.credentials.authentication.password = resp->pass;
}

void xz_mqtt_prot_config_set_default(xz_mqtt_prot_config_t* conf) {
    conf->udp_conf.recv_buf_size = 15000;
    conf->udp_conf.task_conf = (capped_task_config_t){
            .prio = 5,
            .stack = 1024*3,
            .caps = XZ_CHAT_TASK_CAPS,
            .core = tskNO_AFFINITY,
        };
    conf->client_conf = (esp_mqtt_client_config_t){
        .broker = {
            .verification = {
                #ifndef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
                    .crt_bundle_attach = esp_crt_bundle_attach,
                #endif
                #ifdef CONFIG_XZ_PROT_SKIP_COMMON_NAME_CHECK
                    .skip_cert_common_name_check = true,
                #endif
            },
        },
        .session.keepalive = 90, // sec
    };
}

static esp_err_t xz_mqtt_prot_send_msg(xz_chat_t* chat, const char* buf, int len) {
    xz_mqtt_prot_ctx_t* ctx = (xz_mqtt_prot_ctx_t*)chat->prot_ctx;
    return esp_mqtt_client_publish(ctx->mqtt_hd, ctx->pub_topic, buf, len, 0, 0)>=0? ESP_OK: ESP_FAIL;
}

static esp_err_t xz_mqtt_prot_send_data(xz_chat_t* chat, const void* buf, int len) {
    xz_mqtt_prot_ctx_t* ctx = (xz_mqtt_prot_ctx_t*)chat->prot_ctx;
    char* nonce = ctx->udp.aes_nonce;
    *(uint16_t*)&nonce[2] = htons(len);
    *(uint32_t*)&nonce[8] = htonl(0); // timestamp
    *(uint32_t*)&nonce[12] = htonl(++ ctx->udp.local_sequence);

    int needed_size = ctx->udp.aes_nonce_len + len;
    if(ctx->udp.encrypted_buf_size < needed_size) {
        void* tmp = realloc(ctx->udp.encrypted_buf, needed_size);
        if(tmp) {
            ctx->udp.encrypted_buf = tmp;
            ctx->udp.encrypted_buf_size = needed_size;
        } else {
            return ESP_ERR_NO_MEM;
        }
    }
    memcpy(ctx->udp.encrypted_buf, nonce, ctx->udp.aes_nonce_len);
    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if(0!= mbedtls_aes_crypt_ctr(&ctx->udp.aes_ctx, len, &nc_off, (uint8_t*)nonce, stream_block, buf, &ctx->udp.encrypted_buf[ctx->udp.aes_nonce_len])) {// invalid input length
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return ESP_FAIL; 
    }
    return send(ctx->udp.sock, ctx->udp.encrypted_buf, needed_size, 0)>=0? ESP_OK: ESP_FAIL;
}

static esp_err_t xz_mqtt_prot_close_audio_chan(xz_chat_t* chat) {
    if(!chat) return ESP_ERR_INVALID_ARG;
    xz_mqtt_prot_ctx_t* ctx = (xz_mqtt_prot_ctx_t*)chat->prot_ctx;
    if(!ctx) return ESP_ERR_INVALID_STATE;

    if(chat->session_buf) {
        int n = snprintf(chat->send_buf, chat->send_buf_size, "{\"session_id\":\"%s\",\"type\":\"goodbye\"}", chat->session_id);
        xz_mqtt_prot_send_msg(chat, chat->send_buf, n);
        RELEASE(chat->session_buf);
    }
    if(ctx->udp.sock != -1) {
        close(ctx->udp.sock);
        ctx->udp.sock = -1;
    }
    RELEASE(ctx->udp.encrypted_buf);
    ctx->udp.encrypted_buf_size = 0;
    mbedtls_aes_free(&ctx->udp.aes_ctx);
    return ESP_OK;
}

// if gethostbyname fails, out_addr will remain unchange.
// it's your job to initialize out_addr's other property if required.
static esp_err_t str2sockaddr(const char* hostname, int port, struct sockaddr_in* out_addr) {
    struct hostent *host = gethostbyname(hostname);
    if(host==NULL) return ESP_FAIL;
    out_addr->sin_family = AF_INET;
    out_addr->sin_port = htons(port);
    memcpy(&out_addr->sin_addr, host->h_addr, host->h_length);
    return ESP_OK;
}

#define XZ_MQTT_PROT_OPEN_AUDIO_CMD "{\"type\":\"hello\",\"version\":3,\"transport\":\"udp\",\"audio_params\":{" \
                    "\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":"OPUS_FRAME_DURATION_MS_PR"}}"
static esp_err_t xz_mqtt_prot_open_audio_chan(xz_chat_t* chat) {
    if(!chat) return ESP_ERR_INVALID_ARG;
    xz_mqtt_prot_ctx_t* ctx = (xz_mqtt_prot_ctx_t*)chat->prot_ctx;
    if(!ctx) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;

    xEventGroupClearBits(chat->eg, XZ_EG_SERVER_HELLO_BIT|XZ_EG_PROT_DISCONN_BIT|XZ_EG_PROT_ERR_BIT);
    ESP_RETURN_ON_ERROR(xz_mqtt_prot_send_msg(chat, XZ_MQTT_PROT_OPEN_AUDIO_CMD, sizeof(XZ_MQTT_PROT_OPEN_AUDIO_CMD)-1), TAG, "send hello");
    ESP_RETURN_ON_FALSE(XZ_EG_SERVER_HELLO_BIT&xEventGroupWaitBits(chat->eg, XZ_EG_SERVER_HELLO_BIT|XZ_EG_PROT_ERR_BIT|XZ_EG_PROT_DISCONN_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000)), ESP_ERR_TIMEOUT, TAG, "wait server hello");
    if((ctx->udp.sock=socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)) <0) {ret= ESP_ERR_NO_MEM; return ret;}
    struct sockaddr_in dest_addr = {0};
    ESP_GOTO_ON_ERROR(str2sockaddr(ctx->udp.server, ctx->udp.port, &dest_addr), err, TAG, "get host by name");
    ESP_GOTO_ON_FALSE(connect(ctx->udp.sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) >=0, ESP_FAIL, err, TAG, "connect");
    ctx->udp.encrypted_buf = NULL;
    ctx->udp.encrypted_buf_size = 0;
    resume_task(ctx->udp.task_hd);
err:
    if(ret) {
        xz_mqtt_prot_close_audio_chan(chat);
    }
    return ret;
}

static void udp_recv_loop(void* arg) {
    xz_chat_t* chat = (xz_chat_t*) arg;
    xz_mqtt_prot_ctx_t* ctx = (xz_mqtt_prot_ctx_t*)chat->prot_ctx;
    while(0== (TASK_STOP_BIT & ulTaskNotifyTake(pdTRUE, portMAX_DELAY))) {
        ESP_LOGI(TAG, "udp recv resume");
        uint8_t* recv_buf = ctx->udp.recv_buf;
        int sock = ctx->udp.sock;
        int n;
        while(0 <= (n=recv(sock, recv_buf, ctx->udp.recv_buf_size, 0))) {
            if(n < ctx->udp.aes_nonce_len) {
                ESP_LOGE(TAG, "Invalid audio packet size: %u", n);
                continue;
            }
            if (recv_buf[0] != 0x01) {
                ESP_LOGE(TAG, "Invalid audio packet type: %x", recv_buf[0]);
                continue;
            }
            uint32_t timestamp = ntohl(*(uint32_t*)&recv_buf[8]);
            uint32_t sequence = ntohl(*(uint32_t*)&recv_buf[12]);
            if (sequence < ctx->udp.remote_sequence) {
                ESP_LOGW(TAG, "Received audio packet with old sequence: %lu, expected: %lu", sequence, ctx->udp.remote_sequence);
                continue;
            }
            if (sequence != ctx->udp.remote_sequence + 1) {
                ESP_LOGW(TAG, "Received audio packet with wrong sequence: %lu, expected: %lu", sequence, ctx->udp.remote_sequence + 1);
            }
            size_t decrypted_size = n - ctx->udp.aes_nonce_len;
            size_t nc_off = 0;
            uint8_t stream_block[16] = {0};
            uint8_t* encrypted = recv_buf + ctx->udp.aes_nonce_len;
            if(0 != mbedtls_aes_crypt_ctr(&ctx->udp.aes_ctx, decrypted_size, &nc_off, (uint8_t*)recv_buf, stream_block, encrypted, encrypted)) { // in-place cryption
                ESP_LOGE(TAG, "Failed to decrypt audio data");
                continue;
            }
            if(chat_has_any_flag(chat, XZ_FLAG_SESS_SPEAKING) && chat->audio_cb)
                chat->audio_cb(encrypted, decrypted_size, chat);
            ctx->udp.remote_sequence = sequence;
        }
        ESP_LOGI(TAG, "udp recv pause");
    }
    ctx->udp.task_hd = NULL;
    xEventGroupSetBits(chat->eg, XZ_EG_UDP_TASK_STOPPED_BIT);
    capped_task_delete(NULL);
}

esp_err_t xz_mqtt_prot_destroy(xz_mqtt_prot_ctx_t* ctx) {
    if(!ctx) return ESP_OK;

    esp_err_t ret = esp_mqtt_client_destroy(ctx->mqtt_hd);
    if(!ret) ctx->mqtt_hd = NULL;
    RELEASE_TASK(ctx->udp.task_hd);

    RELEASE(ctx->udp.recv_buf);
    RELEASE(ctx->pub_topic);

    if(!ret) {
        free(ctx);
    }
    return ret;
}

static void mqtt_event_handler(xz_chat_t* chat, esp_event_base_t base, int32_t event_id, esp_mqtt_event_t *event) {
    switch (event_id) {
    case MQTT_EVENT_DATA: {
        xz_mqtt_prot_ctx_t* ctx = (xz_mqtt_prot_ctx_t*)chat->prot_ctx;

        /*
            handling partial data
        */
        // char* data;
        // if(event->data_len == event->total_data_len) {
        //     data = event->data;
        //     goto process_recv_data;
        // }
        // if(event->topic) { // for truncked message, only the first part has topic
        //     if(chat->recv_buf == NULL)
        //         if(NULL== (chat->recv_buf=malloc(event->total_data_len)))
        //             ESP_LOGE(TAG, "failed malloc mqtt recv buf");
        // }
        // if(chat->recv_buf) {
        //     memcpy(chat->recv_buf+event->current_data_offset, event->data, event->data_len);
        //     if(event->data_len+event->current_data_offset >= event->total_data_len) {
        //         data = chat->recv_buf;
        //         goto process_recv_data;
        //     }
        // }
        // return;

// process_recv_data:
        if(event->data_len != event->total_data_len) { // by defualt, rx_buffer is 1k, should be enough
            ESP_LOGE(TAG, "recv buf too small");
            return;
        }
        char* data = event->data;
        int len = event->total_data_len;
        const char* type; int type_len;
        ESP_LOGI(TAG, "got: %.*s", len, data);
        if(!emjson_locate_string(data, len, "$.type", &type, &type_len)) {
            ESP_LOGE(TAG, "Message type is not specified");
            // goto process_recv_data_end;
            return;
        }
        char* s; int n;
        if(QESTREQL(type, "hello")) {
            if(!(emjson_locate_string(data, len, "$.transport", &s, &n) && QESTREQL(s, "udp"))) {
                ESP_LOGE(TAG, "Unsupported transport");
                // goto process_recv_data_end;
                return;
            }

            chat->session_buf = strndup(data, len);
            data = chat->session_buf;

            char* key;
            if(mjson_find(data, len, "$.udp", &s, &n) != MJSON_TOK_OBJECT) {
                ESP_LOGE(TAG, "UDP is not specified");
                RELEASE(chat->session_buf);
                return;
            }

            ctx->udp.server = emjson_find_string(s, n, "$.server");
            emjson_get_i32(s, n, "$.port", &ctx->udp.port);
            ctx->udp.aes_nonce = emjson_find_string(s, n, "$.nonce");
            emjson_locate_string(s, n, "$.nonce", (const char**)&ctx->udp.aes_nonce, &ctx->udp.aes_nonce_len);
            key = emjson_find_string(s, n, "$.key");

            chat->session_id = emjson_find_string(data, len, "$.session_id");
            if(mjson_find(data, len, "$.audio_params", &s, &n) == MJSON_TOK_OBJECT) {
                emjson_get_i32(s, n, "$.sample_rate", &chat->server_sample_rate);
                emjson_get_i32(s, n, "$.frame_duration", &chat->server_frame_duration);
            }
            emjson_truncate_string_batch(key, ctx->udp.server, ctx->udp.aes_nonce, chat->session_id, NULL);
            dec_hex_i(key);
            dec_hex_i(ctx->udp.aes_nonce);
            ctx->udp.aes_nonce_len /= 2;
            mbedtls_aes_init(&ctx->udp.aes_ctx);
            mbedtls_aes_setkey_enc(&ctx->udp.aes_ctx, (const unsigned char*)key, 128);
            ctx->udp.local_sequence = 0;
            ctx->udp.remote_sequence = 0;
            xEventGroupSetBits(chat->eg, XZ_EG_SERVER_HELLO_BIT);

        } else if(QESTREQL(type, "goodbye")) {
            if(emjson_locate_string(data, len, "$.session_id", &s, &n) && strncmp(chat->session_id, s, n)) {

            } else {
                RELEASE(chat->session_buf); // close audo chan by server, release session buf so client won't send goodbye to server again
                xz_chat_exit_session(chat);
            }
        }
        xz_prot_process_json(chat, data, len, type, type_len);
        return;
    }
    // case MQTT_EVENT_BEFORE_CONNECT:
    //     return;
    // case MQTT_EVENT_SUBSCRIBED:
    //     return;
    case MQTT_EVENT_ERROR:
        xEventGroupSetBits(chat->eg, XZ_EG_PROT_ERR_BIT);
        ESP_LOGI(TAG, "ev_error: %s", esp_err_to_name(event->error_handle->esp_tls_last_esp_err));
        return;
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ev_conn");
        xEventGroupSetBits(chat->eg, XZ_EG_PROT_CONN_BIT);
        return;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "ev_disconn");
        xEventGroupSetBits(chat->eg, XZ_EG_PROT_DISCONN_BIT);
        return;
    default:;
    }
}


esp_err_t xz_mqtt_prot_init(xz_mqtt_prot_ctx_t** ctx, xz_mqtt_prot_config_t* conf, xz_chat_t* chat) {
    xz_mqtt_prot_ctx_t* p = calloc(1, sizeof(xz_mqtt_prot_ctx_t));
    if(p == NULL) return ESP_ERR_NO_MEM;
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE((p->mqtt_hd=esp_mqtt_client_init(&conf->client_conf)), ESP_ERR_NO_MEM, err, TAG, "create mqtt client");
    p->pub_topic = strdup(conf->pub_topic);
    ESP_GOTO_ON_ERROR(esp_mqtt_client_register_event(p->mqtt_hd, ESP_EVENT_ANY_ID, (esp_event_handler_t)mqtt_event_handler, chat), err, TAG, "register event");
    // init udp task
    p->udp.recv_buf_size = conf->udp_conf.recv_buf_size;
    p->udp.task_conf = conf->udp_conf.task_conf;
err:
    if(ret) {
        xz_mqtt_prot_destroy(p);
        p= NULL;
    }
    *ctx = p;
    return ret;
}


static esp_err_t xz_mqtt_prot_stop(xz_chat_t* chat) {
    if(!chat) return ESP_ERR_INVALID_ARG;
    xz_mqtt_prot_ctx_t* ctx = (xz_mqtt_prot_ctx_t*)chat->prot_ctx;
    if(!ctx) return ESP_ERR_INVALID_STATE;

    esp_err_t ret0 = esp_mqtt_client_stop(ctx->mqtt_hd);
    esp_err_t ret1 = term_task_wait(ctx->udp.task_hd, chat->eg, XZ_EG_UDP_TASK_STOPPED_BIT, pdMS_TO_TICKS(5000));
    RELEASE(ctx->udp.recv_buf);
    return ret0 || ret1;
}

static esp_err_t xz_mqtt_prot_start(xz_chat_t* chat) {
    xz_mqtt_prot_ctx_t* ctx = (xz_mqtt_prot_ctx_t*)chat->prot_ctx;
    esp_err_t ret= ESP_OK;
    xEventGroupClearBits(chat->eg, XZ_EG_PROT_CONN_BIT|XZ_EG_PROT_ERR_BIT|XZ_EG_PROT_DISCONN_BIT);
    ESP_GOTO_ON_ERROR(esp_mqtt_client_start(ctx->mqtt_hd), err, TAG, "start mqtt");
    ESP_GOTO_ON_FALSE((ctx->udp.recv_buf=malloc(ctx->udp.recv_buf_size)), ESP_ERR_NO_MEM, err, TAG, "malloc udp recv buf");
    ESP_GOTO_ON_ERROR(capped_task_create(&ctx->udp.task_hd, "udp_task", udp_recv_loop, chat, &ctx->udp.task_conf), err, TAG, "create udp task");
    ESP_GOTO_ON_FALSE(XZ_EG_PROT_CONN_BIT&xEventGroupWaitBits(chat->eg, XZ_EG_PROT_CONN_BIT|XZ_EG_PROT_DISCONN_BIT|XZ_EG_PROT_ERR_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000)), ESP_ERR_TIMEOUT, err, TAG, "wait conn");
err:
    if(ret) {
        xz_mqtt_prot_stop(chat);
    }
    return ret;
}



const xz_prot_if_t xz_mqtt_prot_if = {
    .start = xz_mqtt_prot_start,
    .stop = xz_mqtt_prot_stop,
    .open_audio_chan = xz_mqtt_prot_open_audio_chan,
    .close_audio_chan = xz_mqtt_prot_close_audio_chan,
    .send_msg = xz_mqtt_prot_send_msg,
    .send_data = xz_mqtt_prot_send_data,
};