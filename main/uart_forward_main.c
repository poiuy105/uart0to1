/**
 * USB CDC (TinyUSB) <-> UART1 双向转发 (ESP32-C3)
 *
 * USB CDC: COM33 (TinyUSB)
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
#include "tinyusb.h"
#include "tusb_cdc.h"

static const char *TAG = "FWD";

#define UART1_RX_PIN    GPIO_NUM_0
#define UART1_TX_PIN    GPIO_NUM_1
#define DEFAULT_BAUD    256000
#define BUF_SIZE        1024
#define EVT_QUEUE_SIZE  20

static volatile uint32_t fwd_usb_to_uart = 0;
static volatile uint32_t fwd_uart_to_usb = 0;
static volatile uint32_t current_baud = DEFAULT_BAUD;

/**
 * @brief CDC 线路编码变化回调
 */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* coding)
{
    if (coding->bit_rate != 0 && coding->bit_rate != current_baud) {
        ESP_LOGI(TAG, "Baud rate changed: %lu -> %lu",
                 (unsigned long)current_baud, (unsigned long)coding->bit_rate);
        current_baud = coding->bit_rate;
        uart_set_baudrate(UART_NUM_1, current_baud);
    }
}

static void usb_to_uart_task(void *arg)
{
    uint8_t buf[BUF_SIZE];
    for (;;) {
        if (tud_cdc_available()) {
            int len = tud_cdc_read(buf, sizeof(buf));
            if (len > 0) {
                uart_write_bytes(UART_NUM_1, buf, len);
                fwd_usb_to_uart += len;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

static void uart_to_usb_task(void *arg)
{
    uint8_t buf[BUF_SIZE];
    for (;;) {
        int len = uart_read_bytes(UART_NUM_1, buf, BUF_SIZE, pdMS_TO_TICKS(10));
        if (len > 0) {
            if (tud_cdc_connected() && tud_cdc_write_available()) {
                tud_cdc_write(buf, len);
                tud_cdc_write_flush();
                fwd_uart_to_usb += len;
            }
        }
    }
}

static void monitor_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "USB->UART1: %lu | UART1->USB: %lu | Baud: %lu",
                 (unsigned long)fwd_usb_to_uart, (unsigned long)fwd_uart_to_usb,
                 (unsigned long)current_baud);
    }
}

void app_main(void)
{
    /* 初始化 TinyUSB */
    tusb_init();

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

    ESP_LOGI(TAG, "USB CDC<->UART1 forwarding started (Baud: %d, dynamic)", DEFAULT_BAUD);

    xTaskCreate(usb_to_uart_task, "usb2uart", 4096, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(uart_to_usb_task, "uart2usb", 4096, NULL, configMAX_PRIORITIES - 1, NULL);
    xTaskCreate(monitor_task, "monitor", 2048, NULL, 1, NULL);
}
