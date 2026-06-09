/**
 * USB Serial/JTAG (CDC) <-> UART1 双向转发 (ESP32-C3)
 *
 * USB CDC: COM33 (通过 USB 连接电脑)
 * UART1:   RX=GPIO0, TX=GPIO1 (已确认 GPIO0 有数据输入)
 * 波特率:  5,000,000 (5 MBaud)
 *
 * 采用双任务 + 环形缓冲区，高优先级运行
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "USB_UART_FWD";

/* ============ 引脚定义 ============ */
#define UART1_RX_PIN    GPIO_NUM_0
#define UART1_TX_PIN    GPIO_NUM_1

/* ============ 缓冲配置 ============ */
#define UART_BAUD_RATE  5000000
#define BUF_SIZE        4096
#define USB_BUF_SIZE    4096
#define EVT_QUEUE_SIZE  20

/* ============ 转发统计 ============ */
static volatile uint32_t fwd_usb_to_uart_bytes = 0;
static volatile uint32_t fwd_uart_to_usb_bytes = 0;

/**
 * @brief USB -> UART1 转发任务
 * 从 USB Serial/JTAG 读取数据，写入 UART1
 */
static void usb_to_uart_task(void *param)
{
    uint8_t *buf = (uint8_t *)malloc(USB_BUF_SIZE);
    assert(buf);

    ESP_LOGI(TAG, "USB -> UART1 task started");

    for (;;) {
        int len = usb_serial_jtag_read_bytes(buf, USB_BUF_SIZE, pdMS_TO_TICKS(10));
        if (len > 0) {
            uart_write_bytes(UART_NUM_1, buf, len);
            fwd_usb_to_uart_bytes += len;
        }
    }

    free(buf);
    vTaskDelete(NULL);
}

/**
 * @brief UART1 -> USB 转发任务
 * 从 UART1 读取数据，写入 USB Serial/JTAG
 */
static void uart_to_usb_task(void *param)
{
    uint8_t *buf = (uint8_t *)malloc(BUF_SIZE);
    assert(buf);

    ESP_LOGI(TAG, "UART1 -> USB task started");

    for (;;) {
        int len = uart_read_bytes(UART_NUM_1, buf, BUF_SIZE, pdMS_TO_TICKS(10));
        if (len > 0) {
            usb_serial_jtag_write_bytes(buf, len, portMAX_DELAY);
            fwd_uart_to_usb_bytes += len;
        }
    }

    free(buf);
    vTaskDelete(NULL);
}

/**
 * @brief 统计监控任务
 */
static void monitor_task(void *param)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint32_t a = fwd_usb_to_uart_bytes;
        uint32_t b = fwd_uart_to_usb_bytes;
        ESP_LOGI(TAG, "[STATS] USB->UART1: %lu bytes | UART1->USB: %lu bytes",
                 (unsigned long)a, (unsigned long)b);
    }
}

/**
 * @brief 初始化 UART1
 */
static void uart1_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = UART_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, BUF_SIZE * 2, BUF_SIZE * 2,
                                        EVT_QUEUE_SIZE, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, UART1_TX_PIN, UART1_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART1 initialized: TX=GPIO%d, RX=GPIO%d, Baud=%d",
             UART1_TX_PIN, UART1_RX_PIN, UART_BAUD_RATE);
}

/**
 * @brief 初始化 USB Serial/JTAG
 */
static void usb_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = USB_BUF_SIZE,
        .rx_buffer_size = USB_BUF_SIZE,
    };

    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    ESP_LOGI(TAG, "USB Serial/JTAG driver installed");
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " USB Serial/JTAG <-> UART1 Forward");
    ESP_LOGI(TAG, " Baud: %d bps | ESP32-C3", UART_BAUD_RATE);
    ESP_LOGI(TAG, " UART1: TX=GPIO1, RX=GPIO0");
    ESP_LOGI(TAG, " USB:   COM33 (Serial/JTAG CDC)");
    ESP_LOGI(TAG, "========================================");

    /* 初始化 USB Serial/JTAG */
    usb_init();

    /* 初始化 UART1 */
    uart1_init();

    /* 创建转发任务 */
    xTaskCreate(usb_to_uart_task, "usb_to_uart", 4096, NULL,
                configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(uart_to_usb_task, "uart_to_usb", 4096, NULL,
                configMAX_PRIORITIES - 1, NULL);

    /* 统计监控 */
    xTaskCreate(monitor_task, "monitor", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "[READY] Forwarding started!");
}
