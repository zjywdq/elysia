#include <stdio.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "tcp.h"
#include "i2s_voice.h"
#include "i2s_mirco.h"
#include "uart.h"
#include "bt_state.h"

#include "app_config.h"

#define WIFI_SSID APP_WIFI_SSID
#define WIFI_PASS APP_WIFI_PASSWORD


static const char *TAG = "wifi";


static EventGroupHandle_t wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0


// WiFi事件处理
static void wifi_event_handler(
        void *arg,
        esp_event_base_t event_base,
        int32_t event_id,
        void *event_data)
{

    if(event_base == WIFI_EVENT &&
       event_id == WIFI_EVENT_STA_START)
    {

        ESP_LOGI(TAG,"wifi start");

        esp_wifi_connect();
    }


    else if(event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED)
{
    wifi_event_sta_disconnected_t *event =
        (wifi_event_sta_disconnected_t *)event_data;

    ESP_LOGI(
        TAG,
        "wifi disconnected, reason=%d",
        event->reason
    );

    esp_wifi_connect();
}


    else if(event_base == IP_EVENT &&
            event_id == IP_EVENT_STA_GOT_IP)
    {

        // 连接成功后再次确保关闭省电，避免连接过程中被重置
        esp_wifi_set_ps(WIFI_PS_NONE);

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;


        ESP_LOGI(TAG,
        "got ip:" IPSTR,
        IP2STR(&event->ip_info.ip));


        xEventGroupSetBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT
            //将第0位（WIFI_CONNECTED_BIT）设置为1，表示Wi-Fi已连接成功。xEventGroupSetBits()函数用于在事件组中设置指定的事件位，从而通知等待该事件位的任务，Wi-Fi连接状态已改变。
        );
    }

}



void wifi_init_sta()
{

    wifi_event_group =
        xEventGroupCreate();
        //xEventGroupCreate()的作用是创建一个事件组，用于在FreeRTOS中实现任务之间的同步和通信。事件组允许任务等待特定的事件位被设置，从而实现任务间的协调和状态管理。在这个代码中，wifi_event_group用于管理Wi-Fi连接状态的事件位。


    // 初始化网络接口
    ESP_ERROR_CHECK(
        esp_netif_init()
        //esp_netif_init(）的作用是初始化网络接口库，为后续的网络操作提供基础设施。它会创建默认的网络接口，并为Wi-Fi、以太网等网络协议栈做好准备。
    );


    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
        //esp_event_loop_create_default()的作用是创建一个默认的事件循环，用于处理系统和应用程序中的事件。它会启动一个后台任务来监听和分发事件，使得应用程序可以注册事件处理函数来响应特定的事件。
    );


    esp_netif_create_default_wifi_sta();
    //esp_netif_create_default_wifi_sta()的作用是创建一个默认的Wi-Fi站点接口（STA），用于连接到Wi-Fi网络。它会配置网络接口的参数，并将其注册到网络接口库中，以便后续的Wi-Fi操作可以使用该接口进行通信.



    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();
        //cfg是一个wifi_init_config_t类型的结构体变量，用于存储Wi-Fi初始化配置。WIFI_INIT_CONFIG_DEFAULT()是一个宏，它会返回一个默认的Wi-Fi初始化配置结构体，包含了Wi-Fi驱动所需的各种参数和设置。通过将这个默认配置赋值给cfg，可以方便地使用默认设置来初始化Wi-Fi驱动，而无需手动设置每个参数。


    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );



    // 注册事件
    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );
    //注册WIFI事件


    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );
    //注册IP事件


    wifi_config_t wifi_config =
    {
        .sta =
        {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS
        },
    };


    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            //esp_wifi_set_mode()的作用是设置Wi-Fi的工作模式，可以选择站点模式（STA）、接入点模式（AP）或混合模式（AP+STA）。在这个代码中，WIFI_MODE_STA表示将Wi-Fi设置为站点模式，使设备能够连接到现有的Wi-Fi网络。
            WIFI_MODE_STA
        )
    );


    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            //esp_wifi_set_config()的作用是配置Wi-Fi接口的参数，包括SSID、密码、安全类型等。在这个代码中，WIFI_IF_STA表示配置的是站点模式（STA）的Wi-Fi接口，而wifi_config包含了要设置的具体参数，如SSID和密码。通过调用该函数，可以将指定的配置应用到Wi-Fi接口，使设备能够连接到目标Wi-Fi网络。
            WIFI_IF_STA,
            &wifi_config
        )
    );


    ESP_ERROR_CHECK(
        esp_wifi_start()
    );


    // 关闭 WiFi 省电（modem sleep）。
    // 实时音频流必须关掉，否则 send() 会周期性阻塞，
    // 导致 I2S DMA 缓冲溢出、丢数据。
    ESP_ERROR_CHECK(
        esp_wifi_set_ps(WIFI_PS_NONE)
    );


    ESP_LOGI(TAG,
        "wifi_init_sta finished");

}



void app_main(void)
{

    // 初始化flash
    esp_err_t ret =
        nvs_flash_init();
        //nvs_flash_init()的作用是初始化非易失性存储（NVS）库，用于在闪存中存储和读取数据。它会检查NVS分区的状态，如果分区未初始化或存在错误，会尝试进行初始化操作，以确保后续的NVS操作可以正常进行。


    if(ret == ESP_ERR_NVS_NO_FREE_PAGES ||
       ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }


    // 初始化 I2S 麦克风和扬声器
    i2s_init();  // 初始化 I2S 麦克风（参考示例）
    ESP_ERROR_CHECK(i2s_speaker_init());  // 初始化 I2S 扬声器

    // 初始化 PCM 转发 UART（921600，TX=GPIO20 → A2DP发送端 RX，RX=GPIO21 ← 对方 TX）
    uart_init(PCM_UART_BAUD_RATE);

    // 初始化 GPIO38 外部中断：监听 A2DP 发送端的蓝牙连接状态，
    // 蓝牙已连接时关闭本地扬声器输出，未连接时打开
    bt_state_init();

    wifi_init_sta();



    // 等待联网
    EventBits_t bits =
        xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY
        );
        //bits是一个EventBits_t类型的变量，用于存储等待事件组中指定事件位的结果。xEventGroupWaitBits()函数会阻塞当前任务，直到事件组中的指定事件位被设置为1或者超时发生。函数返回时，bits变量将包含事件组中所有事件位的状态，以便任务可以根据这些状态进行相应的处理.



    if(bits & WIFI_CONNECTED_BIT)
    //bits & WIFI_CONNECTED_BIT的作用是检查事件组中是否设置了WIFI_CONNECTED_BIT位。如果该位被设置为1，表示Wi-Fi已成功连接。通过使用按位与操作，可以判断bits变量中是否包含WIFI_CONNECTED_BIT位，从而确定Wi-Fi连接状态，并执行相应的逻辑。
    //&的作用是按位与运算符，用于比较两个二进制数的每一位。如果两个对应的位都是1，则结果为1，否则为0。在这里，它用于检查bits变量中是否包含WIFI_CONNECTED_BIT位。
    {
        ESP_LOGI(TAG, "WiFi connected!");

        // 启动TCP客户端任务
        xTaskCreate(
            tcp_client_task,
            "tcp_client",
            8192,
            NULL,
            5,
            NULL
        );
    }



    xTaskCreate(
        microphone_task,
        "microphone_task",

        8192,

        NULL,

        8,

        NULL
    );

    while(1)
    {
        vTaskDelay(
            1000 / portTICK_PERIOD_MS
        );

    }

}