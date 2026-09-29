#include "i2s_voice.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <math.h>

static const char *TAG = "I2S_VOICE";

// 全局变量：I2S TX 通道句柄
static i2s_chan_handle_t tx_handle;

// ==========================
// 初始化 I2S 扬声器
// ==========================
esp_err_t i2s_speaker_init(void)
{
    ESP_LOGI(TAG, "初始化 I2S 扬声器");

    // 创建 I2S TX 通道（使用I2S_NUM_1避免与麦克风冲突）
    i2s_chan_config_t chan_cfg = 
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 标准 I2S 模式配置
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SPEAKER_SAMPLE_RATE),

        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_MONO  // 单声道配置
        ),

        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPEAKER_BCLK_GPIO,
            .ws   = I2S_SPEAKER_LRC_GPIO,
            .dout = I2S_SPEAKER_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化 I2S 标准模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启用 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2S 扬声器初始化成功");
    return ESP_OK;
}

// ==========================
// 写入 PCM 数据到 I2S 扬声器
// ==========================
esp_err_t i2s_speaker_write(const void *pcm_data, size_t data_len, size_t *bytes_written)
{
    if (pcm_data == NULL || data_len == 0) {
        ESP_LOGE(TAG, "无效的 PCM 数据");
        return ESP_ERR_INVALID_ARG;
    }

    // I2S 写入
    size_t bytes_written_local = 0;
    esp_err_t ret = i2s_channel_write(
        tx_handle,
        pcm_data,
        data_len,
        &bytes_written_local,
        portMAX_DELAY
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S 写入失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if (bytes_written) {
        *bytes_written = bytes_written_local;
    }

    return ESP_OK;
}

// ==========================
// 停止 I2S 扬声器
// ==========================
esp_err_t i2s_speaker_stop(void)
{
    ESP_LOGI(TAG, "停止 I2S 扬声器");

    // 停止通道
    esp_err_t ret = i2s_channel_disable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "停止 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 删除通道
    ret = i2s_del_channel(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "删除 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2S 扬声器已停止");
    return ESP_OK;
}

// ==========================
// 测试：播放正弦波
// ==========================
esp_err_t i2s_speaker_test_sine_wave(uint32_t freq, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "播放正弦波测试 - 频率: %d Hz, 持续时间: %d ms", freq, duration_ms);

    // 配置正弦波参数
    const uint32_t sample_rate = I2S_SPEAKER_SAMPLE_RATE;
    const int16_t amplitude = 3000; // 幅度 - 降低音量
    const float phase_step = 2.0f * M_PI * freq / sample_rate;

    float phase = 0.0f;
    uint32_t samples_to_play = (duration_ms * sample_rate) / 1000;
    uint32_t samples_played = 0;

    // 缓冲区大小：256 个样本（每个样本 2 字节）
    #define BUFFER_SIZE 256
    int16_t buffer[BUFFER_SIZE];

    while (samples_played < samples_to_play) {
        // 填充缓冲区
        for (int i = 0; i < BUFFER_SIZE && samples_played < samples_to_play; i++) {
            // 生成正弦波样本
            buffer[i] = (int16_t)(amplitude * sinf(phase));

            phase += phase_step;
            if (phase >= 2.0f * M_PI) {
                phase -= 2.0f * M_PI;
            }

            samples_played++;
        }

        // 写入 I2S
        size_t bytes_written;
        esp_err_t ret = i2s_speaker_write(buffer, sizeof(buffer), &bytes_written);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "正弦波写入失败");
            return ret;
        }
    }

    ESP_LOGI(TAG, "正弦波播放完成");
    return ESP_OK;
}