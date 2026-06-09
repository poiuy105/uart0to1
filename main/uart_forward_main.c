/**
 * UART0 <-> UART1 双向转发 (ESP32-C3)
 *
 * UART0: 默认引脚 (GPIO20=RX, GPIO21=TX)
 * UART1: RX=GPIO0, TX=GPIO1  (已确认 GPIO0 有数据输入)
 * 波特率: 5,000,000 (5 MBaud)
 *
 * 采用事件驱动模型，双任务并行转发，高优先级运行
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

/* ============ ANSI 彩色输出宏 ============ */
#define ANSI_RESET   "\033[0m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_BOLD    "\033[1m"

/* ============ 引脚定义 (已确认正确) ============ */
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

static void uart_forward_task(void *param)
{
    uart_port_t *ports = (uart_port_t *)param;
    uart_port_t src  = ports[0];
    uart_port_t dest = ports[1];
    QueueHandle_t evt_queue = (src == UART_NUM_0) ? uart0_evt_queue : uart1_evt_queue;

    uart_event_t event;
    uint8_t *buf = (uint8_t *)malloc(RD_BUF_SIZE);
    assert(buf);

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
                            total += len;
                        } else break;
                    }
                }
                break;

            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "UART%d FIFO overflow, flushing", src);
                uart_flush_input(src);
                xQueueReset(evt_queue);
                break;

            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "UART%d ring buffer full, flushing", src);
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
                ESP_LOGW(TAG, "UART%d unhandled event type: %d", src, event.type);
                break;
            }
        }
    }

    free(buf);
    vTaskDelete(NULL);
}

static void uart_init_port(uart_port_t port, int tx_pin, int rx_pin, QueueHandle_t *evt_queue)
{
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
    ESP_ERROR_CHECK(uart_set_pin(port, tx_pin, rx_pin,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d initialized: TX=%d, RX=%d, Baud=%d",
             port, tx_pin, rx_pin, UART_BAUD_RATE);
}

void app_main(void)
{
    printf(ANSI_BOLD ANSI_CYAN "\n"
           "========================================\n"
           " UART0 <-> UART1 Bidirectional Forward\n"
           " Baud Rate: %d bps (5 MBaud)\n"
           " ESP32-C3 (GPIO20/21 <-> GPIO0/1)\n"
           " [RX confirmed on GPIO0]\n"
           "========================================\n"
           ANSI_RESET "\n", UART_BAUD_RATE);

    ESP_LOGI(TAG, "Initializing UART0 and UART1...");

    uart_init_port(UART_NUM_0,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                   &uart0_evt_queue);

    uart_init_port(UART_NUM_1,
                   UART1_TX_PIN, UART1_RX_PIN,
                   &uart1_evt_queue);

    static uart_port_t fwd_0_to_1[] = { UART_NUM_0, UART_NUM_1 };
    static uart_port_t fwd_1_to_0[] = { UART_NUM_1, UART_NUM_0 };

    xTaskCreate(uart_forward_task, "uart0_to_1", 4096, fwd_0_to_1,
                configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(uart_forward_task, "uart1_to_0", 4096, fwd_1_to_0,
                configMAX_PRIORITIES - 1, NULL);

    printf(ANSI_BOLD ANSI_GREEN
           "[READY] Forwarding tasks started!\n"
           "  UART0 (GPIO21/GPIO20) <--> UART1 (GPIO1/GPIO0)\n"
           "  Baud: %d bps\n"
           ANSI_RESET "\n", UART_BAUD_RATE);
}
