# RFID Team IQ — Project Documentation

## Project Overview
This project implements a real-time RFID/NFC-based attendance tracking system using:
- **ESP32-S3** microcontroller running custom ESP-IDF firmware
- **PN532 NFC module** to read RFID/NFC card UIDs over I2C/UART
- **Flask web server** (Python) that receives scan data over Wi-Fi HTTP and serves a live dashboard

---

## Folder Structure

RFID_TEAM_IQ/
|-- web/                   # Web application (Flask)
|   |-- templates/
|   |   |-- dashboard.html # Jinja2 HTML template with Edit modal
|   |-- static/
|       |-- css/
|           |-- style.css  # Modern glassmorphism dashboard styles
|-- data/                  # Persistent data storage
|   |-- attendance_db.json # JSON database: users (uid->name) + logs
|-- docs/                  # Documentation
|   |-- README.md          # This file
|-- scripts/               # Helper scripts
|   |-- start_server.bat   # Start the Flask server on Windows
|-- main/                  # ESP-IDF main component directory
|   |-- team_IQ.c          # Main firmware source: NFC scan, Wi-Fi HTTP POST
|   |-- CMakeLists.txt     # ESP-IDF component manifest
|   |-- attendance_server.py # Flask server entry point
|   |-- run_attendance_system.bat # Combined launcher
|-- CMakeLists.txt         # Root ESP-IDF CMake configuration
|-- sdkconfig              # ESP-IDF build configuration

---

## Components Used

### Hardware
| Component      | Details                                        |
|----------------|------------------------------------------------|
| ESP32-S3       | Main microcontroller (Wi-Fi + BLE)             |
| PN532 NFC HAT  | NFC reader module (I2C/HSU interface)          |
| LCD 1602 (I2C) | 16x2 LCD display for local scan feedback       |
| USB-C cable    | Programming + power                            |

### Software / Firmware
| Component      | Details                                        |
|----------------|------------------------------------------------|
| ESP-IDF v6.x   | Official ESP32 development framework           |
| FreeRTOS       | RTOS running on ESP32 (via ESP-IDF)            |
| esp_http_client| ESP-IDF HTTP client for POST requests          |
| esp_wifi       | ESP-IDF Wi-Fi station (STA) mode              |
| Flask (Python) | Lightweight Python web framework               |
| Server-Sent Events (SSE) | Browser push for real-time card scan updates |

---

## How It Works

1. The ESP32 boots, connects to the configured Wi-Fi network (SSID/password in firmware).
2. The PN532 NFC reader scans for RFID cards in a loop.
3. When a card is tapped, its UID is read and sent via HTTP POST to:
   http://<PC_IP>:5000/api/scan
4. The Flask server receives the UID, looks up the student name from data/attendance_db.json,
   logs the scan with a timestamp, and broadcasts it to all open browser tabs via SSE.
5. The browser dashboard updates in real-time — no refresh needed.

---

## Running the System

### 1. Start the Flask Server
`
python main\attendance_server.py
`
Or use the helper script:
`
scripts\start_server.bat
`
Server starts at: http://localhost:5000 (also accessible on LAN at http://<PC_IP>:5000)

### 2. Flash the ESP32 Firmware
From the project root with ESP-IDF environment activated:
`
idf.py build flash monitor
`
Update the Wi-Fi credentials and server IP in main/team_IQ.c before flashing.

---

## API Endpoints

| Method | Endpoint              | Description                          |
|--------|-----------------------|--------------------------------------|
| GET    | /                     | Serve the live dashboard HTML        |
| GET    | /api/logs             | Get all attendance logs (JSON)       |
| GET    | /api/users            | Get all registered users (JSON)      |
| POST   | /api/scan             | Receive RFID scan from ESP32         |
| POST   | /api/register         | Register a new UID-to-name mapping   |
| POST   | /api/edit_student     | Rename an existing student/card      |
| GET    | /api/live-stream      | SSE stream for real-time dashboard   |

---

## Edit Student Feature
Each row in the dashboard has an **Edit** button (amber colour).
Clicking it opens a modal where you can enter a new name for the student.
Submitting the form calls /api/edit_student and updates both the users map
and all past log entries for that UID — effectively re-assigning the card.

---
