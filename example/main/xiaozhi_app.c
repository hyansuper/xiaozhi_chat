#include "xz_chat.h"
#include "board.h"
#include "ext_mjson.h"
#include "esp_gmf_oal_sys.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_element.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_io.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_audio_enc.h"
#include "esp_gmf_new_databus.h"
#include "esp_gmf_fifo.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_fourcc.h"
#include "esp_audio_types.h"
#include "esp_afe_config.h"
#include "esp_gmf_afe_manager.h"
#include "esp_gmf_afe.h"
#include "esp_gmf_io_codec_dev.h"
#include "gmf_loader_setup_defaults.h"
#include "esp_audio_simple_player_advance.h"

static const char* const TAG = "xiaozhi_app";

enum audio_player_state_e {
    AUDIO_RUN_STATE_IDLE,
    AUDIO_RUN_STATE_PLAYING,
    AUDIO_RUN_STATE_CLOSED,
};

typedef struct {
    esp_asp_handle_t          player;
    enum audio_player_state_e state;
} audio_prompt_t;

typedef struct {
    esp_gmf_fifo_handle_t         fifo;
    esp_gmf_pipeline_handle_t     pipe;
} recorder_t;

typedef struct {
    esp_gmf_db_handle_t fifo;
    esp_gmf_pipeline_handle_t pipe;
} playback_t;

static esp_gmf_pool_handle_t pool;
static audio_prompt_t prompt;
static recorder_t recorder;
static playback_t playback;
static xz_chat_t* chat;

static esp_err_t audio_prompt_play_to_end(audio_prompt_t* prompt, const char *url);

static void xz_chat_on_event(xz_chat_event_t event, xz_chat_event_data_t *event_data, xz_chat_t* chat) {

    if(event==XZ_EVENT_VERSION_CHECK_RESULT) {
        if(event_data->version_check_err ==0) {
            if(event_data->parsed_response->require_activation) {

                // tell the user they must visit xiaozhi.me and register device with activation code
                ESP_LOGI(TAG, "********************\n\n%s\n\n********************", event_data->parsed_response->activation.message);
                
                // now keep querying device activation status till it's successfully registered
                xz_chat_activation_check(chat, NULL);
            } else {
                xz_chat_start(chat);
            }
        }

    } else if(event==XZ_EVENT_ACTIVATION_CHECK_RESULT) {
        if(event_data->activation_check_err) { 
            // device is not registered yet, check again until user register device.
            xz_chat_activation_check(chat, NULL);
        } else {
            xz_chat_start(chat);
        }

    } else if(event==XZ_EVENT_STARTED) {

    } else if(event==XZ_EVENT_JSON_RECEIVED) {
        if(QESTREQL(event_data->type, "llm")) {
            char* em;
            if((em = emjson_find_string(event_data->json, event_data->len, "$.emotion"))) {
                // change face img/gif accordingly
                if(QESTREQL(em, "happy")) {
                    
                } else if(QESTREQL(em, "sad")) {

                }
            }
        } else if(QESTREQL(event_data->type, "tts")) {
            char* st;
            if((st = emjson_find_string(event_data->json, event_data->len, "$.state"))) {
                if(QESTREQL(st, "start")) {
                    // speaking start
                } else if(QESTREQL(st, "stop")) {
                    // speaking end
                }
            }
        }
    }
}

static void xz_chat_on_audio(uint8_t *data, int len, xz_chat_t* chat) {
    esp_gmf_data_bus_block_t blk = {0};
    int ret = esp_gmf_db_acquire_write(playback.fifo, &blk, len, portMAX_DELAY);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to acquire write to playback FIFO (0x%x)", ret);
        return;
    }
    int bytes_to_copy = (len < blk.buf_length) ? len : blk.buf_length;
    memcpy(blk.buf, data, bytes_to_copy);
    blk.valid_size = bytes_to_copy;
    
    ret = esp_gmf_db_release_write(playback.fifo, &blk, portMAX_DELAY);
}

static void return_blk_to_recorder_fifo(void* blk){
    esp_gmf_fifo_release_read(recorder.fifo, (esp_gmf_data_bus_block_t*)blk, portMAX_DELAY);
    free(blk);
}

static esp_err_t xz_chat_read_audio(xz_tx_audio_pck_t* audio, xz_chat_t* chat) {
    esp_gmf_data_bus_block_t* blk = calloc(1, sizeof(esp_gmf_data_bus_block_t));
    if(blk==NULL) return ESP_ERR_NO_MEM;
    int ret = esp_gmf_fifo_acquire_read(recorder.fifo, blk, 1024/*unused param*/, portMAX_DELAY);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to acquire read from recorder FIFO (0x%x)", ret);
        return ESP_FAIL;
    }
    audio->buf = blk->buf;
    audio->len = blk->valid_size;
    audio->user_data = blk;
    audio->release_cb = return_blk_to_recorder_fifo;
    return ESP_OK;
}

static void afe_event_cb(esp_gmf_obj_handle_t obj, esp_gmf_afe_evt_t *event, void *user_data) {
    switch (event->type) {
        case ESP_GMF_AFE_EVT_WAKEUP_START: {
            esp_gmf_afe_wakeup_info_t *info = event->event_data;
            ESP_LOGI(TAG, "WAKEUP_START [%d : %d]", info->wake_word_index, info->wakenet_model_index); 
            if(!xz_chat_is_in_session(chat)) {
                // the sound must be short
                audio_prompt_play_to_end(&prompt, "file://spiffs/dingding.wav");
                // start listening only AFTER ding sound has finished playing
                xz_chat_new_session(chat);
            }
            break;
        }
        case ESP_GMF_AFE_EVT_WAKEUP_END: 
            ESP_LOGI(TAG, "WAKEUP_END");
            break;
        case ESP_GMF_AFE_EVT_VAD_START: 
            ESP_LOGI(TAG, "VAD_START");
            break;
        case ESP_GMF_AFE_EVT_VAD_END: 
            ESP_LOGI(TAG, "VAD_END");
            break;
        case ESP_GMF_AFE_EVT_VCMD_DECT_TIMEOUT: 
            ESP_LOGI(TAG, "VCMD_DECT_TIMEOUT");
            break;
        default: {
            esp_gmf_afe_vcmd_info_t *info = event->event_data;
            ESP_LOGW(TAG, "Command %d, phrase_id %d, prob %f, str: %s",
                     event->type, info->phrase_id, info->prob, info->str);
            break;
        }
    }
}

static esp_err_t _pipeline_event(esp_gmf_event_pkt_t *event, void *ctx) {
    ESP_LOGI(TAG, "CB: RECV Pipeline EVT: el:%s-%p, type:%d, sub:%s, payload:%p, size:%d,%p",
             OBJ_GET_TAG(event->from), event->from, event->type, esp_gmf_event_get_state_str(event->sub),
             event->payload, event->payload_size, ctx);
    return ESP_OK;
}


static int recorder_outport_acquire_write(void *handle, esp_gmf_data_bus_block_t *blk, int wanted_size, int block_ticks){
    return wanted_size;
}

static int recorder_outport_release_write(void *handle, esp_gmf_data_bus_block_t *blk, int block_ticks){
    esp_gmf_data_bus_block_t _blk = {0};
    int ret = esp_gmf_fifo_acquire_write(recorder.fifo, &_blk, blk->valid_size, block_ticks);
    if (ret < 0) {
        ESP_LOGE(TAG, "%s|%d, Fifo acquire write failed, ret: %d", __func__, __LINE__, ret);
        return ESP_FAIL;
    }
    memcpy(_blk.buf, blk->buf, blk->valid_size);

    _blk.valid_size = blk->valid_size;
    ret = esp_gmf_fifo_release_write(recorder.fifo, &_blk, block_ticks);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Fifo release write failed");
        return ESP_FAIL;
    }
    return blk->valid_size;
}


static void recorder_init_and_run(recorder_t* audio) {
    esp_err_t err = esp_gmf_fifo_create(5 /*size*/, 1 /*unsused param*/, &audio->fifo);
    ESP_ERROR_CHECK(err);

    const char *recorder_elements[] = {"aud_rate_cvt", "ai_afe", "aud_enc"};
    esp_gmf_pool_new_pipeline(pool, "io_codec_dev", recorder_elements, sizeof(recorder_elements)/sizeof(char*), NULL, &audio->pipe);
    assert(audio->pipe);
    // set pipe input
    esp_gmf_io_codec_dev_set_dev(ESP_GMF_PIPELINE_GET_IN_INSTANCE(audio->pipe), board_get_mic_codec_dev());
    // set pipe output
    esp_gmf_port_handle_t out_port = NEW_ESP_GMF_PORT_OUT_BYTE(recorder_outport_acquire_write, recorder_outport_release_write, NULL, NULL, 4096, portMAX_DELAY);
    esp_gmf_pipeline_reg_el_port(audio->pipe, "aud_enc", ESP_GMF_IO_DIR_WRITER, out_port);

    // 因为小智接收到的是24K的采样率，而 es8311 输入和输出要设置相同的采样率, 因此选择 24K。
    // 但 唤醒检测的 ai_afe 接受16K 的采样率，因此要将麦克风的24K转为16K在给 ai_afe，最终发给小智服务器的是 16K
    esp_gmf_info_sound_t info = {
        .sample_rates = 24000,
        .channels = 1,
        .bits = 16,
    };
    esp_gmf_pipeline_report_info(audio->pipe, ESP_GMF_INFO_SOUND, &info, sizeof(info));

    esp_gmf_element_handle_t el;
    esp_gmf_pipeline_get_el_by_name(audio->pipe, "aud_rate_cvt", &el);
    esp_gmf_rate_cvt_set_dest_rate(el, 16000);

    esp_gmf_pipeline_get_el_by_name(audio->pipe, "ai_afe", &el);
    esp_gmf_afe_set_event_cb(el, afe_event_cb, NULL);

    esp_gmf_pipeline_get_el_by_name(audio->pipe, "aud_enc", &el);
    esp_opus_enc_config_t opus_enc_cfg = {
        .sample_rate        = ESP_AUDIO_SAMPLE_RATE_16K,          
        .channel            = ESP_AUDIO_MONO,                    
        .bits_per_sample    = ESP_AUDIO_BIT16,                   
        .bitrate            = 17000, // 小智的实测是取 17000，改大点也行，但发送的数据量变大
        .frame_duration     = ESP_OPUS_ENC_FRAME_DURATION_60_MS, 
        .application_mode   = ESP_OPUS_ENC_APPLICATION_VOIP,     
        .complexity         = 5, // 这个值在小智里如果开启 AEC 则取 0
        .enable_fec         = false,                             
        .enable_dtx         = true,                             
        .enable_vbr         = true,                             
    };
    esp_audio_enc_config_t opus_cfg = {
        .type = ESP_AUDIO_TYPE_OPUS,
        .cfg_sz = sizeof(esp_opus_enc_config_t),
        .cfg = &opus_enc_cfg,
    };
    esp_gmf_audio_enc_reconfig(el, &opus_cfg);


    // create task and run pipe
    esp_gmf_task_cfg_t cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    cfg.name = "recorder_thread";
    cfg.thread.stack = 40*1024;
    cfg.thread.stack_in_ext = true;
    esp_gmf_task_handle_t read_task = NULL;
    esp_gmf_task_init(&cfg, &read_task);

    esp_gmf_pipeline_bind_task(audio->pipe, read_task);
    esp_gmf_pipeline_loading_jobs(audio->pipe);
    esp_gmf_pipeline_set_event(audio->pipe, _pipeline_event, NULL);
    esp_gmf_pipeline_run(audio->pipe);
}

static int playback_inport_acquire_read(void *handle, esp_gmf_data_bus_block_t *blk, int wanted_size, int block_ticks){
    esp_gmf_data_bus_block_t _blk = {0};
    int ret = esp_gmf_db_acquire_read(playback.fifo, &_blk, wanted_size, block_ticks);
    if (ret < 0) {
        ESP_LOGE(TAG, "Fifo acquire read failed (0x%x)", ret);
        return ESP_FAIL;
    }
    if(_blk.valid_size > wanted_size) {
        ESP_LOGE(TAG, "acceptable size less than fifo block");
        esp_gmf_db_release_read(playback.fifo, &_blk, block_ticks);
        return ESP_FAIL;
    }
    memcpy(blk->buf, _blk.buf, _blk.valid_size);
    blk->valid_size = _blk.valid_size;
    esp_gmf_db_release_read(playback.fifo, &_blk, block_ticks);
    return ESP_GMF_ERR_OK;
}

static int playback_inport_release_read(void *handle, esp_gmf_data_bus_block_t *blk, int block_ticks){
    return ESP_GMF_ERR_OK;
}

static void playback_init_and_run(playback_t* audio) {
    esp_err_t err = esp_gmf_db_new_fifo(5 /*size*/, 1 /*unsused param*/, &audio->fifo);
    ESP_ERROR_CHECK(err);

    const char *name[] = { "aud_dec", /*"aud_bit_cvt",*/ "aud_rate_cvt", /*"aud_ch_cvt"*/};
    esp_gmf_pool_new_pipeline(pool, NULL, name, sizeof(name)/sizeof(char*), "io_codec_dev", &audio->pipe);
    assert(audio->pipe);
    // set pipe output
    esp_gmf_io_codec_dev_set_dev(ESP_GMF_PIPELINE_GET_OUT_INSTANCE(audio->pipe), board_get_spk_codec_dev());
    // set pipe input
    esp_gmf_port_handle_t in_port = NEW_ESP_GMF_PORT_IN_BYTE(playback_inport_acquire_read, playback_inport_release_read, NULL, NULL, 4096, portMAX_DELAY);
    esp_gmf_pipeline_reg_el_port(audio->pipe, "aud_dec", ESP_GMF_IO_DIR_READER, in_port);

    esp_gmf_element_handle_t el;
    esp_gmf_pipeline_get_el_by_name(audio->pipe, "aud_rate_cvt", &el);
    esp_gmf_rate_cvt_set_dest_rate(el, 24000);

    esp_gmf_pipeline_get_el_by_name(audio->pipe, "aud_dec", &el);
    esp_opus_dec_cfg_t opus_dec_cfg = {
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .channel = 1,
        .sample_rate = 24000,
    };
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_TYPE_OPUS,
        .dec_cfg = &opus_dec_cfg,
        .cfg_size = sizeof(esp_opus_dec_cfg_t),
    };
    esp_gmf_audio_dec_reconfig(el, &dec_cfg);

    esp_gmf_info_sound_t info = {
        .sample_rates = 24000,
        .channels = 1,
        .bits = 16,
        .format_id = ESP_FOURCC_OPUS,
    };
    esp_gmf_pipeline_report_info(audio->pipe, ESP_GMF_INFO_SOUND, &info, sizeof(info));

    // create task and run pipe
    esp_gmf_task_cfg_t cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    cfg.name="playback_thread";
    cfg.thread.stack = 40*1024;
    cfg.thread.stack_in_ext = true;
    esp_gmf_task_handle_t task = NULL;
    esp_gmf_task_init(&cfg, &task);

    esp_gmf_pipeline_bind_task(audio->pipe, task);
    esp_gmf_pipeline_loading_jobs(audio->pipe);
    esp_gmf_pipeline_set_event(audio->pipe, _pipeline_event, NULL);
    esp_gmf_pipeline_run(audio->pipe);
}

static int prompt_out_data_callback(uint8_t *data, int data_size, void *ctx)
{
    esp_codec_dev_write(board_get_spk_codec_dev(), data, data_size);
    return 0;
}

static int prompt_event_callback(esp_asp_event_pkt_t *event, void *ctx)
{
    if (event->type == ESP_ASP_EVENT_TYPE_STATE) {
        esp_asp_state_t st = 0;
        memcpy(&st, event->payload, event->payload_size);
        if (((st == ESP_ASP_STATE_STOPPED) || (st == ESP_ASP_STATE_FINISHED))) {
            audio_prompt_t* prompt = (audio_prompt_t*)ctx;
            prompt->state = AUDIO_RUN_STATE_IDLE;

        } else if(st == ESP_ASP_STATE_ERROR) {
            ESP_LOGE(TAG, "get err state");
        }
    }
    return 0;
}

static esp_err_t audio_prompt_open(audio_prompt_t* prompt) {
    
    esp_asp_cfg_t cfg = {
        .in.cb = NULL,
        .in.user_ctx = NULL,
        .out.cb = prompt_out_data_callback,
        .out.user_ctx = NULL,
        .task_prio = 5,
    };
    
    esp_err_t err = esp_audio_simple_player_new(&cfg, &prompt->player);
    if (err != ESP_OK || !prompt->player) {
        ESP_LOGE(TAG, "Failed to create audio prompt player");
        return ESP_FAIL;
    }
    
    err = esp_audio_simple_player_set_event(prompt->player, prompt_event_callback, &prompt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set prompt event callback");
        esp_audio_simple_player_destroy(prompt->player);
        prompt->player = NULL;
        return ESP_FAIL;
    }
    
    prompt->state = AUDIO_RUN_STATE_IDLE;
    ESP_LOGI(TAG, "Audio prompt opened successfully");
    return ESP_OK;
}

static esp_err_t audio_prompt_close(audio_prompt_t* prompt)
{
    if (prompt->state == AUDIO_RUN_STATE_CLOSED) {
        ESP_LOGW(TAG, "Audio prompt is already closed");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Closing audio prompt->..");
    
    if (prompt->state == AUDIO_RUN_STATE_PLAYING) {
        esp_audio_simple_player_stop(prompt->player);
    }
    
    if (prompt->player) {
        esp_err_t err = esp_audio_simple_player_destroy(prompt->player);
        ESP_GMF_RET_ON_NOT_OK(TAG, err, { return ESP_FAIL; }, "Failed to destroy audio prompt player");
        prompt->player = NULL;
    }
    
    prompt->state = AUDIO_RUN_STATE_CLOSED;
    ESP_LOGI(TAG, "Audio prompt closed successfully");
    return ESP_OK;
}

static esp_err_t audio_prompt_stop(audio_prompt_t* prompt)
{
    if (prompt->state == AUDIO_RUN_STATE_CLOSED) {
        ESP_LOGE(TAG, "Cannot stop prompt - not opened");
        return ESP_FAIL;
    }
    
    if (prompt->state == AUDIO_RUN_STATE_IDLE) {
        ESP_LOGW(TAG, "Audio prompt is already idle");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Stopping audio prompt->..");
    esp_err_t err = esp_audio_simple_player_stop(prompt->player);
    ESP_GMF_RET_ON_NOT_OK(TAG, err, { return ESP_FAIL; }, "Failed to stop prompt playback");
    
    prompt->state = AUDIO_RUN_STATE_IDLE;
    ESP_LOGI(TAG, "Audio prompt stopped successfully");
    return ESP_OK;
}

static esp_err_t audio_prompt_play_to_end(audio_prompt_t* prompt, const char *url) {
    if (!url) {
        ESP_LOGE(TAG, "Invalid URL for prompt playback");
        return ESP_FAIL;
    }
    
    if (prompt->state == AUDIO_RUN_STATE_CLOSED) {
        ESP_LOGE(TAG, "Cannot play prompt - not opened");
        return ESP_FAIL;
    }
    
    if (prompt->state == AUDIO_RUN_STATE_PLAYING) {
        ESP_LOGW(TAG, "Audio prompt is already playing, stopping current playback");
        esp_audio_simple_player_stop(prompt->player);
    }

    ESP_LOGI(TAG, "Starting prompt playback: %s", url);
    prompt->state = AUDIO_RUN_STATE_PLAYING;
    esp_err_t err = esp_audio_simple_player_run_to_end(prompt->player, url, NULL);
    ESP_GMF_RET_ON_NOT_OK(TAG, err, { return ESP_FAIL; }, "Failed to start prompt playback");
    return ESP_OK;
}

esp_err_t gmf_pool_init() {
	esp_gmf_pool_init(&pool);
	if(pool) {
		return gmf_loader_setup_all_defaults(pool);
	} else {
		return ESP_FAIL;
	}
}

void xiaozhi_app_start() {

	ESP_ERROR_CHECK(gmf_pool_init());

	// load board info(mac, uuid, etc...) from nvs partition,
	// nvs partition must be initialized before this call
	xz_board_info_load();

	// init xiaozhi chat handle
    xz_chat_config_t chat_conf = XZ_CHAT_CONFIG_DEFAULT(xz_chat_read_audio, xz_chat_on_event, xz_chat_on_audio);
    
    // chat_conf.prot_pref = XZ_PROT_TYPE_WS; // prefer websocket over mqtt
    chat = xz_chat_init(&chat_conf);
    assert(chat);  


    // setup recorder pipe and run
    recorder_init_and_run(&recorder);
    playback_init_and_run(&playback);
    audio_prompt_open(&prompt);
 

    // run xiaozhi chat, the first step is call version_check, subsequent steps are done in xz_chat_on_event callback
    xz_chat_version_check(chat, NULL);
}