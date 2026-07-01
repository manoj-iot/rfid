/**
 * @file team_IQ.c
 * @brief Main orchestration for LCD1602 Display and PN532 NFC/RFID Tag Reader
 * (UART HSU)
 *
 * Startup sequence:
 *   1. LCD "Hi Team IQ" for 2 s
 *   2. PN532 UART init + GetFirmwareVersion (up to 5 retries with re-init each
 * time)
 *   3. SAMConfiguration + RFConfig MaxRetries
 *   4. Loop: poll for Mifare card → show UID on LCD + activate relay for 2 s
 */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

// Custom Components
#include "lcd1602_i2c.h"
#include "pn532.h"

static const char *TAG = "main_app";

/* ─── Background HTTP Post Task ───────────────────────────────────────── */
static void http_post_task(void *pvParameters) {
  char *uid_to_send = (char *)pvParameters;
  if (uid_to_send == NULL) {
    vTaskDelete(NULL);
    return;
  }

  char post_data[64];
  snprintf(post_data, sizeof(post_data), "{\"uid\":\"%s\"}", uid_to_send);
  free(uid_to_send); // Free the memory allocated in strdup

  esp_http_client_config_t config = {
      .url = "http://172.23.12.62:5000/api/scan",
      .timeout_ms = 3000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client != NULL) {
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "Wi-Fi Scan Report Success: Status = %d",
               esp_http_client_get_status_code(client));
    } else {
      ESP_LOGE(TAG, "Wi-Fi Scan Report failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
  }
  vTaskDelete(NULL);
}

static void trigger_http_post(const char *uid) {
  char *uid_copy = strdup(uid);
  if (uid_copy != NULL) {
    // Spawn task with a priority of 5 (above idle, same as default)
    xTaskCreate(http_post_task, "http_post_task", 4096, uid_copy, 5, NULL);
  }
}

/* ─── Hardware pin / address configuration ─────────────────────────────── */
#define I2C_SDA_GPIO 8
#define I2C_SCL_GPIO 9
#define LCD_ADDR 0x27

#define PN532_UART_PORT UART_NUM_1
#define PN532_TX_GPIO 4 // ESP32 TX → PN532 RXD // rx 532
#define PN532_RX_GPIO 5 // ESP32 RX → PN532 TXD //tx 532

#define BUZZER_GPIO 12
#define BUZZER_LEDC_SPEED_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_0
#define BUZZER_LEDC_TIMER LEDC_TIMER_0

static void init_buzzer_pwm(void) {
  ledc_timer_config_t ledc_timer = {
      .speed_mode = BUZZER_LEDC_SPEED_MODE,
      .timer_num = BUZZER_LEDC_TIMER,
      .duty_resolution = LEDC_TIMER_13_BIT, // 13-bit duty resolution
      .freq_hz = 2700,                      // 2.7 kHz beep tone
      .clk_cfg = LEDC_AUTO_CLK};
  ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

  ledc_channel_config_t ledc_channel = {.speed_mode = BUZZER_LEDC_SPEED_MODE,
                                        .channel = BUZZER_LEDC_CHANNEL,
                                        .timer_sel = BUZZER_LEDC_TIMER,
                                        .intr_type = LEDC_INTR_DISABLE,
                                        .gpio_num = BUZZER_GPIO,
                                        .duty = 0,
                                        .hpoint = 0};
  ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

static void play_buzzer_beep(uint32_t duration_ms) {
  // 50% duty cycle is 4096 out of 8192 (13-bit)
  ESP_ERROR_CHECK(
      ledc_set_duty(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_CHANNEL, 4096));
  ESP_ERROR_CHECK(
      ledc_update_duty(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_CHANNEL));
  vTaskDelay(pdMS_TO_TICKS(duration_ms));
  ESP_ERROR_CHECK(
      ledc_set_duty(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_CHANNEL, 0));
  ESP_ERROR_CHECK(
      ledc_update_duty(BUZZER_LEDC_SPEED_MODE, BUZZER_LEDC_CHANNEL));
}

/* ─── PN532 init retry config ─────────────────────────────────────────── */
#define PN532_MAX_INIT_ATTEMPTS                                                \
  5 // Number of full re-init attempts before giving up

/* ─── Device handles ───────────────────────────────────────────────────── */
static i2c_master_dev_handle_t s_lcd_device = NULL;

/* ─── Wi-Fi & NTP / Time configuration ─────────────────────────────────── */
#define WIFI_SSID "SSID NAME"  // CHANGE THIS PARAMETER BEFORE FLASH
#define WIFI_PASS "WIFI PASSWORD" // CAHNGE THIS PARAMETER BEFORE FLASH
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *disconn =
        (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGW(TAG, "Disconnected from AP. Reason code: %d", disconn->reason);
    esp_wifi_connect(); // Infinite auto-retry
    ESP_LOGI(TAG, "Retrying to connect to SSID: %s", WIFI_SSID);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static void wifi_init_sta(void) {
  s_wifi_event_group = xEventGroupCreate();

  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASS,
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .scan_method = WIFI_ALL_CHANNEL_SCAN,
              .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
              .threshold.rssi = -127,
          },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_set_max_tx_power(76); // Set stable high power (19 dBm)
  esp_wifi_set_ps(WIFI_PS_NONE); // Disable power save for fast response
  ESP_LOGI(TAG, "Wi-Fi initialization finished.");
}

/* ─── I2C / LCD init ───────────────────────────────────────────────────── */
static void init_i2c_bus_for_lcd(void) {
  ESP_LOGI(TAG, "Initializing I2C bus (SDA=%d SCL=%d)", I2C_SDA_GPIO,
           I2C_SCL_GPIO);

  i2c_master_bus_config_t i2c_cfg = {
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .i2c_port = I2C_NUM_0,
      .scl_io_num = I2C_SCL_GPIO,
      .sda_io_num = I2C_SDA_GPIO,
      .flags.enable_internal_pullup = true,
  };

  i2c_master_bus_handle_t bus;
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &bus));

  i2c_device_config_t lcd_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = LCD_ADDR,
      .scl_speed_hz = 100000,
  };
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &lcd_cfg, &s_lcd_device));
  ESP_LOGI(TAG, "LCD registered at 0x%02X", LCD_ADDR);
}

/* ─── app_main ─────────────────────────────────────────────────────────── */
void app_main(void) {
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "   Team IQ — NFC Access Controller");
  ESP_LOGI(TAG, "========================================");

  /* ── Buzzer & LCD init ─────────────────────────────────────────────── */
  init_buzzer_pwm();
  init_i2c_bus_for_lcd();
  lcd_init(s_lcd_device);
  vTaskDelay(
      pdMS_TO_TICKS(100)); // Allow LCD to fully settle after initialization

  lcd_clear(s_lcd_device);
  vTaskDelay(pdMS_TO_TICKS(50)); // Wait for clear command to fully process
  lcd_put_str(s_lcd_device, "Hi Team IQ!");
  ESP_LOGI(TAG, "LCD ready");
  vTaskDelay(pdMS_TO_TICKS(2000));

  // Initialize TCP/IP and default event loop
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Initialize NVS (required for Wi-Fi storage)
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_err);

  // Initialize SNTP client using hostnames
  ESP_LOGI(TAG, "Initializing SNTP");
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  esp_netif_sntp_init(&config);
  esp_sntp_setservername(1, "time.google.com");
  esp_sntp_setservername(2, "time.windows.com");
  setenv("TZ", "IST-5:30", 1);
  tzset();

  // Show "Connecting WiFi..." on the LCD
  lcd_clear(s_lcd_device);
  vTaskDelay(pdMS_TO_TICKS(50));
  lcd_put_str(s_lcd_device, "Connecting WiFi");
  lcd_set_cursor(s_lcd_device, 0, 1);
  lcd_put_str(s_lcd_device, WIFI_SSID);

  // Start Wi-Fi station
  wifi_init_sta();

  // Wait for Wi-Fi connection with a 120-second timeout
  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE,
                          pdFALSE, pdMS_TO_TICKS(120000));

  // Re-initialize LCD now to recover from any Wi-Fi startup power fluctuations
  lcd_init(s_lcd_device);

  if (bits & WIFI_CONNECTED_BIT) {
    lcd_clear(s_lcd_device);
    lcd_put_str(s_lcd_device, "WiFi Connected");
    lcd_set_cursor(s_lcd_device, 0, 1);
    lcd_put_str(s_lcd_device, "Syncing Time...");

    // NTP will sync in the background automatically if port 123 is open.
    // We proceed immediately to avoid blocking device startup on mobile
    // networks.
    lcd_clear(s_lcd_device);
    lcd_put_str(s_lcd_device, "WiFi Connected");
    lcd_set_cursor(s_lcd_device, 0, 1);
    lcd_put_str(s_lcd_device, "System Ready");
  } else {
    lcd_clear(s_lcd_device);
    lcd_put_str(s_lcd_device, "WiFi Connection");
    lcd_set_cursor(s_lcd_device, 0, 1);
    lcd_put_str(s_lcd_device, "Failed!");
  }
  vTaskDelay(pdMS_TO_TICKS(2000));

  /* ── PN532 init — retry loop ─────────────────────────────────────── */
  esp_err_t err = ESP_FAIL;
  uint32_t fw_version = 0;
  bool pn532_ready = false;

  for (int attempt = 1; attempt <= PN532_MAX_INIT_ATTEMPTS; attempt++) {
    ESP_LOGI(TAG, "PN532 init attempt %d/%d", attempt, PN532_MAX_INIT_ATTEMPTS);

    // Update LCD so the user can see progress
    char lcd_msg[17];
    snprintf(lcd_msg, sizeof(lcd_msg), "PN532 init %d/%d", attempt,
             PN532_MAX_INIT_ATTEMPTS);
    lcd_clear(s_lcd_device);
    lcd_put_str(s_lcd_device, lcd_msg);

    // (Re-)initialise UART + wakeup the PN532
    // uart_driver_install tolerates being called again (returns
    // ESP_ERR_INVALID_STATE if already installed — pn532_init handles this
    // gracefully).
    err = pn532_init(PN532_UART_PORT, PN532_TX_GPIO, PN532_RX_GPIO);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "pn532_init failed: 0x%X — retrying...", err);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // GetFirmwareVersion is the cheapest end-to-end communication test
    fw_version = 0;
    err = pn532_get_firmware_version(PN532_UART_PORT, &fw_version);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "GetFirmwareVersion failed (0x%X) — retrying...", err);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    ESP_LOGI(TAG, "PN532 detected! IC=0x%02X Ver=%d Rev=%d",
             (uint8_t)(fw_version >> 24), (uint8_t)(fw_version >> 16),
             (uint8_t)(fw_version >> 8));

    // Configure the SAM (mandatory before card polling)
    err = pn532_sam_config(PN532_UART_PORT);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "SAMConfig failed (0x%X) — retrying...", err);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // Set retry limit: 0xFF = retry forever until a card appears
    // (pn532_read_passive_target already applies its own timeout via
    // read_response)
    err = pn532_set_passive_activation_retries(PN532_UART_PORT, 0xFF);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "RFConfig retries failed (0x%X) — continuing anyway", err);
      // Non-fatal — default behaviour is still usable
    }

    pn532_ready = true;
    break; // Success — exit the retry loop
  }

  /* ── Handle permanent PN532 failure ─────────────────────────────── */
  if (!pn532_ready) {
    ESP_LOGE(TAG, "PN532 did not respond after %d attempts.",
             PN532_MAX_INIT_ATTEMPTS);
    ESP_LOGE(TAG, "Check: DIP SW1=OFF SW2=OFF, TX->RXD, RX->TXD, GND shared, "
                  "3.3V power.");
    lcd_clear(s_lcd_device);
    lcd_put_str(s_lcd_device, "PN532 FAIL");
    lcd_set_cursor(s_lcd_device, 0, 1);
    lcd_put_str(s_lcd_device, "Check wiring!");
    while (1)
      vTaskDelay(pdMS_TO_TICKS(1000));
  }

  /* ── Ready ─────────────────────────────────────────────────────── */
  lcd_clear(s_lcd_device);
  lcd_put_str(s_lcd_device, "PN532 Connected");
  ESP_LOGI(TAG, "PN532 connected and communication verified.");
  vTaskDelay(pdMS_TO_TICKS(2000));

  lcd_clear(s_lcd_device);
  lcd_put_str(s_lcd_device, "Scan your card");
  ESP_LOGI(TAG, "Ready — waiting for card...");

  /* ── Main polling loop ─────────────────────────────────────────── */
  uint8_t uid[8] = {0};
  uint8_t uid_len = 0;

  while (1) {
    err = pn532_read_passive_target(PN532_UART_PORT, uid, &uid_len, 3000);

    if (err == ESP_OK) {
      /* ── Card detected ────────────────────────────────────── */
      char uid_str[32] = {0};
      char *ptr = uid_str;
      for (int i = 0; i < uid_len; i++)
        ptr += sprintf(ptr, "%02X ", uid[i]);
      // Remove trailing space
      if (uid_len > 0 && uid_str[strlen(uid_str) - 1] == ' ')
        uid_str[strlen(uid_str) - 1] = '\0';

      ESP_LOGI(TAG, ">>> Card detected! UID: %s <<<", uid_str);

      // Sound the buzzer beep
      play_buzzer_beep(150);

      // Report scan to laptop server asynchronously over Wi-Fi
      trigger_http_post(uid_str);

      // Get local time when the card is detected
      time_t now;
      struct tm timeinfo;
      time(&now);
      localtime_r(&now, &timeinfo);
      // LCD: line 0 = "Card Detected!", line 1 = UID
      lcd_clear(s_lcd_device);
      lcd_put_str(s_lcd_device, "Card Detected!");
      lcd_set_cursor(s_lcd_device, 0, 1);
      lcd_put_str(s_lcd_device, uid_str);

      // Wait 2 seconds showing the UID
      vTaskDelay(pdMS_TO_TICKS(2000));

      if (timeinfo.tm_year > (2020 - 1900)) {
        char time_str[17];
        strftime(time_str, sizeof(time_str), "%d-%b %H:%M:%S", &timeinfo);

        // LCD: line 0 = "Card Detected!", line 1 = formatted time
        lcd_clear(s_lcd_device);
        lcd_put_str(s_lcd_device, "Card Detected!");
        lcd_set_cursor(s_lcd_device, 0, 1);
        lcd_put_str(s_lcd_device, time_str);

        // Wait another 2 seconds showing the time
        vTaskDelay(pdMS_TO_TICKS(2000));
      } else {
        // Just show the UID screen for another 2 seconds
        vTaskDelay(pdMS_TO_TICKS(2000));
      }

      // Restore scan prompt
      lcd_clear(s_lcd_device);
      lcd_put_str(s_lcd_device, "Scan your card");

    } else if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND) {
      // Normal: no card in field — just poll again
      vTaskDelay(pdMS_TO_TICKS(100));

    } else {
      // Unexpected UART/protocol error
      ESP_LOGW(TAG, "pn532_read_passive_target error 0x%X — will retry", err);
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
}
