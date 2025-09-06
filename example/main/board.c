#include "board.h"

#include <esp_err.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <driver/i2s_common.h>
#include <driver/gpio.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include "esp_check.h"

static const char* const TAG = "board";

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240
#define AUDIO_CODEC_DEFAULT_MIC_GAIN 30.0
#define AUDIO_CODEC_DEFAULT_SAMPLE_RATE 24000

#define GPIO_MCLK 38
#define GPIO_WS 41
#define GPIO_BCLK 39
#define GPIO_DIN  40
#define GPIO_DOUT 42

#define GPIO_AMP_PA       18
#define GPIO_AMP_PA_INVERTED  0
#define GPIO_I2C_SDA  48
#define GPIO_I2C_SCL  47
#define I2C_ADDR  ES8311_CODEC_DEFAULT_ADDR

static int sample_rate;
static i2s_chan_handle_t tx_handle;
static i2s_chan_handle_t rx_handle;
static audio_codec_data_if_t* data_if;
static audio_codec_ctrl_if_t* ctrl_if;
static audio_codec_if_t* codec_if;
static audio_codec_gpio_if_t* gpio_if;
static esp_codec_dev_handle_t dev;
static i2c_master_bus_handle_t codec_i2c_bus;
static int out_vol;
static bool input_enabled;
static bool output_enabled;
static bool inited;

void board_enable_spk_amp(bool en) {
    #if GPIO_AMP_PA != -1
        #if GPIO_AMP_PA_INVERTED
            gpio_set_level(GPIO_AMP_PA, !en);
        #else
            gpio_set_level(GPIO_AMP_PA, en);
        #endif
    #endif
}
static esp_err_t update_state(bool input_enabled_, bool output_enabled_) {
    esp_err_t ret = ESP_OK;
    if (input_enabled_ || output_enabled_) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)sample_rate,
            .mclk_multiple = 0,
        };
        ESP_RETURN_ON_ERROR(esp_codec_dev_open(dev, &fs), TAG, "open dev");
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(dev, AUDIO_CODEC_DEFAULT_MIC_GAIN), TAG, "set in gain");
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(dev, out_vol), TAG, "set out vol");
    } else if (!input_enabled_ && !output_enabled_) {
        esp_codec_dev_close(dev);
    }
    board_enable_spk_amp(output_enabled_ && out_vol>0);
    input_enabled = input_enabled_;
    output_enabled = output_enabled_;
    return ret;
}

static void _del_i2s_channels() {
    if(tx_handle){
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
        tx_handle = NULL;
    }
    if(rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
}
static esp_err_t create_duplex_channels() {
	esp_err_t ret = ESP_OK;

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle), TAG, "new i2s chan");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)sample_rate,
            .clk_src = I2C_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2    
				.ext_clk_freq_hz = 0,
			#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2   
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif
        },
        .gpio_cfg = {
            .mclk = GPIO_MCLK,
            .bclk = GPIO_BCLK,
            .ws = GPIO_WS,
            .dout = GPIO_DOUT,
            .din = GPIO_DIN,
        }
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg) || i2s_channel_init_std_mode(rx_handle, &std_cfg),
    	TAG, "i2s init std mode");

    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_handle) || i2s_channel_enable(rx_handle), TAG, "enable i2s");
    return ret;

}

static esp_err_t init_i2c_bus() {
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_I2C_SDA,
        .scl_io_num = GPIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        // .flags = {
        //     .enable_internal_pullup = 1,
        // },
    };
    return i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus);
}

esp_err_t board_init() {
    if(inited) return ESP_OK;
	esp_err_t ret;

	sample_rate = AUDIO_CODEC_DEFAULT_SAMPLE_RATE;
	if((ret=create_duplex_channels())) return ret;

	audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle,
        .tx_handle = tx_handle,
    };
    ESP_RETURN_ON_FALSE((data_if=audio_codec_new_i2s_data(&i2s_cfg)), ESP_FAIL, TAG, "create data_if");
    ESP_RETURN_ON_ERROR(init_i2c_bus(), TAG, "init i2c bus");
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = I2C_ADDR,
        .bus_handle = codec_i2c_bus,
    };

    ESP_RETURN_ON_FALSE((ctrl_if=audio_codec_new_i2c_ctrl(&i2c_cfg)), ESP_FAIL, TAG, "create ctrl_if");
    ESP_RETURN_ON_FALSE((gpio_if=audio_codec_new_gpio()), ESP_FAIL, TAG, "create gpio_if");

    es8311_codec_cfg_t es8311_cfg = {
	    .ctrl_if = ctrl_if,
	    .gpio_if = gpio_if,
	    .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
	    .pa_pin = -1, // pa only controls output, lets set it manually.
	    .use_mclk = GPIO_MCLK!=-1,
	    .hw_gain.pa_voltage = 5.0,
	    .hw_gain.codec_dac_voltage = 3.3,
	    .pa_reverted = GPIO_AMP_PA_INVERTED,
	};
    ESP_RETURN_ON_FALSE((codec_if=es8311_codec_new(&es8311_cfg)), ESP_FAIL, TAG, "create dev_if");
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    ESP_RETURN_ON_FALSE(( dev = esp_codec_dev_new(&dev_cfg)), ESP_FAIL, TAG, "create dev");

    // setup amp pa
    const gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1<< GPIO_AMP_PA),
    };
    gpio_config(&io_conf);

    out_vol = 50;
    inited = 1;
    update_state(true, true);
	return ESP_OK;
}

esp_err_t board_deinit() {
    if(inited) {
    	esp_codec_dev_delete(dev);
        audio_codec_delete_codec_if(codec_if);
        audio_codec_delete_ctrl_if(ctrl_if);
        audio_codec_delete_gpio_if(gpio_if);
        audio_codec_delete_data_if(data_if);
        i2c_del_master_bus(codec_i2c_bus);
        _del_i2s_channels();

        board_enable_spk_amp(false);
        inited = 0;
    }
    return ESP_OK;
}

esp_codec_dev_handle_t board_get_mic_codec_dev() {
    return dev;
}

esp_codec_dev_handle_t board_get_spk_codec_dev() {
    return dev;
}

static esp_err_t board_enable_mic_spk(bool mic, bool spk) {
    if(mic==output_enabled && spk==input_enabled) return ESP_OK;
    return update_state(mic, spk);
}

esp_err_t board_enable_spk(bool en) {
    return en==output_enabled? ESP_OK: update_state(input_enabled, en);
}

esp_err_t board_enable_mic(bool en) {
    return en==input_enabled? ESP_OK: update_state(en, output_enabled);
}

esp_err_t board_set_spk_vol(int vol) {
    ESP_RETURN_ON_FALSE(output_enabled, ESP_ERR_INVALID_STATE, TAG, "speaker disabled");
    ESP_RETURN_ON_FALSE(0<=vol && vol<=100, ESP_ERR_INVALID_ARG, TAG, "vol out of range");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(dev, vol), TAG, "set out vol");
    if((vol>0) ^ (out_vol>0)) board_enable_spk_amp(vol>0);
    out_vol = vol;
    return ESP_OK;
}

int board_get_spk_vol() {
    return out_vol;
}