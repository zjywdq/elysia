#ifndef I2S_H
#define I2S_H

#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_err.h"


#define I2S_TAG "MIC"

// ============================
// I2S 引脚
// ============================

#define I2S_BCLK_GPIO    4
#define I2S_WS_GPIO      5
#define I2S_DIN_GPIO     6


// ============================
// 音频参数
// ============================

#define SAMPLE_RATE      16000

// INMP441 输出 24bit 数据，I2S 按 32bit 槽位读取（数据左对齐在高 24 位）
#define SAMPLE_BITS      I2S_DATA_BIT_WIDTH_32BIT

// 一次读取 1024 个字节
#define BUFFER_SIZE      1024

// I2S function declarations and definitions go here

void i2s_init(void);
void microphone_task(void *arg);

typedef struct
{
    size_t len;
    uint8_t data[BUFFER_SIZE];

} audio_frame_t;

#endif // I2S_H