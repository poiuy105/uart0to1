cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS main)

# UART0 <-> UART1 Mutual Forwarding for ESP32-C3
# Max baud rate: 5 MBaud
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(uart0_uart1_forward)
