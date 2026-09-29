#ifndef I2S_VOICE_H
#define I2S_VOICE_H

#include <stdint.h>
#include "esp_err.h"

// ==========================
// I2S 扬声器 GPIO 定义
// ==========================
#define I2S_SPEAKER_BCLK_GPIO   9
#define I2S_SPEAKER_LRC_GPIO    10
#define I2S_SPEAKER_DOUT_GPIO   11

// ==========================
// 音频参数定义
// ==========================
// VoxCPM2 输出采样率为 48000 Hz，扬声器必须匹配，否则播放速度不对
#define I2S_SPEAKER_SAMPLE_RATE     48000
#define I2S_SPEAKER_CHANNELS        1
#define I2S_SPEAKER_BITS_PER_SAMPLE  16

// ==========================
// 函数声明
// ==========================

/**
 * @brief 初始化 I2S 扬声器
 * 
 * @return esp_err_t ESP_OK 成功，其他值表示失败
 */
esp_err_t i2s_speaker_init(void);

/**
 * @brief 发送 PCM 音频数据到 I2S 扬声器
 * 
 * @param pcm_data PCM 数据指针
 * @param data_len 数据长度（字节）
 * @param bytes_written 实际写入的字节数，可为NULL
 * 
 * @return esp_err_t ESP_OK 成功，其他值表示失败
 */
esp_err_t i2s_speaker_write(const void *pcm_data, size_t data_len, size_t *bytes_written);

/**
 * @brief 停止 I2S 扬声器
 * 
 * @return esp_err_t ESP_OK 成功，其他值表示失败
 */
esp_err_t i2s_speaker_stop(void);

/**
 * @brief 播放正弦波（用于测试）
 * 
 * @param freq 频率 (Hz)
 * @param duration_ms 持续时间 (ms)
 * @return esp_err_t ESP_OK 成功，其他值表示失败
 */
esp_err_t i2s_speaker_test_sine_wave(uint32_t freq, uint32_t duration_ms);

#endif // I2S_VOICE_H