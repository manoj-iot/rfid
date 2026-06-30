/**
 * @file lcd1602_i2c.h
 * @brief I2C driver for standard LCD1602 (HD44780 + PCF8574 I2C backpack)
 */

#pragma once

#include "driver/i2c_master.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LCD commands
#define LCD_CLEARDISPLAY 0x01
#define LCD_RETURNHOME 0x02
#define LCD_ENTRYMODESET 0x04
#define LCD_DISPLAYCONTROL 0x08
#define LCD_CURSORSHIFT 0x10
#define LCD_FUNCTIONSET 0x20
#define LCD_SETCGRAMADDR 0x40
#define LCD_SETDDRAMADDR 0x80

// Flags for entry mode set
#define LCD_ENTRYLEFT 0x02
#define LCD_ENTRYSHIFTDECREMENT 0x00

// Flags for display on/off control
#define LCD_DISPLAYON 0x04
#define LCD_CURSOROFF 0x00
#define LCD_BLINKOFF 0x00

// Flags for function set
#define LCD_4BITMODE 0x00
#define LCD_2LINE 0x08
#define LCD_5x8DOTS 0x00

// Control bits for PCF8574
#define RS_BIT 0x01        // P0 (Register Select)
#define RW_BIT 0x02        // P1 (Read/Write)
#define ENABLE_BIT 0x04    // P2 (Enable)
#define LCD_BACKLIGHT 0x08 // P3 (Backlight)

/**
 * @brief Initialize the LCD device on the given I2C master bus handle.
 *
 * @param dev The I2C master device handle for the LCD.
 */
void lcd_init(i2c_master_dev_handle_t dev);

/**
 * @brief Clear the LCD display.
 *
 * @param dev The I2C master device handle.
 */
void lcd_clear(i2c_master_dev_handle_t dev);

/**
 * @brief Set cursor position on the LCD display.
 *
 * @param dev The I2C master device handle.
 * @param col Column index (0-based).
 * @param row Row index (0-based).
 */
void lcd_set_cursor(i2c_master_dev_handle_t dev, uint8_t col, uint8_t row);

/**
 * @brief Display a string on the LCD starting at the current cursor position.
 *
 * @param dev The I2C master device handle.
 * @param str The null-terminated string to print.
 */
void lcd_put_str(i2c_master_dev_handle_t dev, const char *str);

#ifdef __cplusplus
}
#endif
