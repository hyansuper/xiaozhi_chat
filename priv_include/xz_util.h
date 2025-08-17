#pragma once
#include "esp_err.h"
#include "xz_common.h"



#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"


#define TASK_STOP_BIT (1<<16)
esp_err_t resume_task(TaskHandle_t task);
esp_err_t term_task_wait(TaskHandle_t task, EventGroupHandle_t eg, int32_t bit, TickType_t ticks);



void dec_hex_i(char* hex_string);

