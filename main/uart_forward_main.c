/**
 * USB Serial/JTAG <-> UART1 逐字符转发 (ESP32-C3)
 *
 * 不关心波特率，收到1个字符立即转发1个字符
 * 最小延迟，最简逻辑
 */

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

#define BUF_SIZE        64
#define EVT_QUEUE_SIZE  20

static volatile uint32_t fwd_usb_to_uart = 0;
static volatile uint32_t fwd_uart_to_usb = 0;

static void usb_to_uart_task(void *arg)
{
    uint8_t ch;
    for (;;) {
        int len = usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY);
        if (len == 1) {
            uart_write_bytes(UART_NUM_1, &ch, 1);
            fwd_usb_to_uart++;
        }
    }
}

static void uart_to_usb_task(void *arg)
{
    uint8_t ch;
    for (;;) {
        int len = uart_read_bytes(UART_NUM_1, &ch, 1, portMAX_DELAY);
        if (len == 1) {
            usb_serial_jtag_write_bytes(&ch, 1, portMAX_DELAY);
            fwd_uart_to_usb++;
        }
    }
}

static void monitor_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "USB->UART1: %lu | UART1->USB: %lu",
                 (unsigned long)fwd_usb_to_uart, (unsigned long)fwd_uart_to_usb);
    }
}

void app_main(void)
{
    /* USB Serial/JTAG 驱动已由 console 子系统安装，无需重复安装 */

    uart_config_t uart_cfg = {
        .baud_rate  = 256000,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, BUF_SIZE, BUF_SIZE,
                                         EVT_QUEUE_SIZE, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, UART1_TX_PIN, UART1_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "USB<->UART1 char-by-char forwarding started");

    xTaskCreate(usb_to_uart_task, "usb2uart", 2048, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(uart_to_usb_task, "uart2usb", 2048, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(monitor_task, "monitor", 2048, NULL, 1, NULL);
}
