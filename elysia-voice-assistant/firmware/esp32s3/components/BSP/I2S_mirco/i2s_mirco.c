#include "i2s_mirco.h"

#include "tcp.h"


static i2s_chan_handle_t rx_handle;

// I2S 接收溢出次数（丢数据时递增，用于判断杂音是否来自丢帧）
static volatile uint32_t rx_overflow_count = 0;


// I2S RX 队列溢出回调
static IRAM_ATTR bool i2s_rx_q_ovf_cb(
        i2s_chan_handle_t handle,
        i2s_event_data_t *event,
        void *user_ctx)
{
    rx_overflow_count++;
    return false;
}


// ============================
// 初始化 I2S
// ============================

void i2s_init(void)
{
    // 创建 I2S RX 通道（麦克风 → ESP32）
    // 增大 DMA 缓冲，避免处理不及时导致丢数据
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_0,
            I2S_ROLE_MASTER
        );
    chan_cfg.dma_desc_num = 8;
    //chan_cfg.dma_desc_num = 8;是什么？
    // 8 是 DMA 描述符的数量，表示 I2S 驱动可以同时管理 8 个 DMA 缓冲区。
    chan_cfg.dma_frame_num = 480;
    //chan_cfg.dma_frame_num = 480;是什么？
    // 480 是 I2S 的帧数，表示每个 DMA 缓冲区可以存储 480 个采样点。
    // 480 个采样点对应 480 * 2 = 960 字节的缓冲区。

    ESP_ERROR_CHECK(
        i2s_new_channel(
            &chan_cfg,
            NULL,
            &rx_handle
        )
    );


    // ============================
    // I2S 标准模式配置
    // ============================

    i2s_std_config_t std_cfg = {

        // 时钟配置
        .clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(
                SAMPLE_RATE
            ),

        // 数据格式
        // INMP441 是标准 I2S（Philips）格式，不是 MSB 对齐
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                SAMPLE_BITS,
                I2S_SLOT_MODE_STEREO
            ),

        // GPIO 配置
        .gpio_cfg = {

            // 不使用 MCLK
            .mclk = I2S_GPIO_UNUSED,

            // BCLK → GPIO4
            .bclk = I2S_BCLK_GPIO,

            // WS → GPIO5
            .ws = I2S_WS_GPIO,

            // 不发送数据
            .dout = I2S_GPIO_UNUSED,

            // 接收数据
            // SD → GPIO6
            .din = I2S_DIN_GPIO,

            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    // INMP441 是单声道（L/R 接地 → 左声道），
    // 但它的总线时序必须按 64×FS（左右两个槽位）走。
    // 若用 MONO，BCLK 只有 32×FS，麦克风数据会错乱、声音严重变形。
    // 所以槽位用 STEREO，再只启用左声道，驱动就只返回左声道连续样本。
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;


    // 初始化 I2S 标准模式

    ESP_ERROR_CHECK(
        i2s_channel_init_std_mode(
            rx_handle,
            &std_cfg
        )
        //i2s_new_channel和i2s_channel_init_std_mode的区别是什么？
        // i2s_new_channel 用于创建一个新的 I2S 通道（TX 或 RX），并返回通道句柄。
        // i2s_channel_init_std_mode 用于初始化一个已创建的 I2S通道，使其进入标准模式（Philips、MSB、PCM 等），并配置时钟、槽位和 GPIO。
    );


    // 注册溢出回调，用于检测接收数据是否丢失
    i2s_event_callbacks_t cbs = {
        .on_recv = NULL,
        .on_recv_q_ovf = i2s_rx_q_ovf_cb,
        .on_sent = NULL,
        .on_send_q_ovf = NULL,
    };
    i2s_channel_register_event_callback(
        rx_handle,
        &cbs,
        NULL
    );


    // 启动 I2S

    ESP_ERROR_CHECK(
        i2s_channel_enable(
            rx_handle
        )
    );


    ESP_LOGI(
        I2S_TAG,
        "I2S microphone started"
    );
}


// ============================
// 麦克风采集任务
// ============================

void microphone_task(void *arg)
{
    /*
     * 音频缓冲区
     *
     * INMP441 按 32bit 槽位读取，
     * 每个采样占 4 字节。
     */

    int32_t raw_samples[BUFFER_SIZE / 4];
    //为什么要/4？因为每个采样占4个字节，所以缓冲区大小除以4得到采样点的数量。

    size_t bytes_read;

    // 上次已报告的溢出次数
    static uint32_t last_overflow = 0;

    // 因发送失败/未连接而丢弃的帧数
    uint32_t dropped_frames = 0;


    while (1)
    {
        /*
         * 从 I2S 读取数据
         */

        esp_err_t ret =
            i2s_channel_read(
                rx_handle,

                raw_samples,

                sizeof(raw_samples),

                &bytes_read,

                portMAX_DELAY
            );


        if (ret == ESP_OK)
        {
            /*
             * bytes_read 表示：
             *
             * 实际读取了多少字节
             */

            audio_frame_t frame;

            frame.len = 0;

            size_t sample_count =
                bytes_read / sizeof(int32_t);


            /*
             * INMP441 的 24bit 数据左对齐在
             * 32bit 槽位的高 24 位（bit31..bit8），
             *
             * 取高 16 位（>>16）转成 16bit PCM，
             * 方便电脑端按 16bit 播放。
             */

            for (size_t i = 0;
                 i < sample_count;
                 i++)
            {
                int16_t s =
                    (int16_t)(raw_samples[i] >> 16);


                // 小端存储：低字节在前
                frame.data[frame.len++] =
                    (uint8_t)(s & 0xFF);

                frame.data[frame.len++] =
                    (uint8_t)((s >> 8) & 0xFF);
            }

            /*
             * 参考 arpy8/ESP32_Voice_Assistant 的读法：
             * 读出来直接交给 tcp_send() 发送，不经过中间软件队列。
             *
             * 采集速率 = 发送速率，由 I2S 的 DMA 缓冲自然背压，
             * 不会出现“软件队列越堆越满”的问题。
             *
             * tcp_send() 内部带发送超时，发不出去就返回失败，
             * 这里丢弃这一帧，保证音频始终是最新的、实时的。
             */
            if (tcp_send(frame.data, frame.len) < 0)
            {
                // 未连接 / 发送超时 / 出错：丢弃这一帧，保持实时
                dropped_frames++;
            }
        }
        else
        {
            ESP_LOGE(
                I2S_TAG,
                "I2S read error: %s",
                esp_err_to_name(ret)
            );
        }


        // 如果发生数据丢失，打印警告
        if (rx_overflow_count != last_overflow)
        {
            last_overflow = rx_overflow_count;
            ESP_LOGW(
                I2S_TAG,
                "RX overflow! lost=%lu",
                (unsigned long)rx_overflow_count
            );
        }


        // 如果因发送失败/未连接而丢帧，打印统计
        static uint32_t last_dropped = 0;
        if (dropped_frames != last_dropped)
        {
            last_dropped = dropped_frames;
            ESP_LOGW(
                I2S_TAG,
                "send dropped=%lu",
                (unsigned long)dropped_frames
            );
        }
    }
}