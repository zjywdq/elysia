#ifndef UART_H
#define UART_H

// UART function declarations and data structures go here

#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_select.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define UART_NUM UART_NUM_1
// UART_TX_PIN GPIO_NUM_20用来发送数据，UART_RX_PIN GPIO_NUM_21用来接收数据。
// 接线：本机 TX(20) → A2DP发送端 RX(GPIO16)，本机 RX(21) → 对方 TX(GPIO17)。
// 注意：GPIO20 是原生 USB 的 D+，使用这两个引脚期间不要连接开发板的原生 USB 口。
#define UART_TX_PIN GPIO_NUM_39
#define UART_RX_PIN GPIO_NUM_40

// PCM 转发波特率，必须与 A2DP 发送端的 PCM_UART_BAUD_RATE 一致。
// A2DP 发送端按 44.1kHz 立体声消耗 PCM = 176400 字节/秒，
// 921600 波特率只有约 92160 字节/秒，物理上装不下（爆音根因），
// 所以两边统一改用 2000000（约 200000 字节/秒，留有余量）。
#define PCM_UART_BAUD_RATE 1000000

/* UART wire format: 44100 Hz, signed 16-bit little-endian, mono PCM.
 * The A2DP receiver duplicates each sample to left and right channels. */

#define RX_BUF_SIZE 1024
#define QUEUE_SIZE 10
#define DATA_SIZE  1024



typedef struct {
    int len;
    uint8_t data[DATA_SIZE];
} uart_msg_t;

void uart_init(uint32_t baud_rate);
void uart_send_task(void *pvParameters);
void uart_receive_task(void *pvParameters);

// 把扬声器收到的同一份 PCM 数据同步转发到 UART（发给 A2DP 发送端）
// 非阻塞：带宽不足时丢弃数据，绝不影响 I2S 播放
void pcm_uart_send(const uint8_t *data, size_t len);

#endif // UART_H
