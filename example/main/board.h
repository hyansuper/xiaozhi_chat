#pragma once
#include <esp_codec_dev.h>

esp_err_t board_init();
esp_err_t board_deinit();

esp_codec_dev_handle_t board_get_mic_codec_dev();
esp_codec_dev_handle_t board_get_spk_codec_dev();

esp_err_t board_enable_spk(bool en);
esp_err_t board_enable_mic(bool en);
esp_err_t board_set_spk_vol(int vol);
int board_get_spk_vol();
void board_enable_spk_amp(bool en);