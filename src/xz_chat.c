#include "xz_protocol_priv.h"
#include "xz_util.h"
#include "xz_chat_priv.h"
#include "xz_board_info.h"
#include "esp_check.h"
#include "ext_mjson.h"
#include "task_util.h"


static const char* const TAG = "xz_chat";

void* xz_chat_get_user_data(xz_chat_t* chat) {
    if(chat) return chat->user_data;
    return NULL;
}
void xz_chat_set_user_data(xz_chat_t* chat, void* user_data) {
    if(chat)  chat->user_data = user_data;
}

static esp_err_t xz_prot_send_msg(xz_chat_t* chat, const char* fmt, ...) {
    va_list args;
    va_start (args, fmt);
    int n = vsnprintf (chat->send_buf, chat->send_buf_size, fmt, args);
    va_end(args);
    if (n >= chat->send_buf_size) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "%.*s", n, chat->send_buf);
    return chat->prot_if.send_msg(chat, chat->send_buf, n);
}

static inline esp_err_t xz_prot_send_abort_speaking(xz_chat_t* chat, xz_chat_abort_reason_t reason) {
    if(chat->session_buf == NULL) return ESP_ERR_INVALID_STATE;
    return xz_prot_send_msg(chat,
        "{\"session_id\":\"%s\",\"type\":\"abort\"%s}",
        chat->session_id,
        reason==XZ_ABORT_REASON_WAKE_WORD_DETECTED? ",\"reason\":\"wake_word_detected\"": "");
}

static inline esp_err_t xz_prot_send_wake_word_detected(xz_chat_t* chat, const char* wake_word) {
    if(chat->session_buf == NULL) return ESP_ERR_INVALID_STATE;
    return xz_prot_send_msg(chat,
        "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"%s\"}",
        chat->session_id, wake_word);
}

static inline esp_err_t xz_prot_send_start_listening(xz_chat_t* chat, xz_chat_listening_mode_t mode) {
    if(chat->session_buf == NULL) return ESP_ERR_INVALID_STATE;
    char* mode_str;
    switch(mode) {
    case XZ_LISTENING_MODE_MANUAL_STOP:
        mode_str = "manual"; break;
    case XZ_LISTENING_MODE_REALTIME:
        mode_str = "realtime"; break;
    default:
        mode_str = "auto";
    }
    return xz_prot_send_msg(chat,
        "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"%s\"}",
        chat->session_id, mode_str);
}

static inline esp_err_t xz_prot_send_stop_listening(xz_chat_t* chat) {
    if(chat->session_buf == NULL) return ESP_ERR_INVALID_STATE;
    return xz_prot_send_msg(chat,
        "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
        chat->session_id);
}

xz_prot_type_t xz_chat_get_protocol_type(xz_chat_t* chat) {
    return chat->prot_type;
}


static void* gen_prot_conf_from_http_resp(xz_http_client_response_t* resp) {
    void* conf;
    switch(resp->prot_type) {
    case XZ_PROT_TYPE_WS:
        if((conf=calloc(1, sizeof(xz_ws_prot_config_t)))) {
            xz_ws_prot_config_set_default((xz_ws_prot_config_t*)conf);
            xz_ws_prot_config_fill_rest_from_response((xz_ws_prot_config_t*)conf, &resp->ws);
        }
        break;
    case XZ_PROT_TYPE_MQTT:
        if((conf=calloc(1, sizeof(xz_mqtt_prot_config_t)))) {
            xz_mqtt_prot_config_set_default((xz_mqtt_prot_config_t*)conf);
            xz_mqtt_prot_config_fill_rest_from_response((xz_mqtt_prot_config_t*)conf, &resp->mqtt);
        }
        break;
    default: conf = NULL;
    }
    return conf;
}

static inline void dispatch_event(xz_chat_t* chat, xz_chat_event_t eid) {
    if(chat->event_cb)
        chat->event_cb(eid, &chat->event_data, chat);
}

static void main_task_loop(void* arg) {
    xz_chat_t* chat = (xz_chat_t*) arg;
    chat_set_flag(chat, XZ_FLAG_INIT);
    while (1) {
        cmd_q_el_t cmd;
        xQueueReceive( chat->cmd_q, &cmd, portMAX_DELAY );
        esp_err_t ret= cmd.fn(cmd.a, cmd.b, cmd.c);
        if(ret) {
            ESP_LOGW(TAG, "cmd err=%d", ret);
        }
    }
    atomic_store(&chat->flags, 0);
    chat->main_task = NULL;
    capped_task_delete(NULL);
}

xz_chat_t* xz_chat_init(xz_chat_config_t* conf) {
    xz_board_info_init();

    esp_err_t ret = ESP_OK;
    xz_chat_t* chat = NULL;
    ESP_GOTO_ON_FALSE(conf->read_audio_cb, ESP_ERR_INVALID_ARG, err, TAG, "xz_chat_config_t.read_audio_cb must be set");
    
    ESP_GOTO_ON_FALSE((chat=calloc(1, sizeof(xz_chat_t))), ESP_ERR_NO_MEM, err, TAG, "calloc chat handle");
    memcpy(chat, conf, sizeof(xz_chat_config_t));

    ESP_GOTO_ON_FALSE((chat->send_buf=malloc(chat->send_buf_size)), ESP_ERR_NO_MEM, err, TAG, "malloc send buf");
    ESP_GOTO_ON_FALSE((chat->eg=xEventGroupCreate()), ESP_ERR_NO_MEM, err, TAG, "create event group");

    ESP_GOTO_ON_FALSE((chat->cmd_q=xQueueCreate(conf->cmd_q_size, sizeof(cmd_q_el_t))), ESP_ERR_NO_MEM, err, TAG, "create cmd q");
    ESP_GOTO_ON_ERROR(capped_task_create(&chat->main_task, "xz_main_task", main_task_loop, chat, &conf->main_task_conf), err, TAG, "create main task");
    
err:
    if(ret) {
        xz_chat_destroy(chat);
        chat = NULL;
    }
    return chat;
}


static esp_err_t _version_check(xz_chat_t* chat, esp_http_client_handle_t client) {
    if(chat_has_any_flag(chat, XZ_FLAG_VER_CHECKED)) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    esp_http_client_handle_t http = client;
    if(!http) {
        ESP_GOTO_ON_FALSE((http=http_client_util_create()), ESP_ERR_NO_MEM, err, TAG, "create http client");
        ESP_GOTO_ON_ERROR(xz_http_client_set_headers(http, chat->lang), err, TAG, "set header");
    }
    ESP_GOTO_ON_FALSE((chat->version_check_response=malloc(2048 + sizeof(xz_http_client_response_t))), ESP_ERR_NO_MEM, err, TAG, "malloc resp");
    ESP_GOTO_ON_ERROR(xz_http_client_version_check(http, chat->ota.version_check_url, chat->prot_pref, chat->version_check_response->buf, 2048, chat->version_check_response), err, TAG, "check version");
    chat->prot_type = chat->version_check_response->prot_type;
    ESP_GOTO_ON_FALSE((chat->prot_conf=gen_prot_conf_from_http_resp(chat->version_check_response)), ESP_ERR_NO_MEM, err, TAG, "gen prot conf");
err:
    if(!client && http) http_client_util_delete(http);
    if(!ret) {
        chat_set_flag(chat, XZ_FLAG_VER_CHECKED);
        if(!chat->version_check_response->require_activation) {
            chat_set_flag(chat, XZ_FLAG_ACT_CHECKED);
        }
    }
    chat->event_data.version_check_err = ret;
    chat->event_data.parsed_response = ret? NULL: chat->version_check_response;
    chat->event_data.protocol_config = ret? NULL: chat->prot_conf;
    dispatch_event(chat, XZ_EVENT_VERSION_CHECK_RESULT);
    if(ret) {
        RELEASE(chat->version_check_response);
        RELEASE(chat->prot_conf);
    }
    return ret;
}
void xz_chat_version_check(xz_chat_t* chat, esp_http_client_handle_t client) {
    CMD(chat, _version_check, chat, client);
}

static esp_err_t _activation_check(xz_chat_t* chat, esp_http_client_handle_t client) {
    if(chat_has_any_flag(chat, XZ_FLAG_ACT_CHECKED)) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    esp_http_client_handle_t http = client;
    if(!http) {
        ESP_GOTO_ON_FALSE((http=http_client_util_create()), ESP_ERR_NO_MEM, err, TAG, "create http client");
        ESP_GOTO_ON_ERROR(xz_http_client_set_headers(http, chat->lang), err, TAG, "set header");
    }
    ESP_GOTO_ON_ERROR(xz_http_client_activation_check(http, chat->ota.activation_check_url), err, TAG, "check activation");
err:
    if(!client && http) http_client_util_delete(http);
    chat->event_data.activation_check_err = ret;
    dispatch_event(chat, XZ_EVENT_ACTIVATION_CHECK_RESULT);
    if(!ret) {
        chat_set_flag(chat, XZ_FLAG_ACT_CHECKED);
    }
    return ret;
}

void xz_chat_activation_check(xz_chat_t* chat, esp_http_client_handle_t client) {
    CMD(chat, _activation_check, chat, client);
}

static void read_audio_loop(void* arg) {
    xz_chat_t* chat = (xz_chat_t*)arg;
 
#ifdef CONFIG_XZ_CONTINUOUSLY_DIGEST_AUDIO_INPUT
    while(0== (TASK_STOP_BIT & ulTaskNotifyTake(pdTRUE, 0))) {
        xz_tx_audio_pck_t audio;
        if(chat->read_audio_cb(&audio, chat))
            continue;
        if(chat_has_any_flag(chat, XZ_FLAG_SESS_LISTENING)) {
            chat->prot_if.send_data(chat, audio.buf, audio.len);
        } 
        if(audio.release_cb) {
            audio.release_cb(audio.user_data);
        }
    }
#else
    while(0== (TASK_STOP_BIT & ulTaskNotifyTake(pdTRUE, portMAX_DELAY))) {
        while(xEventGroupGetBits(chat->eg) & XZ_EG_READ_AUDIO_TASK_RUN_BIT) {
            xz_tx_audio_pck_t audio;
            if(chat->read_audio_cb(&audio, chat))
                continue;
            if(chat_has_any_flag(chat, XZ_FLAG_SESS_LISTENING)) {
                chat->prot_if.send_data(chat, audio.buf, audio.len);
            }
            if(audio.release_cb) {
                audio.release_cb(audio.user_data);
            }
        }
    }
#endif
    chat->read_audio_task = NULL;
    xEventGroupSetBits(chat->eg, XZ_EG_READ_AUDIO_TASK_STOPPED_BIT);
    capped_task_delete(NULL);
}

static esp_err_t _start(xz_chat_t* chat) {
    if(!chat_has_any_flag(chat, XZ_FLAG_ACT_CHECKED) || chat_has_any_flag(chat, XZ_FLAG_STARTED)) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    if(chat->prot_ctx == NULL) {
        switch(chat->prot_type) {
        case XZ_PROT_TYPE_WS:
            ret = xz_ws_prot_init((xz_ws_prot_ctx_t**)&chat->prot_ctx, (xz_ws_prot_config_t*)chat->prot_conf, chat);
            chat->prot_if = xz_ws_prot_if;
            break;
        case XZ_PROT_TYPE_MQTT:
            ret = xz_mqtt_prot_init((xz_mqtt_prot_ctx_t**)&chat->prot_ctx, (xz_mqtt_prot_config_t*)chat->prot_conf, chat);
            chat->prot_if = xz_mqtt_prot_if;
            break;
        default: ret = ESP_ERR_INVALID_ARG;
        }
        if(ret) return ret;
        RELEASE(chat->prot_conf); // when protocol context is already initialized, it's configuration is nolonger needed.
        RELEASE(chat->version_check_response);
    }
    ESP_RETURN_ON_ERROR(chat->prot_if.start(chat), TAG, "start prot"); // if start fails, we dont need to deinit chat->prot_ctx
    ESP_GOTO_ON_ERROR(capped_task_create(&chat->read_audio_task, "xz_read_audio_task", read_audio_loop, chat, &chat->read_audio_task_conf), err, TAG, "create read audio task");
    ESP_LOGI(TAG, "started");
err:
    if(ret) {
        chat->prot_if.stop(chat);
    } else { 
        chat_set_flag(chat, XZ_FLAG_STARTED);
        dispatch_event(chat, XZ_EVENT_STARTED);
    }
    return ret;
}
void xz_chat_start(xz_chat_t* chat) {
    CMD(chat, _start, chat);
}

static esp_err_t _exit_session(xz_chat_t* chat) {
    if(!chat_has_any_flag(chat, XZ_FLAGS_IN_SESS)) return ESP_ERR_INVALID_STATE;
#ifndef CONFIG_CONTINUOUSLY_DIGEST_AUDIO_INPUT
    xEventGroupClearBits(chat->eg, XZ_EG_READ_AUDIO_TASK_RUN_BIT);
#endif
    chat_clear_flag(chat, XZ_FLAGS_IN_SESS);
    chat->prot_if.close_audio_chan(chat);
    return ESP_OK;
}

static esp_err_t _stop(xz_chat_t* chat) {
    esp_err_t ret;
    _exit_session(chat);
    if(!chat_has_any_flag(chat, XZ_FLAG_STARTED)) return ESP_ERR_INVALID_STATE;
    if((ret=chat->prot_if.stop(chat))) return ret;
    if((ret=term_task_wait(chat->read_audio_task, chat->eg, XZ_EG_READ_AUDIO_TASK_STOPPED_BIT, pdMS_TO_TICKS(5000)))) return ret;
    chat_clear_flag(chat, XZ_FLAG_STARTED);
    dispatch_event(chat, XZ_EVENT_STOPPED);
    return ret;
}
void xz_chat_stop(xz_chat_t* chat) {
    CMD(chat, _stop, chat);
}

static esp_err_t __start_listening(xz_chat_t* chat, xz_chat_listening_mode_t listening_mode) {
    chat->listening_mode = listening_mode;
    esp_err_t ret = xz_prot_send_start_listening(chat, listening_mode);
    if(ret) return ret;
    chat_clear_flag(chat, XZ_FLAG_SESS_LEAVING|XZ_FLAG_SESS_SPEAKING);
    chat_set_flag(chat, XZ_FLAG_SESS_LISTENING);
#ifndef CONFIG_CONTINUOUSLY_DIGEST_AUDIO_INPUT
    xEventGroupSetBits(chat->eg, XZ_EG_READ_AUDIO_TASK_RUN_BIT);
    resume_task(chat->read_audio_task);
#endif
    return ESP_OK;
}

static esp_err_t __enter_session_then_listen(xz_chat_t* chat, xz_chat_listening_mode_t listening_mode) {
    esp_err_t ret;
    if((ret=chat->prot_if.open_audio_chan(chat))) {
        ESP_LOGE(TAG, "open audio chan");
        return ret;
    }
    if((ret=__start_listening(chat, listening_mode))) {
        chat->prot_if.close_audio_chan(chat);
        return ret;
    }
    return ESP_OK;
}

void xz_chat_exit_session(xz_chat_t* chat) {
    CMD(chat, _exit_session, chat);
}

static esp_err_t __abort_speaking_then_listen(xz_chat_t* chat, xz_chat_listening_mode_t listening_mode) {
    chat_clear_flag(chat, XZ_FLAG_SESS_SPEAKING);
    esp_err_t ret = xz_prot_send_abort_speaking(chat, XZ_ABORT_REASON_NONE);
    if(ret) return ret;
    vTaskDelay(pdMS_TO_TICKS(250)); // wait till remaining data played.
    return __start_listening(chat, listening_mode);
}

static esp_err_t _new_session(xz_chat_t* chat) {
    int flags = atomic_load(&chat->flags);
    if((flags & XZ_FLAG_STARTED) == 0) return ESP_ERR_INVALID_STATE;
    xz_chat_listening_mode_t listening_mode = chat->enable_realtime_listening? XZ_LISTENING_MODE_REALTIME: XZ_LISTENING_MODE_AUTO_STOP;
    if(0 == (flags & XZ_FLAGS_IN_SESS))
        return __enter_session_then_listen(chat, listening_mode);
    if(flags & XZ_FLAG_SESS_SPEAKING)
        return __abort_speaking_then_listen(chat, listening_mode);
    if(flags & XZ_FLAG_SESS_LEAVING)
        return __start_listening(chat, listening_mode);
    return ESP_ERR_INVALID_STATE;
}
void xz_chat_new_session(xz_chat_t* chat) {
    CMD(chat, _new_session, chat);
}

static esp_err_t _toggle_chat_state(xz_chat_t* chat) {
    int flags = atomic_load(&chat->flags);
    if((flags & XZ_FLAG_STARTED) == 0) return ESP_ERR_INVALID_STATE;
    xz_chat_listening_mode_t listening_mode = chat->enable_realtime_listening? XZ_LISTENING_MODE_REALTIME: XZ_LISTENING_MODE_AUTO_STOP;
    if(0 == (flags & XZ_FLAGS_IN_SESS))
        return __enter_session_then_listen(chat, listening_mode);
    if(flags & XZ_FLAG_SESS_SPEAKING)
        return __abort_speaking_then_listen(chat, listening_mode);
    if(flags & XZ_FLAG_SESS_LEAVING)
        return __start_listening(chat, listening_mode);
    if(flags & XZ_FLAG_SESS_LISTENING)
        return _exit_session(chat);
    return ESP_ERR_INVALID_STATE;
}
void xz_chat_toggle_chat_state(xz_chat_t* chat) {
    CMD(chat, _toggle_chat_state, chat);
}

esp_err_t xz_chat_destroy(xz_chat_t* chat) {
    if(!chat) return ESP_ERR_INVALID_ARG;
    if(xTaskGetCurrentTaskHandle() == chat->main_task) {
        ESP_LOGE(TAG, "Cannot call xz_chat_destroy in its own event callback.");
        return ESP_FAIL;
    }
    RELEASE_TASK(chat->main_task);

    esp_err_t ret0= term_task_wait(chat->read_audio_task, chat->eg, XZ_EG_READ_AUDIO_TASK_STOPPED_BIT, pdMS_TO_TICKS(5000));
    RELEASE_TASK(chat->read_audio_task);

    esp_err_t ret1;
    switch(chat->prot_type) {
        case XZ_PROT_TYPE_WS: ret1 = xz_ws_prot_destroy((xz_ws_prot_ctx_t*) chat->prot_ctx); break;
        case XZ_PROT_TYPE_MQTT: ret1 = xz_mqtt_prot_destroy((xz_mqtt_prot_ctx_t*) chat->prot_ctx); break;
        default: ret1= ESP_ERR_INVALID_ARG;
    }
    if(!ret1) chat->prot_ctx = NULL;

    if(chat->eg) {vEventGroupDelete(chat->eg); chat->eg = NULL;}

    if(chat->cmd_q) { vQueueDelete(chat->cmd_q);chat->cmd_q = NULL; }

    RELEASE(chat->version_check_response);
    RELEASE(chat->prot_conf);
    RELEASE(chat->send_buf);
    RELEASE(chat->session_buf);
    
    esp_err_t ret = ret0 || ret1;
    if(ret) {
        chat_set_flag(chat, XZ_FLAG_ERR);
    } else {
        RELEASE(chat);
    }
    return ret;
}

static esp_err_t _start_tts(xz_chat_t* chat) {
    if(!chat_has_any_flag(chat, XZ_FLAGS_IN_SESS)) return ESP_ERR_INVALID_STATE;
    if(chat->listening_mode==XZ_LISTENING_MODE_AUTO_STOP) {
        chat_clear_flag(chat, XZ_FLAG_SESS_LISTENING);
    }
    chat_set_flag(chat, XZ_FLAG_SESS_SPEAKING);
#ifndef CONFIG_CONTINUOUSLY_DIGEST_AUDIO_INPUT
    if(!chat->enable_realtime_listening) {
        xEventGroupClearBits(chat->eg, XZ_EG_READ_AUDIO_TASK_RUN_BIT);
    }
#endif
    return ESP_OK;
}

static esp_err_t _stop_tts(xz_chat_t* chat) {
    if(!chat_has_any_flag(chat, XZ_FLAG_SESS_SPEAKING)) return ESP_ERR_INVALID_STATE;

    // wait for the speaker to empty its buffer
    vTaskDelay(pdMS_TO_TICKS(500));
    chat_clear_flag(chat, XZ_FLAG_SESS_SPEAKING);
    if(chat->listening_mode == XZ_LISTENING_MODE_MANUAL_STOP) {
        return ESP_OK;
    }
    esp_err_t ret = __start_listening(chat, chat->enable_realtime_listening? XZ_LISTENING_MODE_REALTIME: XZ_LISTENING_MODE_AUTO_STOP);
    if(ret) {
        // if server send tts.stop and close connection, then sending start listening msg will fail.
        chat_set_flag(chat, XZ_FLAG_SESS_LEAVING);
        _exit_session(chat);
        return ret;
    }
    return ESP_OK;
}

static esp_err_t _process_mcp(xz_chat_t* chat, char* buf, int len) {
    // todo: process the string in buf and invoke responding mcp function
    free(buf);
    return ESP_OK;
}

void xz_prot_process_json(xz_chat_t* chat, char*  json,  int len,  char*  type,  int tlen) {
    const char* s; int slen;
    if(QESTREQL(type, "tts")) {
        if((s = emjson_find_string(json, len, "$.state"))) {
            if(QESTREQL(s, "start")) {
                CMD(chat, _start_tts, chat);
            } else if(QESTREQL(s, "stop")) {
                CMD(chat, _stop_tts, chat);
            }
        }
    } else if(QESTREQL(type, "mcp")) {
        if(mjson_find(json, len, "$.payload", &s, &slen)==MJSON_TOK_OBJECT) {
            CMD(chat, _process_mcp, chat, strndup(s, slen), (void*)slen);
        }
    } /*else if(QESTREQL(type, "system")) {
        if(emjson_locate_string(json, len, "$.command", &s, &slen)) {
            ESP_LOGW(TAG, "recv system command: %.*s", slen, s);
            // if(QESTREQL(s, "reboot")) {
            //     esp_restart();
            // }
        }
    }*/

    chat->event_data.json = json;
    chat->event_data.len = len;
    chat->event_data.type = type;
    chat->event_data.type_len = tlen;
    dispatch_event(chat, XZ_EVENT_JSON_RECEIVED);
}

static esp_err_t _start_manual_listening(xz_chat_t* chat) {
    int flags = atomic_load(&chat->flags);
    if((flags & XZ_FLAG_STARTED) == 0) return ESP_ERR_INVALID_STATE;
    if((flags & XZ_FLAGS_IN_SESS) == 0)
        return __enter_session_then_listen(chat, XZ_LISTENING_MODE_MANUAL_STOP);
    if(flags & XZ_FLAG_SESS_SPEAKING)
        return __abort_speaking_then_listen(chat, XZ_LISTENING_MODE_MANUAL_STOP);
    if(flags & XZ_FLAG_SESS_LEAVING) 
        return __start_listening(chat, XZ_LISTENING_MODE_MANUAL_STOP);
    return ESP_ERR_INVALID_STATE;
}
void xz_chat_start_manual_listening(xz_chat_t* chat) {
    CMD(chat, _start_manual_listening, chat);
}

static esp_err_t _stop_manual_listening(xz_chat_t* chat) {
    if(chat_has_any_flag(chat, XZ_FLAG_SESS_LISTENING)) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = xz_prot_send_stop_listening(chat);
    if(!ret) {
        chat_set_flag(chat, XZ_FLAG_SESS_LEAVING);
        chat_clear_flag(chat, XZ_FLAG_SESS_LISTENING);
    }
    return ret;
}

void xz_chat_stop_manual_listening(xz_chat_t* chat) {
    CMD(chat, _stop_manual_listening, chat);
}

