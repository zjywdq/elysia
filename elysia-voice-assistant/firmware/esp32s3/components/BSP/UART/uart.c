#include "uart.h"



void uart_init(uint32_t baud_rate)
{
    uart_config_t uart_config = {0};


    uart_config.baud_rate = baud_rate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    //parity是UART通信中的一种错误检测机制，用于检测数据传输过程中是否发生了错误。它通过在数据位中添加一个额外的位（称为奇偶校验位）来实现。根据设置的奇偶校验类型，UART接收端可以检查接收到的数据是否符合预期的奇偶性，从而判断数据是否被篡改或传输过程中是否出现了错误。在这里，UART_PARITY_DISABLE表示禁用奇偶校验，即不使用奇偶校验位进行错误检测。
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_APB;
    // Configure UART parameters


    uart_param_config(UART_NUM, &uart_config);

    // Set UART pins
    uart_set_pin(UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // Install UART driver（TX 缓冲加大到 8KB：2M 波特率下约 40ms 音频，
    // 用于吸收 WiFi/TCP 抖动；RX 保持原样）
    uart_driver_install(UART_NUM, RX_BUF_SIZE * 2, 8192, 20, NULL, 0);
}

void uart_send_task(void *pvParameters)
{
    uart_msg_t msg;

    while (1) {


            // msg.data[msg.len] = '\r';
            // msg.len++;
            // msg.data[msg.len] = '\n';
            // msg.len++;
            ESP_LOGI("UART", "Sending %d bytes to AM335x", msg.len);

            uart_write_bytes(
                UART_NUM_1,
                msg.data,
                msg.len
            );
        }
}


void uart_receive_task(void *pvParameters)
{
    uart_msg_t msg;

    while (1) {
        uart_get_buffered_data_len(UART_NUM, (size_t *)&msg.len);

        if (msg.len > 0)
        {
            // ★ 关键修复：限制读取长度不超过缓冲区大小减1（留一个给'\0'）
            if (msg.len >= RX_BUF_SIZE)
            {
                msg.len = RX_BUF_SIZE - 1;
            }

            memset(msg.data, 0, RX_BUF_SIZE);
            uart_read_bytes(UART_NUM, msg.data, msg.len, portMAX_DELAY);
            // msg.data[msg.len] = '\0';  // 现在安全了

            ESP_LOGI("UART", "Received %s ", msg.data);


        }





        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


// ============================
// PCM 转发：48kHz 单声道 → 44.1kHz 立体声 重采样后发给 A2DP 发送端
// ============================

// A2DP 发送端按 44.1kHz 立体声消耗 PCM（176400 字节/秒），
// 而扬声器收到的原始数据是 48kHz 单声道（96000 字节/秒）。
// 之前直接转发字节流，因为格式不匹配 + 921600 波特率装不下，
// 耳机里全是爆音。这里先线性插值重采样到 44100Hz，
// 再把单声道复制成左右声道，输出正好是发送端需要的 176400 字节/秒，
// 2M 波特率的 UART（约 200000 字节/秒）可以稳定承载。

#define PCM_IN_RATE     48000
#define PCM_OUT_RATE    44100
// 相位步进 16.16 定点：(48000/44100) * 65536 ≈ 71331
#define RESAMPLE_STEP   ((48000UL << 16) / PCM_OUT_RATE)
#define PCM_UART_HEADER_SIZE 12
#define PCM_UART_FRAME_PAYLOAD 512

static uint16_t pcm_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void pcm_uart_send(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    static uint8_t  s_half_byte;      // 跨块缓存的半个采样（TCP 分包可能切断采样）
    static bool     s_half_pending;
    static int16_t  s_prev_sample;    // 上一个输入采样（插值基准）
    static uint32_t s_phase;          // 16.16 定点相位：[0,65536) 表示 prev..cur 之间的位置
    static bool     s_started;
    static uint16_t s_sequence;
    uint32_t s_overflow_frames = 0;

    // 4096 字节输入最多产出 4096/2*44100/48000*4 ≈ 7528 字节输出，8KB 足够
    static uint8_t out_buf[8192 + PCM_UART_HEADER_SIZE];
    size_t out_len = PCM_UART_HEADER_SIZE;

    size_t i = 0;
    while (i < len) {
        uint16_t raw;
        if (s_half_pending) {
            raw = (uint16_t)s_half_byte | ((uint16_t)data[i] << 8);
            s_half_pending = false;
            i++;
        } else {
            if (i + 1 >= len) {      // 奇数结尾：留下一个字节，和下次的拼成一个采样
                s_half_byte = data[i];
                s_half_pending = true;
                break;
            }
            raw = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
            i += 2;
        }
        int16_t cur = (int16_t)raw;

        if (!s_started) {
            s_prev_sample = cur;
            s_phase = 0;
            s_started = true;
        }

        // 每来一个新输入采样，在 prev(相位0) 到 cur(相位65536) 之间按相位取点
        while (s_phase < 65536) {
            if (out_len + 2 <= sizeof(out_buf)) {
                // 线性插值：prev + (cur - prev) * phase / 65536
                int32_t diff = (int32_t)cur - (int32_t)s_prev_sample;
                int16_t v = (int16_t)(s_prev_sample +
                                      (int16_t)(((int64_t)diff * (int64_t)s_phase) >> 16));
                // 单声道复制成左右声道（L = R）
                out_buf[out_len++] = (uint8_t)(v & 0xFF);
                out_buf[out_len++] = (uint8_t)((v >> 8) & 0xFF);
            } else {
                s_overflow_frames++;    // 理论不会进这里，只计数
            }
            s_phase += RESAMPLE_STEP;
        }
        s_phase -= 65536;               // 相位折回 [0, 65536)
        s_prev_sample = cur;
    }

    if (out_len == PCM_UART_HEADER_SIZE) {
        return;
    }

    size_t payload_total = out_len - PCM_UART_HEADER_SIZE;
    size_t payload_offset = 0;
    uint32_t write_dropped = s_overflow_frames * 2;
    static uint8_t frame_buf[PCM_UART_HEADER_SIZE + PCM_UART_FRAME_PAYLOAD];

    while (payload_offset < payload_total) {
        size_t payload_len = payload_total - payload_offset;
        if (payload_len > PCM_UART_FRAME_PAYLOAD) {
            payload_len = PCM_UART_FRAME_PAYLOAD;
        }

        frame_buf[0] = 0xa5;
        frame_buf[1] = 0x5a;
        frame_buf[2] = 0xc3;
        frame_buf[3] = 0x3c;
        frame_buf[4] = (uint8_t)payload_len;
        frame_buf[5] = (uint8_t)(payload_len >> 8);
        frame_buf[6] = (uint8_t)s_sequence;
        frame_buf[7] = (uint8_t)(s_sequence >> 8);
        memcpy(frame_buf + PCM_UART_HEADER_SIZE,
               out_buf + PCM_UART_HEADER_SIZE + payload_offset, payload_len);
        uint16_t payload_crc = pcm_crc16(frame_buf + PCM_UART_HEADER_SIZE,
                                         payload_len);
        frame_buf[8] = (uint8_t)payload_crc;
        frame_buf[9] = (uint8_t)(payload_crc >> 8);
        uint16_t header_crc = pcm_crc16(frame_buf, 10);
        frame_buf[10] = (uint8_t)header_crc;
        frame_buf[11] = (uint8_t)(header_crc >> 8);
        s_sequence++;

        size_t frame_len = PCM_UART_HEADER_SIZE + payload_len;
        int written = uart_write_bytes(UART_NUM, frame_buf, frame_len);
        if (written < 0) {
            written = 0;
        }
        write_dropped += (uint32_t)(frame_len - (size_t)written);
        payload_offset += payload_len;
    }

    static uint32_t dropped_bytes = 0;
    static TickType_t last_log_tick = 0;

    dropped_bytes += write_dropped;

    // 只在丢数据时提示，且最多每 5 秒打印一次，避免刷屏
    TickType_t now = xTaskGetTickCount();
    if (dropped_bytes > 0 && (now - last_log_tick) >= pdMS_TO_TICKS(5000)) {
        ESP_LOGW("UART", "PCM UART 发送跟不上，累计丢弃 %lu 字节（对端会自动补静音）",
                 (unsigned long)dropped_bytes);
        last_log_tick = now;
    }
}
