#include "bt_state.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "BT_STATE";

// 蓝牙连接状态输入脚：接 A2DP 发送端（bule teeh 工程）的 STATUS_GPIO（GPIO5）。
// 对端蓝牙连上耳机时输出高电平，断开时输出低电平，据此开关本地扬声器。
#define BT_STATE_GPIO   GPIO_NUM_38

// 电平消抖时间（毫秒）。对端的 GPIO5 是 strapping 脚，上电瞬间会有
// 短暂脉冲，消抖后采样到的才是稳定电平。
#define DEBOUNCE_MS     50

// 中断事件队列。长度固定为 1：只保留最新一次边沿，
// 抖动期间产生的多次触发会互相覆盖，不会堆积。
static QueueHandle_t s_evt_queue;

// 蓝牙连接状态：true = 已连接（本地扬声器关闭），false = 未连接（扬声器打开）
static volatile bool s_bt_connected = false;

// ============================
// GPIO 中断服务函数
// ============================
// 任意边沿触发。ISR 里不做任何业务逻辑，只把当前电平丢进队列后尽快返回，
// 消抖和状态更新交给 bt_state_task 处理。
static void IRAM_ATTR bt_state_gpio_isr(void *arg)
{
    int level = gpio_get_level(BT_STATE_GPIO);
    xQueueOverwriteFromISR(s_evt_queue, &level, NULL);
}

// ============================
// 消抖任务：等待电平稳定后更新蓝牙状态
// ============================
static void bt_state_task(void *arg)
{
    int level;

    while (1) {
        // 阻塞等待中断事件
        if (xQueueReceive(s_evt_queue, &level, portMAX_DELAY)) {
            // 等待电平稳定，滤除抖动
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

            // 消抖结束后重新采样实际电平（以最终电平为准）
            bool connected = gpio_get_level(BT_STATE_GPIO);
            if (connected != s_bt_connected) {
                s_bt_connected = connected;
                ESP_LOGI(TAG, "蓝牙%s → 本地扬声器%s",
                         connected ? "已连接" : "已断开",
                         connected ? "关闭" : "打开");
            }
        }
    }
}

// ============================
// 初始化 GPIO38 外部中断
// ============================
void bt_state_init(void)
{
    // GPIO38 配置为输入，双边沿外部中断（上升沿=蓝牙连接，下降沿=蓝牙断开）
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BT_STATE_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    s_evt_queue = xQueueCreate(1, sizeof(int));
    configASSERT(s_evt_queue != NULL);

    // 安装 GPIO 中断服务（本项目其它地方没有使用 GPIO 中断，直接安装即可）
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BT_STATE_GPIO, bt_state_gpio_isr, NULL));

    // 中断服务是刚装的，之前的边沿已经错过，先按当前电平同步一次初始状态
    s_bt_connected = gpio_get_level(BT_STATE_GPIO);
    ESP_LOGI(TAG, "蓝牙状态监听就绪: GPIO%d，当前%s",
             BT_STATE_GPIO,
             s_bt_connected ? "已连接，扬声器关闭" : "未连接，扬声器打开");

    // 启动消抖任务
    xTaskCreate(bt_state_task, "bt_state", 2048, NULL, 8, NULL);
}

// ============================
// 查询蓝牙连接状态
// ============================
bool bt_is_connected(void)
{
    return s_bt_connected;
}
