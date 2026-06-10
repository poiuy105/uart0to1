/**
 * USB Serial/JTAG (CDC) <-> UART1 双向转发 (ESP32-C3)
 *
 * USB CDC: COM33
 * UART1:   RX=GPIO0, TX=GPIO1
 * 波特率:  动态跟随 USB CDC 主机设置
 *
 * 当用户在串口工具中修改波特率时，UART1 波特率自动同步
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "FWD";

#define UART1_RX_PIN    GPIO_NUM_0
#define UART1_TX_PIN    GPIO_NUM_1
#define DEFAULT_BAUD    256000
#define BUF_SIZE        1024
#define EVT_QUEUE_SIZE  20

static volatile uint32_t fwd_usb_to_uart = 0;
static volatile uint32_t fwd_uart_to_usb = 0;

/**
 * @brief USB CDC 线路编码变化回调
 * 当主机修改波特率时，同步更新 UART1 波特率
 */
static void line_coding_cb(void *arg)
{
    /* 读取当前 USB CDC 的波特率设置 */
    usb_serial_jtag_line_coding_t coding;
    usb_serial_jtag_get_line_coding(&coding);
    ESP_LOGI(TAG, "Baud rate changed: %d -> %d", 
             uart_get_baudrate(UART_NUM_1), coding.bit_rate);
    uart_set_baudrate(UART_NUM_1, coding.bit_rate);
}

static void usb_to_uart_task(void *arg)
{
    uint8_t buf[BUF_SIZE];
    for (;;) {
        int len = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        if (len > 0) {
            uart_write_bytes(UART_NUM_1, buf, len);
            fwd_usb_to_uart += len;
        }
    }
}

static void uart_to_usb_task(void *arg)
{
    uint8_t buf[BUF_SIZE];
    for (;;) {
        int len = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), portMAX_DELAY);
        if (len > 0) {
            usb_serial_jtag_write_bytes(buf, len, portMAX_DELAY);
            fwd_uart_to_usb += len;
        }
    }
}

static void monitor_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "USB->UART1: %lu | UART1->USB: %lu | Baud: %d",
                 (unsigned long)fwd_usb_to_uart, (unsigned long)fwd_uart_to_usb,
                 uart_get_baudrate(UART_NUM_1));
    }
}

void app_main(void)
{
    /* USB Serial/JTAG */
    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 1024,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    /* 注册线路编码变化回调 */
    usb_serial_jtag_register_line_coding_cb(line_coding_cb, NULL);

    /* UART1 */
    uart_config_t uart_cfg = {
        .baud_rate  = DEFAULT_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, BUF_SIZE * 2, BUF_SIZE * 2,
                                         EVT_QUEUE_SIZE, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, UART1_TX_PIN, UART1_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "USB<->UART1 forwarding started (Baud: %d, dynamic)", DEFAULT_BAUD);

    xTaskCreate(usb_to_uart_task, "usb2uart", 4096, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(uart_to_usb_task, "uart2usb", 4096, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(monitor_task, "monitor", 2048, NULL, 1, NULL);
}
