/**
 * @file pn532.h
 * @brief High-performance PN532 NFC/RFID controller driver using ESP-IDF v6.0 UART.
 */

#pragma once

#include "driver/uart.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// PN532 Commands
#define PN532_COMMAND_GETFIRMWAREVERSION 0x02
#define PN532_COMMAND_SAMCONFIGURATION    0x14
#define PN532_COMMAND_INLISTPASSIVETARGET 0x4A

// Card types for InListPassiveTarget
#define PN532_MIFARE_ISO14443A            0x00

/**
 * @brief Initialize the PN532 driver and UART peripheral.
 * 
 * @param uart_num UART port number (e.g., UART_NUM_1).
 * @param tx_pin GPIO number for ESP32 TX.
 * @param rx_pin GPIO number for ESP32 RX.
 * @return esp_err_t ESP_OK on success, or error code.
 */
esp_err_t pn532_init(uart_port_t uart_num, int tx_pin, int rx_pin);

/**
 * @brief Get the Firmware Version from the PN532 to verify communication.
 * 
 * @param uart_num UART port number.
 * @param version_buf Buffer to store version details (must be at least 4 bytes).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t pn532_get_firmware_version(uart_port_t uart_num, uint32_t *version);

/**
 * @brief Configure the SAM (Secure Access Module) on the PN532.
 * 
 * @param uart_num UART port number.
 * @return esp_err_t ESP_OK on success, or error code.
 */
esp_err_t pn532_sam_config(uart_port_t uart_num);

/**
 * @brief Poll the PN532 for a passive card/tag (Mifare ISO14443A).
 * 
 * @param uart_num UART port number.
 * @param uid Buffer to store the read card's UID (must be at least 7 bytes).
 * @param uid_len Pointer to store the read UID length.
 * @param timeout_ms Maximum time to wait for a tag in milliseconds.
 * @return esp_err_t ESP_OK if a card is successfully read, ESP_ERR_TIMEOUT if no card is found, or error code.
 */
esp_err_t pn532_read_passive_target(uart_port_t uart_num,
                                    uint8_t *uid, uint8_t *uid_len, uint32_t timeout_ms);

/**
 * @brief Configure the maximum number of retry attempts when activating a passive target.
 * 
 * @param uart_num UART port number.
 * @param max_retries Maximum number of retries (0x00 = try once, 0xFF = retry forever).
 * @return esp_err_t ESP_OK on success, or error code.
 */
esp_err_t pn532_set_passive_activation_retries(uart_port_t uart_num, uint8_t max_retries);

#ifdef __cplusplus
}
#endif
