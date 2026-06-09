/**
 * UART0 <-> UART1 双向转发 (ESP32-C3)
 *
 * UART0: GPIO20=RX, GPIO21=TX (显式设置)
 * UART1: RX=GPIO0, TX=GPIO1  (已确认 GPIO0 有数据输入)
 * 波特率: 5,000,000 (5 MBaud)
 *
 * Console: USB Serial/JTAG (不影响 UART0)
 * 采用事件驱动模型，双任务并行转发，带统计监控
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "UART_FWD";

/* ============ 引脚定义 ============ */
#define UART0_RX_PIN    GPIO_NUM_20
#define UART0_TX_PIN    GPIO_NUM_21
#define UART1_RX_PIN    GPIO_NUM_0   /* 数据输入在此引脚 */
#define UART1_TX_PIN    GPIO_NUM_1

/* ============ 缓冲与波特率配置 ============ */
#define UART_BAUD_RATE  5000000     // 5 MBaud
#define BUF_SIZE        4096
#define RD_BUF_SIZE     4096
#define EVT_QUEUE_SIZE  20

/* ============ 事件队列句柄 ============ */
static QueueHandle_t uart0_evt_queue = NULL;
static QueueHandle_t uart1_evt_queue = NULL;

/* ============ 转发统计 ============ */
static volatile uint32_t fwd_0_to_1_bytes = 0;
static volatile uint32_t fwd_1_to_0_bytes = 0;

/**
 * @brief 通用 UART 事件处理任务
 */
static void uart_forward_task(void *param)
{
    uart_port_t *ports = (uart_port_t *)param;
    uart_port_t src  = ports[0];
    uart_port_t dest = ports[1];
    QueueHandle_t evt_queue = (src == UART_NUM_0) ? uart0_evt_queue : uart1_evt_queue;
    volatile uint32_t *counter = (src == UART_NUM_0) ? &fwd_0_to_1_bytes : &fwd_1_to_0_bytes;

    uart_event_t event;
    uint8_t *buf = (uint8_t *)malloc(RD_BUF_SIZE);
    assert(buf);

    ESP_LOGI(TAG, "Forward task started: UART%d -> UART%d", src, dest);

    for (;;) {
        if (xQueueReceive(evt_queue, (void *)&event, portMAX_DELAY)) {
            switch (event.type) {
            case UART_DATA:
                if (event.size > 0) {
                    int total = 0;
                    while (total < event.size) {
                        int to_read = event.size - total;
                        if (to_read > RD_BUF_SIZE) to_read = RD_BUF_SIZE;
                        int len = uart_read_bytes(src, buf, to_read, portMAX_DELAY);
                        if (len > 0) {
                            uart_write_bytes(dest, buf, len);
                            *counter += len;
                            total += len;
                        } else break;
                    }
                }
                break;

            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "UART%d FIFO overflow", src);
                uart_flush_input(src);
                xQueueReset(evt_queue);
                break;

            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "UART%d buffer full", src);
                uart_flush_input(src);
                xQueueReset(evt_queue);
                break;

            case UART_BREAK:
                break;

            case UART_PARITY_ERR:
                ESP_LOGW(TAG, "UART%d parity error", src);
                break;

            case UART_FRAME_ERR:
                ESP_LOGW(TAG, "UART%d frame error", src);
                break;

            default:
                break;
            }
        }
    }

    free(buf);
    vTaskDelete(NULL);
}

/**
 * @brief 统计监控任务，每 5 秒打印转发字节数
 */
static void monitor_task(void *param)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t a = fwd_0_to_1_bytes;
        uint32_t b = fwd_1_to_0_bytes;
        ESP_LOGI(TAG, "[STATS] UART0->UART1: %lu bytes | UART1->UART0: %lu bytes", (unsigned long)a, (unsigned long)b);
    }
}

/**
 * @brief 初始化指定 UART 端口，显式设置引脚
 */
static void uart_init_port(uart_port_t port, int tx_pin, int rx_pin, QueueHandle_t *evt_queue)
{
    /* UART0 可能被 console 占用，先删除已有驱动 */
    if (port == UART_NUM_0) {
        uart_driver_delete(UART_NUM_0);
    }

    uart_config_t cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(port, BUF_SIZE * 2, BUF_SIZE * 2,
                                         EVT_QUEUE_SIZE, evt_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
    /* 显式设置引脚，不使用 NO_CHANGE */
    ESP_ERROR_CHECK(uart_set_pin(port, tx_pin, rx_pin,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d initialized: TX=GPIO%d, RX=GPIO%d, Baud=%d",
             port, tx_pin, rx_pin, UART_BAUD_RATE);
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " UART0 <-> UART1 Bidirectional Forward");
    ESP_LOGI(TAG, " Baud: %d bps | ESP32-C3", UART_BAUD_RATE);
    ESP_LOGI(TAG, " UART0: TX=GPIO21, RX=GPIO20");
    ESP_LOGI(TAG, " UART1: TX=GPIO1,  RX=GPIO0");
    ESP_LOGI(TAG, "========================================");

    /* 初始化 UART0 - 显式指定引脚 */
    uart_init_port(UART_NUM_0, UART0_TX_PIN, UART0_RX_PIN, &uart0_evt_queue);

    /* 初始化 UART1 */
    uart_init_port(UART_NUM_1, UART1_TX_PIN, UART1_RX_PIN, &uart1_evt_queue);

    static uart_port_t fwd_0_to_1[] = { UART_NUM_0, UART_NUM_1 };
    static uart_port_t fwd_1_to_0[] = { UART_NUM_1, UART_NUM_0 };

    xTaskCreate(uart_forward_task, "uart0_to_1", 4096, fwd_0_to_1,
                configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(uart_forward_task, "uart1_to_0", 4096, fwd_1_to_0,
                configMAX_PRIORITIES - 1, NULL);

    /* 统计监控任务 */
    xTaskCreate(monitor_task, "monitor", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "[READY] Forwarding tasks started!");
}
