/**
 * @file hardware_bridge_app.h
 * @brief HardwareBridge – Logic Analyzer, Voltmeter & Signal Generator.
 *
 * Turns the ESP32 into a hardware hacking multi-tool:
 *  - UART/I2C/SPI Sniffer   – intercept bus traffic, stream to Dashboard SSE
 *  - Digital Voltmeter       – ADC-based 0-3.3 V meter with OLED bar graph
 *  - Signal Generator        – configurable PWM / square-wave output
 */

#pragma once

#include "apps/app_base.h"

AppBase *createHardwareBridgeApp();
