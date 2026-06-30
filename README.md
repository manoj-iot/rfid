# RFID_TEAM_IQ

An ESP-IDF application for ESP32-based access control using an I2C LCD1602 display and a PN532 NFC/RFID reader.

## Features
- **LCD1602 Display Integration:** Shows real-time system status and instructions to the user.
- **PN532 RFID/NFC Tag Reader:** Polls Mifare tags via High-Speed UART (HSU).
- **Automatic Robust Initialization:** Retries PN532 setup up to 5 times on startup before showing a wiring error alert on the LCD.

## Hardware Connections
To run this application, make sure your ESP32 is wired correctly to the components:

### LCD1602 I2C Display
| LCD1602 Pin | ESP32 GPIO |
| ----------- | ---------- |
| VCC         | 5V   |
| GND         | GND        |
| SDA         | GPIO 8     |
| SCL         | GPIO 9     |

### PN532 NFC Module (HSU mode: SW1=OFF, SW2=OFF)
| PN532 Pin | ESP32 GPIO |
| --------- | ---------- |
| VCC       | 5V / 3.3V  |
| GND       | GND        |
| TXD       | GPIO 5 (RX)|
| RXD       | GPIO 4 (TX)|

## Getting Started
1. Set up the Espressif ESP-IDF environment.
2. Build the project:
   ```bash
   idf.py build
   ```
3. Flash and monitor the logs:
   ```bash
   idf.py -p <PORT> flash monitor
   ```
