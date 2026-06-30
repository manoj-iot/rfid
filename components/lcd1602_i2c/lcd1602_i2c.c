/**
 * @file lcd1602_i2c.c
 * @brief Implementation of I2C driver for LCD1602 (HD44780 + PCF8574)
 */

#include "lcd1602_i2c.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void lcd_write_nibble(i2c_master_dev_handle_t dev, uint8_t nibble, uint8_t mode) {
    uint8_t data = (nibble & 0xF0) | mode | LCD_BACKLIGHT;
    uint8_t tx_buf[2];
    tx_buf[0] = data | ENABLE_BIT;
    tx_buf[1] = data & ~ENABLE_BIT;

    // Transmit both bytes in a single I2C transaction to guarantee timing
    i2c_master_transmit(dev, tx_buf, 2, -1);
    esp_rom_delay_us(50);
}

/**
 * @brief Send an 8-bit command or data byte to the LCD.
 */
static void lcd_send(i2c_master_dev_handle_t dev, uint8_t value, uint8_t mode) {
    lcd_write_nibble(dev, value & 0xF0, mode);
    lcd_write_nibble(dev, (value << 4) & 0xF0, mode);
}

/**
 * @brief Send a command instruction to the LCD controller.
 */
static void lcd_write_cmd(i2c_master_dev_handle_t dev, uint8_t cmd) {
    lcd_send(dev, cmd, 0);
}

/**
 * @brief Send character data to the LCD controller to display it.
 */
static void lcd_write_data(i2c_master_dev_handle_t dev, uint8_t data) {
    lcd_send(dev, data, RS_BIT);
}

void lcd_clear(i2c_master_dev_handle_t dev) {
    lcd_write_cmd(dev, LCD_CLEARDISPLAY);
    vTaskDelay(pdMS_TO_TICKS(10)); // Ensure HD44780 finishes clearing the display RAM
}

void lcd_set_cursor(i2c_master_dev_handle_t dev, uint8_t col, uint8_t row) {
    uint8_t row_offsets[] = {0x00, 0x40};
    lcd_write_cmd(dev, LCD_SETDDRAMADDR | (col + row_offsets[row]));
}

void lcd_init(i2c_master_dev_handle_t dev) {
    // Wait for LCD power-up (at least 40ms)
    vTaskDelay(pdMS_TO_TICKS(50));

    // Initialization sequence for 4-bit mode (Standard HD44780 procedure)
    lcd_write_nibble(dev, 0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(5));

    lcd_write_nibble(dev, 0x30, 0);
    esp_rom_delay_us(150);

    lcd_write_nibble(dev, 0x30, 0);
    esp_rom_delay_us(150);

    // Set interface to 4-bit wide
    lcd_write_nibble(dev, 0x20, 0);

    // Configure function set: 4-bit, 2 lines, 5x8 font size
    lcd_write_cmd(dev, LCD_FUNCTIONSET | LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS);
    
    // Turn display on, cursor off, blinking off
    lcd_write_cmd(dev, LCD_DISPLAYCONTROL | LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF);
    
    // Clear screen
    lcd_clear(dev);
    
    // Entry mode set: increment cursor automatically, shift off
    lcd_write_cmd(dev, LCD_ENTRYMODESET | LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT);
}

void lcd_put_str(i2c_master_dev_handle_t dev, const char *str) {
    while (*str) {
        lcd_write_data(dev, *str++);
    }
}
