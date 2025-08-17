#include "xz_util.h"
#include <string.h>

esp_err_t term_task_wait(TaskHandle_t task, EventGroupHandle_t eg, int32_t bit, TickType_t ticks) {
	if(task==NULL) return ESP_OK;
    xEventGroupClearBits(eg, bit);
    xTaskNotify(task, TASK_STOP_BIT, eSetBits);
    return (bit&xEventGroupWaitBits(eg, bit, pdTRUE, pdFALSE, ticks))? ESP_OK: ESP_ERR_TIMEOUT;
}

esp_err_t resume_task(TaskHandle_t task) {
    if(task == NULL) return ESP_ERR_INVALID_ARG;
    return pdPASS==xTaskNotify(task, 0, eNoAction)? ESP_OK: ESP_FAIL;
}

// static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t chr2hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

// decode hex string to NULL terminated byte array IN PLACE
void dec_hex_i(char* hex_string) {
    char* out = hex_string;
    for (; *hex_string; out++, hex_string+=2)
        *out = (chr2hex(*hex_string) << 4) | chr2hex(*(hex_string+1));
}


