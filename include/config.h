/**
 * @file config.h
 * @brief Central hardware pin definitions for HackOS.
 *
 * All GPIO assignments are declared as compile-time constants using
 * `constexpr uint8_t` so the compiler can fold them into immediates
 * and catch type mismatches early.  Modify only this file when
 * porting HackOS to a different ESP32 board variant.
 *
 * @note Targeted board: ESP32 DevKit v1 (240 MHz, 4 MB Flash)
 */

#pragma once

#include <cstdint>

// ── OLED display (I2C) ────────────────────────────────────────────────────────

/// @brief I2C data line shared by the OLED display.
constexpr uint8_t PIN_OLED_SDA = 21U;

/// @brief I2C clock line shared by the OLED display.
constexpr uint8_t PIN_OLED_SCL = 22U;

// ── Joystick ──────────────────────────────────────────────────────────────────

/// @brief ADC input for the joystick X-axis (horizontal).
constexpr uint8_t PIN_JOY_X = 34U;

/// @brief ADC input for the joystick Y-axis (vertical).
constexpr uint8_t PIN_JOY_Y = 35U;

/// @brief Digital input for the joystick push-button switch.
constexpr uint8_t PIN_JOY_SW = 32U;

// ── SD card (SPI) ─────────────────────────────────────────────────────────────

/// @brief SPI chip-select for the SD card module.
constexpr uint8_t PIN_SD_CS = 5U;

// ── NFC module – PN532 (SPI) ─────────────────────────────────────────────────

/// @brief SPI chip-select for the PN532 NFC controller.
constexpr uint8_t PIN_NFC_CS = 17U;

// ── Shared SPI bus (VSPI) ─────────────────────────────────────────────────────
/// These are the default ESP32 VSPI hardware SPI pins shared by all SPI devices.

/// @brief SPI clock line (VSPI SCK).
constexpr uint8_t PIN_SPI_SCK = 18U;

/// @brief SPI MISO line (VSPI MISO).
constexpr uint8_t PIN_SPI_MISO = 19U;

/// @brief SPI MOSI line (VSPI MOSI).
constexpr uint8_t PIN_SPI_MOSI = 23U;

// ── IR transceiver ────────────────────────────────────────────────────────────

/// @brief UART TX / IR LED drive pin for IR transmission.
constexpr uint8_t PIN_IR_TX = 4U;

/// @brief UART RX / IR receiver pin for IR reception.
constexpr uint8_t PIN_IR_RX = 15U;

// ── RF transceiver ────────────────────────────────────────────────────────────

/// @brief UART TX pin for the RF module.
constexpr uint8_t PIN_RF_TX = 25U;

/// @brief UART RX pin for the RF module.
constexpr uint8_t PIN_RF_RX = 16U;

// ── Buzzer / speaker ──────────────────────────────────────────────────────────

/// @brief PWM-capable pin driving a passive buzzer for audio feedback.
constexpr uint8_t PIN_BUZZER = 27U;

// ── Hardware Bridge (Logic Analyzer / Signal Generator) ──────────────────────

/// @brief UART RX sniff pin for the Hardware Bridge sniffer.
constexpr uint8_t PIN_HB_UART_RX = 26U;

/// @brief UART TX sniff pin for the Hardware Bridge sniffer.
constexpr uint8_t PIN_HB_UART_TX = 33U;

/// @brief I2C data pin for the Hardware Bridge I2C sniffer.
constexpr uint8_t PIN_HB_SDA = 13U;

/// @brief I2C clock pin for the Hardware Bridge I2C sniffer.
constexpr uint8_t PIN_HB_SCL = 14U;

/// @brief ADC input pin for the Hardware Bridge voltmeter (input-only).
constexpr uint8_t PIN_HB_ADC = 36U;

/// @brief PWM output pin for the Hardware Bridge signal generator.
constexpr uint8_t PIN_HB_SIGGEN = 33U;
