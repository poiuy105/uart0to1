/**
 * UART1 RX 引脚探测诊断工具 (ESP32-C3)
 *
 * 交替将 UART1 RX 设为 GPIO0 和 GPIO1，各检测 3 秒，
 * 统计接收字节数，判断哪个引脚有持续的串口数据输入。
 *
 * 输出通过 USB Serial/JTAG 查看。
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "RX_DETECT";

#define DETECT_BAUD_RATE  5000000
#define BUF_SIZE          4096
#define DETECT_DURATION_S 3

/**
 * @brief 在指定 RX 引脚上检测串口数据，返回接收到的字节数
 */
static int detect_rx_on_pin(int rx_pin)
{
    QueueHandle_t evt_queue = NULL;

    /* 先删除可能存在的 UART1 驱动 */
    uart_driver_delete(UART_NUM_1);

    uart_config_t cfg = {
        .baud_rate  = DETECT_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(UART_NUM_1, BUF_SIZE * 2, BUF_SIZE * 2,
                                        20, &evt_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return -1;
    }

    uart_param_config(UART_NUM_1, &cfg);
    /* TX 引脚设为 -1（不使用），只关心 RX */
    uart_set_pin(UART_NUM_1, -1, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "Testing UART1 RX on GPIO%d for %d seconds...", rx_pin, DETECT_DURATION_S);

    int total_bytes = 0;
    uint8_t buf[256];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(DETECT_DURATION_S * 1000);

    while (xTaskGetTickCount() < deadline) {
        int len = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            total_bytes += len;
        }
    }

    uart_driver_delete(UART_NUM_1);

    return total_bytes;
}

void app_main(void)
{
    /* 等待 USB Serial/JTAG 连接就绪 */
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n========================================\n");
    printf("  UART1 RX Pin Detection Tool\n");
    printf("  Baud: %d bps\n", DETECT_BAUD_RATE);
    printf("  Each test: %d seconds\n", DETECT_DURATION_S);
    printf("========================================\n\n");

    /* 第一轮：测试 GPIO0 */
    int gpio0_bytes = detect_rx_on_pin(GPIO_NUM_0);
    printf("[RESULT] GPIO0 as RX: received %d bytes\n\n", gpio0_bytes);

    /* 短暂间隔，让线路稳定 */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 第二轮：测试 GPIO1 */
    int gpio1_bytes = detect_rx_on_pin(GPIO_NUM_1);
    printf("[RESULT] GPIO1 as RX: received %d bytes\n\n", gpio1_bytes);

    /* 输出结论 */
    printf("========================================\n");
    printf("  DETECTION RESULT\n");
    printf("  GPIO0 RX: %d bytes\n", gpio0_bytes);
    printf("  GPIO1 RX: %d bytes\n", gpio1_bytes);
    printf("----------------------------------------\n");

    if (gpio0_bytes > 0 && gpio1_bytes == 0) {
        printf("  >> Data detected on GPIO0\n");
        printf("  >> Set UART1_RX_PIN = GPIO_NUM_0\n");
    } else if (gpio1_bytes > 0 && gpio0_bytes == 0) {
        printf("  >> Data detected on GPIO1\n");
        printf("  >> Set UART1_RX_PIN = GPIO_NUM_1\n");
    } else if (gpio0_bytes > 0 && gpio1_bytes > 0) {
        printf("  >> Data on BOTH pins! (GPIO0: %d, GPIO1: %d)\n", gpio0_bytes, gpio1_bytes);
        printf("  >> GPIO%d has more data\n", gpio0_bytes >= gpio1_bytes ? 0 : 1);
    } else {
        printf("  >> NO data detected on either pin\n");
        printf("  >> Check: wiring, baud rate, and data source\n");
    }
    printf("========================================\n\n");

    /* 再做第二轮确认 */
    printf("--- Running confirmation round ---\n\n");
    int gpio0_r2 = detect_rx_on_pin(GPIO_NUM_0);
    printf("[CONFIRM] GPIO0 as RX: %d bytes\n\n", gpio0_r2);
    vTaskDelay(pdMS_TO_TICKS(500));
    int gpio1_r2 = detect_rx_on_pin(GPIO_NUM_1);
    printf("[CONFIRM] GPIO1 as RX: %d bytes\n\n", gpio1_r2);

    printf("========================================\n");
    printf("  FINAL RESULT (2 rounds averaged)\n");
    printf("  GPIO0: round1=%d  round2=%d  total=%d\n", gpio0_bytes, gpio0_r2, gpio0_bytes + gpio0_r2);
    printf("  GPIO1: round1=%d  round2=%d  total=%d\n", gpio1_bytes, gpio1_r2, gpio1_bytes + gpio1_r2);
    printf("========================================\n");
}
