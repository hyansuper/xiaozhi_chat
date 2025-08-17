#pragma once
#include "xz_chat.h"
#include <stdatomic.h>

#define XZ_EG_SERVER_HELLO_BIT (1<<0)
#define XZ_EG_PROT_CONN_BIT (1<<1)
#define XZ_EG_PROT_DISCONN_BIT (1<<2)
#define XZ_EG_PROT_ERR_BIT (1<<3)
#define XZ_EG_UDP_TASK_STOPPED_BIT (1<<4)
// #define XZ_EG_UDP_TASK_PAUSED_BIT (1<<5)
// #define XZ_EG_UDP_TASK_RESUMED_BIT (1<<6)
#define XZ_EG_READ_AUDIO_TASK_STOPPED_BIT (1<<8)
#define XZ_EG_READ_AUDIO_TASK_RUN_BIT (1<<9)

typedef enum {
    XZ_LISTENING_MODE_AUTO_STOP,
    XZ_LISTENING_MODE_MANUAL_STOP,
    XZ_LISTENING_MODE_REALTIME,
} xz_chat_listening_mode_t;

struct _xz_chat_t {
    XZ_CHAT_CONFIG_STRUCT; // this must be the first memeber in struct.

    xz_http_client_response_t* version_check_response;
    void* prot_conf;
    xz_prot_type_t prot_type;

    _Atomic int flags;
    xz_chat_listening_mode_t listening_mode;
    
    EventGroupHandle_t eg;
    TaskHandle_t main_task;
    TaskHandle_t read_audio_task;

    xz_prot_if_t prot_if;
    void* prot_ctx;

    QueueHandle_t cmd_q;
    char* send_buf;
    xz_chat_event_data_t event_data;

    char* session_buf;
    char* session_id;
    int server_sample_rate;
    int server_frame_duration;

    void* user_data;
};


#define XZ_FLAG_ERR 1
#define XZ_FLAG_INIT (1<<1)
#define XZ_FLAG_VER_CHECKED (1<<2)
#define XZ_FLAG_ACT_CHECKED (1<<3)
#define XZ_FLAG_STARTED (1<<4)

#define XZ_FLAG_SESS_LEAVING (1<<8)
#define XZ_FLAG_SESS_LISTENING (1<<9)
#define XZ_FLAG_SESS_SPEAKING (1<<10)

#define XZ_FLAGS_IN_SESS (XZ_FLAG_SESS_LEAVING|XZ_FLAG_SESS_LISTENING|XZ_FLAG_SESS_SPEAKING)

static inline void chat_set_flag(xz_chat_t* chat, int bit) {
    atomic_fetch_or(&chat->flags, bit);
}
static inline void chat_clear_flag(xz_chat_t* chat, int bit) {
    atomic_fetch_and(&chat->flags, ~bit);
}
static inline bool chat_has_any_flag(xz_chat_t* chat, int bit) {
    return (atomic_load(&chat->flags) & bit)!= 0;
}
static inline bool chat_has_every_flag(xz_chat_t* chat, int bit) {
    return (atomic_load(&chat->flags) & bit) == bit;
}

typedef esp_err_t (*_cmd_el_fn_t)(void* a,void* b,void* c);
typedef struct {
    _cmd_el_fn_t fn;
    void* a;
    void* b;
    void* c;
} cmd_q_el_t;



#define CMD(chat, fn, ...) do{cmd_q_el_t el={(_cmd_el_fn_t)fn, __VA_ARGS__ };xQueueSend(chat->cmd_q, &el, portMAX_DELAY);}while(0)