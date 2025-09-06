#pragma once
#include "esp_err.h"
#include "xz_http_client_request.h"
#include "xz_protocol.h"
#include "xz_common.h"
#include "task_util.h"

typedef enum {
    XZ_ABORT_REASON_NONE,
    XZ_ABORT_REASON_WAKE_WORD_DETECTED,
} xz_chat_abort_reason_t;

typedef enum {
    XZ_EVENT_VERSION_CHECK_RESULT,
    XZ_EVENT_ACTIVATION_CHECK_RESULT,
    XZ_EVENT_STARTED,
    XZ_EVENT_STOPPED,
    // XZ_EVENT_STATE_CHANGED,
    XZ_EVENT_JSON_RECEIVED,
} xz_chat_event_t;

typedef struct {
    void* buf;
    int len;
    void(*release_cb)(void* user_data);
    void* user_data;
} xz_tx_audio_pck_t;

struct _xz_chat_t;
typedef struct _xz_chat_t xz_chat_t;

typedef struct {
    union {
        struct {                   // 连接服务器时先进行版本检测，以及是否设备已激活
            int version_check_err;
            xz_http_client_response_t* parsed_response;
            void* protocol_config;
        };
        
        int activation_check_err; // 检测设备是否激活

        struct {               // 收得的 json
            char* json;
            int len;
            char* type;
            int type_len;
        };
    };
}xz_chat_event_data_t;


typedef void (*xz_chat_audio_cb_t)(uint8_t *data, int len, xz_chat_t* chat);
typedef void (*xz_chat_event_cb_t)(xz_chat_event_t event, xz_chat_event_data_t *event_data, xz_chat_t* chat);
typedef esp_err_t (*xz_chat_read_audio_cb_t)(xz_tx_audio_pck_t* audio, xz_chat_t* chat);

#define XZ_CHAT_CONFIG_STRUCT struct { \
    struct { \
        char* version_check_url; \
        char* activation_check_url; \
    } ota; \
    char* lang; /*默认语言*/ \
    xz_prot_type_t          prot_pref; /*首选通信协议,websocket或mqtt*/ \
    int send_buf_size; \
    capped_task_config_t        main_task_conf; \
    capped_task_config_t        read_audio_task_conf; \
    bool enable_realtime_listening; /* 如果本地 ACE 的选上该选项 */ \
    xz_chat_audio_cb_t  audio_cb; /*接收到音频数据的回调，用户需要在该回调中播放音频*/ \
    xz_chat_event_cb_t  event_cb;   /*事件回调*/ \
    xz_chat_read_audio_cb_t read_audio_cb; /*读取录音的回调，内部有个线程会通过该函数读取录音并发送*/ \
}

typedef struct {
    XZ_CHAT_CONFIG_STRUCT;
    int cmd_q_size;
    int send_audio_q_size;
} xz_chat_config_t;

#define XZ_CHAT_CONFIG_DEFAULT(read_audio, on_event, on_audio) { \
    .cmd_q_size = 8, \
    .send_audio_q_size = 36, \
    .lang = "en-US", \
    .ota = { \
        .version_check_url= CONFIG_XZ_CHAT_VERSION_CHECK_URL, \
        .activation_check_url = CONFIG_XZ_CHAT_ACTIVATION_CHECK_URL, \
    }, \
    .prot_pref = XZ_PROT_TYPE_MQTT, \
    .read_audio_task_conf = {.stack=4096,.prio=5,.caps=0,.core=tskNO_AFFINITY}, \
    .main_task_conf = {.stack=4096,.prio=4,.caps=0,.core=tskNO_AFFINITY}, \
    .send_buf_size = 256, \
    .enable_realtime_listening = false, \
    .event_cb = on_event, \
    .audio_cb = on_audio, \
    .read_audio_cb = read_audio, \
}

/* creates chat handle and spins main loop */
xz_chat_t* xz_chat_init(xz_chat_config_t* conf);

/*
 below functions only push corresponding job to the main loop, and return immediately.
 if the job is successfully done, events will be fired and handled by event_cb.
*/
void xz_chat_version_check(xz_chat_t* chat_hd, esp_http_client_handle_t client); // either provied your own http_client for reuse, or leave it as NULL
void xz_chat_activation_check(xz_chat_t* chat_hd, esp_http_client_handle_t client); // either provied your own http_client for reuse, or leave it as NULL
void xz_chat_start(xz_chat_t* chat_hd); // starts protocol thread/connection
void xz_chat_stop(xz_chat_t* chat_hd); // stops protocol, it does not stop the main loop.
/* destroy chat and free resources,
   don't call this function in event callback */
esp_err_t xz_chat_destroy(xz_chat_t* chat_hd);

/* start a new session and listen, if device is speaking, interrupt it. */
void xz_chat_new_session(xz_chat_t* chat_hd);
void xz_chat_exit_session(xz_chat_t* chat_hd); // close audio channel

/* combination of enter/abort/exit session based on current state */
void xz_chat_toggle_chat_state(xz_chat_t* chat);

/* below 2 functions are designed for walki-talki style pressing-button-to-speek cases */
void xz_chat_start_manual_listening(xz_chat_t* chat);
void xz_chat_stop_manual_listening(xz_chat_t* chat);

/* only after version_checked */
xz_prot_type_t xz_chat_get_protocol_type(xz_chat_t* chat);

void* xz_chat_get_user_data(xz_chat_t* chat);
void xz_chat_set_user_data(xz_chat_t* chat, void* user_data);

bool xz_chat_is_listening(xz_chat_t* chat);
bool xz_chat_is_speaking(xz_chat_t* chat);
bool xz_chat_is_in_session(xz_chat_t* chat);


void xz_chat_set_audio_cb(xz_chat_t* chat, xz_chat_audio_cb_t cb);
void xz_chat_set_event_cb(xz_chat_t* chat, xz_chat_event_cb_t cb);
void xz_chat_set_read_audio_cb(xz_chat_t* chat, xz_chat_read_audio_cb_t cb);