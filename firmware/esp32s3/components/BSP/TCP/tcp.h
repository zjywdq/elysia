#ifndef TCP_H
#define TCP_H

#include <stdio.h>

#include <string.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"


#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <stddef.h>


// 电脑IP
#include "app_config.h"
#define SERVER_IP APP_SERVER_IP


// 电脑端口
#define SERVER_PORT APP_SERVER_PORT


// ============================
// 通用发送接口
// ============================
//
// 发送任意数据（循环发送直到全部发完，socket 已设置发送超时）。
// 返回实际发送的字节数；未连接或发送失败返回 -1。
int tcp_send(const void *data, size_t len);


void tcp_client_task(void *pvParameters);

#endif // TCP_H