#include "tcp.h"
#include "i2s_voice.h"
#include "uart.h"
#include "bt_state.h"
#include <stdlib.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_system.h"

static volatile int sock = -1;

// TCP 连接状态（连接成功后置 true，断开后置 false）
static volatile bool tcp_connected = false;

// 断线后自动重连的间隔（毫秒）
#define TCP_RECONNECT_DELAY_MS 3000
#define PLAY_CHUNK_SIZE 4096
#define TTS2_MAX_CHUNK (64 * 1024)
#define TTS2_PREBUFFER_CAP (384 * 1024) /* up to about 4 s at 48 kHz mono */

static int recv_full(int fd, void *buffer, size_t len)
{
    uint8_t *p = (uint8_t *)buffer;
    size_t received = 0;
    while (received < len) {
        int n = recv(fd, p + received, len - received, 0);
        if (n <= 0) {
            return -1;
        }
        received += (size_t)n;
    }
    return 0;
}

static void play_and_forward_pcm(const uint8_t *data, size_t len)
{
    static const uint8_t silence[PLAY_CHUNK_SIZE] = {0};
    while (len > 0) {
        size_t part = len > PLAY_CHUNK_SIZE ? PLAY_CHUNK_SIZE : len;
        size_t written = 0;
        esp_err_t ret = i2s_speaker_write(bt_is_connected() ? silence : data,
                                          part, &written);
        if (ret != ESP_OK) {
            ESP_LOGW("TCP", "I2S playback failed: %s", esp_err_to_name(ret));
        }
        pcm_uart_send(data, part);
        data += part;
        len -= part;
    }
}

static bool receive_tts2_stream(int fd, uint32_t sample_rate, uint32_t prebuffer_ms)
{
    static uint8_t chunk[PLAY_CHUNK_SIZE];
    size_t target = ((size_t)sample_rate * 2U * prebuffer_ms) / 1000U;
    if (target > TTS2_PREBUFFER_CAP) target = TTS2_PREBUFFER_CAP;
    if (target < PLAY_CHUNK_SIZE) target = PLAY_CHUNK_SIZE;
    uint8_t *prebuffer = malloc(target);
    if (prebuffer == NULL) {
        ESP_LOGE("TCP", "Cannot allocate TTS2 prebuffer (%u bytes)", (unsigned)target);
        return false;
    }

    size_t buffered = 0;
    bool playing = false;
    uint32_t total = 0;
    ESP_LOGI("TCP", "TTS2 stream: %lu Hz, prebuffer %lu ms (%u bytes)",
             (unsigned long)sample_rate, (unsigned long)prebuffer_ms,
             (unsigned)target);

    for (;;) {
        uint32_t net_len;
        if (recv_full(fd, &net_len, sizeof(net_len)) != 0) goto failed;
        uint32_t frame_len = ntohl(net_len);
        if (frame_len == 0) {
            if (buffered > 0) play_and_forward_pcm(prebuffer, buffered);
            ESP_LOGI("TCP", "TTS2 complete: %lu bytes", (unsigned long)total);
            free(prebuffer);
            return true;
        }
        if (frame_len > TTS2_MAX_CHUNK) {
            ESP_LOGE("TCP", "Invalid TTS2 chunk: %lu", (unsigned long)frame_len);
            goto failed;
        }

        uint32_t remaining = frame_len;
        while (remaining > 0) {
            size_t part = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
            if (recv_full(fd, chunk, part) != 0) goto failed;
            total += part;
            remaining -= part;

            if (!playing && buffered + part <= target) {
                memcpy(prebuffer + buffered, chunk, part);
                buffered += part;
                if (buffered >= target) {
                    playing = true;
                    play_and_forward_pcm(prebuffer, buffered);
                    buffered = 0;
                }
            } else {
                if (!playing && buffered > 0) {
                    play_and_forward_pcm(prebuffer, buffered);
                    buffered = 0;
                }
                playing = true;
                play_and_forward_pcm(chunk, part);
            }
        }
    }

failed:
    free(prebuffer);
    return false;
}


// ============================
// 通用发送接口
// ============================

int tcp_send(const void *data, size_t len)
{
    if (!tcp_connected || sock < 0)
    {
        return -1;
    }

    const char *p = (const char *)data;
    size_t sent_total = 0;

    while (sent_total < len)
    {
        int n = send(
            sock,
            p + sent_total,
            len - sent_total,
            0
        );

        if (n <= 0)
        {
            // 发送超时或出错：返回 -1，由调用方决定如何处理
            return -1;
        }

        sent_total += (size_t)n;
    }

    return (int)sent_total;
}


// ============================
// TCP连接任务（包含接收循环 + 断线自动重连）
// ============================

void tcp_client_task(void *pvParameters)
{
    // 服务器地址固定，连接前只需配置一次
    struct sockaddr_in server_addr;

    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );


    server_addr.sin_family =
        AF_INET;


    server_addr.sin_port =
        htons(SERVER_PORT);


    server_addr.sin_addr.s_addr =
        inet_addr(SERVER_IP);


    while(1)
    {
        // 每轮重连前先确保处于未连接状态
        tcp_connected = false;


        // 创建 socket

        sock = socket(
            AF_INET,
            SOCK_STREAM,
            0
        );


        if(sock < 0)
        {
            printf("socket创建失败，稍后重试\n");

            vTaskDelay(
                pdMS_TO_TICKS(TCP_RECONNECT_DELAY_MS)
            );

            continue;
        }


        // 连接服务器

        int err = connect(
            sock,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        );


        if(err != 0)
        {
            printf("连接服务器失败，稍后重试\n");

            close(sock);
            sock = -1;

            vTaskDelay(
                pdMS_TO_TICKS(TCP_RECONNECT_DELAY_MS)
            );

            continue;
        }


        printf("TCP连接成功\n");


        // 禁用 Nagle 算法，音频数据包立刻发送，降低延迟
        int flag = 1;
        setsockopt(
            sock,
            IPPROTO_TCP,
            TCP_NODELAY,
            &flag,
            sizeof(flag)
        );


        // 设置发送超时：20ms 内发不出去就放弃这一帧，
        // 避免 send() 因对端收得慢而无限阻塞（这是之前队列爆满的根因）。
        struct timeval tv = {
            .tv_sec  = 0,
            .tv_usec = 20000
        };
        setsockopt(
            sock,
            SOL_SOCKET,
            SO_SNDTIMEO,
            &tv,
            sizeof(tv)
        );


        // 标记连接成功，采集任务此时才允许发送数据
        tcp_connected = true;

        printf("TCP连接成功，等待接收音频数据...\n");

        // ============================
        // 接收循环：保持连接，循环接收多段音频
        // ============================
        bool connection_ok = true;

        while (connection_ok)
        {
            // ----------------------------
            // 1. 循环接收完整的 12 字节协议头
            //    recv 可能一次返回少于 12 字节，必须循环收满
            // ----------------------------
            char header[12];  // 4字节魔术码 + 4字节长度 + 4字节采样率
            int total_header = 0;

            while (total_header < (int)sizeof(header))
            {
                int n = recv(
                    sock,
                    header + total_header,
                    sizeof(header) - total_header,
                    0
                );

                if (n <= 0)
                {
                    printf("接收协议头失败，连接断开\n");
                    connection_ok = false;
                    break;
                }

                total_header += n;
            }

            if (!connection_ok)
            {
                break;
            }

            // 验证魔术码
            bool is_tts2 = (memcmp(header, "TTS2", 4) == 0);
            if (!is_tts2 && memcmp(header, "TTS1", 4) != 0)
            {
                printf("无效的音频数据格式\n");
                connection_ok = false;
                break;
            }

            // ----------------------------
            // 2. 解析数据长度和采样率
            //    Python 用网络字节序（大端）打包，ESP32 是小端，
            //    必须用 ntohl() 转换，否则数值完全错误
            // ----------------------------
            uint32_t field1_net;
            uint32_t field2_net;
            memcpy(&field1_net, header + 4, sizeof(field1_net));
            memcpy(&field2_net, header + 8, sizeof(field2_net));
            uint32_t data_len = ntohl(field1_net);
            uint32_t sample_rate = ntohl(field2_net);

            if (is_tts2) {
                sample_rate = data_len;
                uint32_t prebuffer_ms = ntohl(field2_net);
                if (!receive_tts2_stream(sock, sample_rate, prebuffer_ms)) {
                    connection_ok = false;
                }
                continue;
            }

            printf(
                "收到音频数据: 长度=%lu, 采样率=%lu\n",
                (unsigned long)data_len,
                (unsigned long)sample_rate
            );

            // ----------------------------
            // 3. 边接收边播放（流式）
            //    使用 static 缓冲区，避免占用任务栈
            //    （栈溢出会破坏堆和 TLS，导致 socket() 崩溃）
            // ----------------------------
            static uint8_t play_buffer[PLAY_CHUNK_SIZE];

            uint32_t total_received = 0;
            bool recv_ok = true;

            while (total_received < data_len)
            {
                uint32_t remaining = data_len - total_received;
                uint32_t this_chunk =
                    (remaining < PLAY_CHUNK_SIZE) ? remaining : PLAY_CHUNK_SIZE;

                // 循环接收确保收满 this_chunk 字节
                uint32_t chunk_received = 0;
                while (chunk_received < this_chunk)
                {
                    int n = recv(
                        sock,
                        play_buffer + chunk_received,
                        this_chunk - chunk_received,
                        0
                    );

                    if (n <= 0)
                    {
                        printf("接收音频数据失败\n");
                        recv_ok = false;
                        break;
                    }

                    chunk_received += n;
                }

                if (!recv_ok)
                {
                    break;
                }

                // 立即播放这一块
                size_t bytes_written = 0;

                // 蓝牙已连接时关闭本地扬声器输出（写静音数据，保持 I2S
                // 播放节奏不被打断）；蓝牙未连接时正常输出。
                // 无论是否静音，PCM 都照常转发给 A2DP 发送端，互不影响。
                static const uint8_t silence_frame[PLAY_CHUNK_SIZE] = { 0 };

                esp_err_t ret = i2s_speaker_write(
                    bt_is_connected() ? silence_frame : play_buffer,
                    chunk_received,
                    &bytes_written
                );

                if (ret != ESP_OK)
                {
                    printf("I2S 播放失败: %s\n", esp_err_to_name(ret));
                }

                // 同步把同一份 PCM 转发到 UART，发给 A2DP 发送端（非阻塞，不影响播放）
                pcm_uart_send(play_buffer, chunk_received);

                total_received += chunk_received;
            }

            if (recv_ok)
            {
                printf(
                    "音频播放完成: %lu 字节\n",
                    (unsigned long)total_received
                );
            }
            else
            {
                connection_ok = false;
            }
        }

        // ============================
        // 连接已断开：清理 socket 并等待一段时间后自动重连
        // ============================

        tcp_connected = false;
        close(sock);
        sock = -1;

        printf(
            "TCP连接断开，%d ms 后自动重连\n",
            TCP_RECONNECT_DELAY_MS
        );

        vTaskDelay(
            pdMS_TO_TICKS(TCP_RECONNECT_DELAY_MS)
        );
    }
}
