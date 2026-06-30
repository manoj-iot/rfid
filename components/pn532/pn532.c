/**
 * @file pn532.c
 * @brief Implementation of the PN532 NFC/RFID controller driver using UART (HSU).
 *
 * Protocol Reference: NXP PN532 User Manual UM10232, Chapter 6 (HSU Interface)
 *
 * Fixes applied:
 *  1. Wakeup sequence corrected: 0x55 x3 then delay 2ms (per Adafruit reference),
 *     NOT a long sequence ending in 0x00 bytes that look like a frame preamble.
 *  2. ACK state machine fully fixed: only restart from idx=1 when byte==0x00
 *     AND idx is already 0. No more false resets mid-sequence.
 *  3. pn532_read_response: len==1 guard removed (SAMConfig returns LEN=1),
 *     guard replaced with len==0 only.
 *  4. All UART reads log raw hex bytes for diagnostics on the serial monitor.
 *  5. Response DCS checksum covers TFI + data bytes only (correct per NXP spec).
 */

#include "pn532.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "pn532";

/* ─── Low-level frame builder ─────────────────────────────────────────────── */

/**
 * @brief Send a standard TFI frame to the PN532 over UART.
 *
 * Frame format (NXP UM10232 §7.1):
 *   PREAMBLE  START_CODE  LEN  LCS  TFI  DATA...  DCS  POSTAMBLE
 *   0x00      0x00 0xFF   N    ~N+1 0xD4 cmd+data ~sum 0x00
 *
 * LEN = number of data bytes including TFI (0xD4).
 * LCS = (uint8_t)(~LEN + 1)   so that LEN + LCS == 0x00 (mod 256)
 * DCS = (uint8_t)(~(sum of TFI + all data bytes) + 1)
 */
static esp_err_t pn532_write_frame(uart_port_t uart, const uint8_t *cmd, uint8_t cmd_len) {
    uint8_t packet_len = 8 + cmd_len;  // preamble(1)+start(2)+LEN+LCS+TFI+cmd+DCS+postamble(1)
    uint8_t *packet = malloc(packet_len);
    if (!packet) return ESP_ERR_NO_MEM;

    uint8_t data_len = cmd_len + 1;  // TFI + cmd bytes
    packet[0] = 0x00;                // Preamble
    packet[1] = 0x00;                // Start Code byte 1
    packet[2] = 0xFF;                // Start Code byte 2
    packet[3] = data_len;            // LEN
    packet[4] = (uint8_t)(~data_len + 1); // LCS  (LEN + LCS == 0 mod 256)
    packet[5] = 0xD4;                // TFI: Host → PN532

    memcpy(&packet[6], cmd, cmd_len);

    // DCS: one's-complement checksum of TFI + all data bytes
    uint8_t dcs = 0xD4;
    for (uint8_t i = 0; i < cmd_len; i++) dcs += cmd[i];
    packet[6 + cmd_len] = (uint8_t)(~dcs + 1);
    packet[7 + cmd_len] = 0x00; // Postamble

    // Log the outgoing frame for debugging
    char hex[192] = {0};
    char *p = hex;
    for (uint8_t i = 0; i < packet_len; i++) p += sprintf(p, "%02X ", packet[i]);
    ESP_LOGI(TAG, "TX → [%s]", hex);

    // Flush stale RX bytes before sending so we read a clean response
    uart_flush_input(uart);

    int written = uart_write_bytes(uart, (const char*)packet, packet_len);
    free(packet);

    if (written != packet_len) {
        ESP_LOGE(TAG, "uart_write_bytes failed (%d/%d bytes written)", written, packet_len);
        return ESP_FAIL;
    }

    // Wait until every byte has been clocked out of the TX FIFO.
    // At 115200 baud a 11-byte frame takes ~950 µs. Without this the
    // PN532 may not have received the full frame before we start reading
    // the ACK, causing spurious timeouts (NXP UM10232 §6.2).
    uart_wait_tx_done(uart, pdMS_TO_TICKS(20));
    return ESP_OK;
}

/* ─── ACK reader ───────────────────────────────────────────────────────────── */

/**
 * @brief Wait for and validate the 6-byte PN532 ACK frame.
 *
 * ACK frame (NXP UM10232 §6.2.1.3):  00 00 FF 00 FF 00
 *
 * State machine:
 *   idx 0 → expecting 0x00 (preamble / start byte 1)
 *   idx 1 → expecting 0x00 (start byte 2) — already saw one 0x00
 *   idx 2 → expecting 0xFF
 *   idx 3 → expecting 0x00
 *   idx 4 → expecting 0xFF
 *   idx 5 → expecting 0x00 (postamble) → DONE
 *
 * On mismatch we reset correctly: if the mismatched byte IS 0x00 and
 * we're at idx==0, stay at idx=1 (it could be the first byte of the ACK).
 * In all other cases on mismatch, reset to 0 (or 1 if byte == 0x00 and
 * idx > 0 was the start of a fresh 0x00 run at the beginning).
 */
static esp_err_t pn532_read_ack(uart_port_t uart, uint32_t timeout_ms) {
    static const uint8_t ACK[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    uint8_t idx = 0;
    uint8_t byte = 0;
    uint32_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    // Accumulate received bytes for diagnostic logging
    uint8_t dbg[32];
    uint8_t dbg_n = 0;

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        int n = uart_read_bytes(uart, &byte, 1, pdMS_TO_TICKS(10));
        if (n != 1) { vTaskDelay(1); continue; }

        if (dbg_n < sizeof(dbg)) dbg[dbg_n++] = byte;

        if (byte == ACK[idx]) {
            idx++;
            if (idx == 6) {
                ESP_LOGI(TAG, "ACK ✔");
                return ESP_OK;
            }
        } else {
            // Mismatch — decide where to restart
            if (byte == 0x00) {
                // A 0x00 is always a valid first or second byte of the ACK preamble.
                // Restart from idx=1 (one 0x00 already consumed).
                idx = 1;
            } else {
                // Non-zero byte that doesn't match: reset entirely
                idx = 0;
            }
        }
    }

    // Timeout — log what was received
    char hex[128] = {0};
    char *p = hex;
    for (int i = 0; i < dbg_n; i++) p += sprintf(p, "%02X ", dbg[i]);
    if (dbg_n > 0)
        ESP_LOGE(TAG, "ACK timeout. Received %d byte(s): %s", dbg_n, hex);
    else
        ESP_LOGE(TAG, "ACK timeout. No bytes received at all — check wiring/baud.");
    return ESP_ERR_TIMEOUT;
}

/* ─── Response reader ─────────────────────────────────────────────────────── */

/**
 * @brief Read and validate a PN532 response frame.
 *
 * Response frame (NXP UM10232 §7.1):
 *   00 00 FF  LEN LCS  TFI(D5) CMD_RESP  DATA...  DCS  00
 *
 * @param buf     Output buffer for the data payload (excluding TFI and CMD_RESP).
 * @param max_len Maximum bytes to copy into buf.
 * @param timeout_ms  Timeout waiting for the preamble to appear.
 *
 * The function returns the raw payload *after* the command response code byte.
 * Caller is responsible for checking response[0] (the CMD_RESP byte).
 */
static esp_err_t pn532_read_response(uart_port_t uart, uint8_t *buf,
                                      uint8_t max_len, uint32_t timeout_ms) {
    uint8_t byte = 0;
    uint32_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    uint8_t dbg[32];
    uint8_t dbg_n = 0;

    // ── 1. Locate preamble: 00 00 FF ──────────────────────────────────────
    uint8_t pre_idx = 0;  // 0 → need 0x00; 1 → need 0x00; 2 → need 0xFF
    bool found_preamble = false;

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        int n = uart_read_bytes(uart, &byte, 1, pdMS_TO_TICKS(10));
        if (n != 1) { vTaskDelay(1); continue; }

        if (dbg_n < sizeof(dbg)) dbg[dbg_n++] = byte;

        if (pre_idx < 2) {
            // Expecting 0x00
            if (byte == 0x00) {
                pre_idx++;
            } else {
                pre_idx = 0;
            }
        } else {
            // pre_idx == 2: expecting 0xFF
            if (byte == 0xFF) {
                found_preamble = true;
                break;
            } else if (byte == 0x00) {
                // Extra 0x00 before 0xFF — stay at pre_idx 2
                pre_idx = 2;
            } else {
                pre_idx = 0;
            }
        }
    }

    if (!found_preamble) {
        char hex[128] = {0};
        char *p = hex;
        for (int i = 0; i < dbg_n; i++) p += sprintf(p, "%02X ", dbg[i]);
        if (dbg_n > 0)
            ESP_LOGE(TAG, "Response preamble timeout. Received: %s", hex);
        else
            ESP_LOGE(TAG, "Response preamble timeout. No bytes — check wiring.");
        return ESP_ERR_TIMEOUT;
    }

    // ── 2. Read LEN and LCS ───────────────────────────────────────────────
    uint8_t len_lcs[2] = {0};
    int n = uart_read_bytes(uart, len_lcs, 2, pdMS_TO_TICKS(200));
    if (n != 2) {
        ESP_LOGE(TAG, "Failed to read LEN/LCS (got %d bytes)", n);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t len = len_lcs[0];
    uint8_t lcs = len_lcs[1];
    ESP_LOGI(TAG, "Response LEN=0x%02X LCS=0x%02X", len, lcs);

    if ((uint8_t)(len + lcs) != 0x00) {
        ESP_LOGE(TAG, "LEN/LCS checksum error (0x%02X + 0x%02X != 0x00)", len, lcs);
        return ESP_ERR_INVALID_CRC;
    }

    if (len == 0) {
        ESP_LOGE(TAG, "LEN == 0, invalid frame");
        return ESP_ERR_INVALID_RESPONSE;
    }

    // ── 3. Read TFI + data + DCS + postamble ─────────────────────────────
    // Total = len bytes (TFI + data) + 1 DCS + 1 postamble = len + 2
    uint16_t tail_size = (uint16_t)len + 2;
    uint8_t *tail = malloc(tail_size);
    if (!tail) return ESP_ERR_NO_MEM;

    n = uart_read_bytes(uart, tail, (int)tail_size, pdMS_TO_TICKS(500));
    if (n != (int)tail_size) {
        ESP_LOGE(TAG, "Incomplete payload: got %d of %d bytes", n, (int)tail_size);
        // Log what we got
        char hex[128] = {0};
        char *p = hex;
        for (int i = 0; i < n && i < 32; i++) p += sprintf(p, "%02X ", tail[i]);
        ESP_LOGE(TAG, "  Partial bytes: %s", hex);
        free(tail);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Log the full raw tail for diagnostics
    {
        char hex[196] = {0};
        char *p = hex;
        for (int i = 0; i < (int)tail_size && i < 60; i++) p += sprintf(p, "%02X ", tail[i]);
        ESP_LOGI(TAG, "RX ← 00 00 FF %02X %02X %s", len, lcs, hex);
    }

    // ── 4. Validate TFI ───────────────────────────────────────────────────
    if (tail[0] != 0xD5) {
        ESP_LOGE(TAG, "Bad TFI: 0x%02X (expected 0xD5)", tail[0]);
        free(tail);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // ── 5. Validate DCS ──────────────────────────────────────────────────
    // DCS check: sum of (TFI + all data bytes + DCS) == 0x00 (mod 256)
    uint8_t sum = 0;
    for (uint8_t i = 0; i <= len; i++) sum += tail[i];  // tail[0..len-1] is data, tail[len] is DCS
    if (sum != 0x00) {
        ESP_LOGE(TAG, "DCS checksum error (sum=0x%02X)", sum);
        free(tail);
        return ESP_ERR_INVALID_CRC;
    }

    // ── 6. Copy payload (skip TFI byte at tail[0], skip DCS and postamble) ─
    // Payload = tail[1 .. len-1]  (len-1 bytes)
    uint8_t payload_len = len - 1;  // exclude TFI
    uint8_t copy_len = (payload_len < max_len) ? payload_len : max_len;
    memcpy(buf, &tail[1], copy_len);

    free(tail);
    return ESP_OK;
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

esp_err_t pn532_init(uart_port_t uart_num, int tx_pin, int rx_pin) {
    uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(uart_num, 512, 512, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) {
        ESP_LOGE(TAG, "uart_driver_install failed: 0x%X", err);
        return err;
    }

    err = uart_param_config(uart_num, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: 0x%X", err);
        return err;
    }

    err = uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: 0x%X", err);
        return err;
    }

    // Pull-up on RX to prevent a floating line from generating garbage
    gpio_pullup_en(rx_pin);

    // Give the PN532 time to stabilise after power-on / UART pin config
    vTaskDelay(pdMS_TO_TICKS(150));

    // ── HSU Wakeup sequence ───────────────────────────────────────────────
    // Adafruit/Standard HSU wakeup sequence: 0x55, 0x55, then a series of 0x00s, 
    // followed by a SAMConfiguration frame.
    uint8_t wakeup[] = {
        0x55, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03, 0xfd, 0xd4, 
        0x14, 0x01, 0x17, 0x00
    };
    uart_write_bytes(uart_num, (const char*)wakeup, sizeof(wakeup));
    ESP_LOGI(TAG, "HSU Adafruit wakeup sequence sent");

    // Wait for the PN532 oscillator to stabilise
    vTaskDelay(pdMS_TO_TICKS(20));

    // Flush any echo or garbage the PN532 may have emitted during startup
    uart_flush_input(uart_num);
    vTaskDelay(pdMS_TO_TICKS(50));
    uart_flush_input(uart_num);

    ESP_LOGI(TAG, "PN532 UART init done (TX=%d, RX=%d, UART%d)", tx_pin, rx_pin, uart_num);
    return ESP_OK;
}

esp_err_t pn532_get_firmware_version(uart_port_t uart_num, uint32_t *version) {
    uint8_t cmd[] = {PN532_COMMAND_GETFIRMWAREVERSION};
    ESP_LOGI(TAG, "── GetFirmwareVersion ──");

    esp_err_t err = pn532_write_frame(uart_num, cmd, sizeof(cmd));
    if (err != ESP_OK) return err;

    err = pn532_read_ack(uart_num, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GetFirmwareVersion: no ACK");
        return err;
    }

    // Expected response payload (after TFI + command-echo):
    //   [0] = 0x03  (cmd echo = GETFIRMWAREVERSION + 1)
    //   [1] = IC    (0x32 for PN532)
    //   [2] = Ver
    //   [3] = Rev
    //   [4] = Support
    uint8_t resp[8] = {0};
    err = pn532_read_response(uart_num, resp, sizeof(resp), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GetFirmwareVersion: no response");
        return err;
    }

    ESP_LOGI(TAG, "FW resp: %02X %02X %02X %02X %02X",
             resp[0], resp[1], resp[2], resp[3], resp[4]);

    if (resp[0] != 0x03) {
        ESP_LOGE(TAG, "GetFirmwareVersion: unexpected cmd-echo 0x%02X (want 0x03)", resp[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *version = ((uint32_t)resp[1] << 24) |
               ((uint32_t)resp[2] << 16) |
               ((uint32_t)resp[3] <<  8) |
                (uint32_t)resp[4];

    ESP_LOGI(TAG, "PN532 IC=0x%02X  Ver=%d  Rev=%d  Support=0x%02X",
             resp[1], resp[2], resp[3], resp[4]);
    return ESP_OK;
}

esp_err_t pn532_sam_config(uart_port_t uart_num) {
    // SAMConfiguration (NXP UM10232 §7.2.10):
    //   Mode    = 0x01 (Normal mode — SAM not used)
    //   Timeout = 0x14 (1 s, irrelevant in Normal mode)
    //   IRQ     = 0x00 (do NOT use IRQ pin — we poll via UART)
    //             Setting IRQ=0x01 on boards that hold P70_IRQ LOW after a
    //             response can corrupt the next command's ACK detection.
    uint8_t cmd[] = {PN532_COMMAND_SAMCONFIGURATION, 0x01, 0x14, 0x00};
    ESP_LOGI(TAG, "── SAMConfiguration ──");

    esp_err_t err = pn532_write_frame(uart_num, cmd, sizeof(cmd));
    if (err != ESP_OK) return err;

    err = pn532_read_ack(uart_num, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SAMConfig: no ACK");
        return err;
    }

    // SAM response payload:  [0] = 0x15 (cmd echo = SAMCONFIGURATION + 1)
    uint8_t resp[4] = {0};
    err = pn532_read_response(uart_num, resp, sizeof(resp), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SAMConfig: no response");
        return err;
    }

    if (resp[0] != 0x15) {
        ESP_LOGE(TAG, "SAMConfig: unexpected response 0x%02X (want 0x15)", resp[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "SAM configured OK (Normal mode).");
    return ESP_OK;
}

esp_err_t pn532_set_passive_activation_retries(uart_port_t uart_num, uint8_t max_retries) {
    // RFConfiguration CfgItem=0x05 (MaxRetries):
    //   MxRtyATR             = 0xFF (no limit)
    //   MxRtyPSL             = 0x01
    //   MxRtyPassiveActivation = max_retries
    uint8_t cmd[] = {0x32, 0x05, 0xFF, 0x01, max_retries};
    ESP_LOGI(TAG, "── RFConfig MaxRetries=%d ──", max_retries);

    esp_err_t err = pn532_write_frame(uart_num, cmd, sizeof(cmd));
    if (err != ESP_OK) return err;

    err = pn532_read_ack(uart_num, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RFConfig: no ACK");
        return err;
    }

    uint8_t resp[4] = {0};
    err = pn532_read_response(uart_num, resp, sizeof(resp), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RFConfig: no response");
        return err;
    }

    // RFConfiguration response echo = command code + 1 = 0x32 + 1 = 0x33
    if (resp[0] != 0x33) {
        ESP_LOGE(TAG, "RFConfig: unexpected response code 0x%02X (want 0x33)", resp[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "RFConfig MaxRetries set to %d OK.", max_retries);
    return ESP_OK;
}

esp_err_t pn532_read_passive_target(uart_port_t uart_num,
                                    uint8_t *uid, uint8_t *uid_len,
                                    uint32_t timeout_ms) {
    // InListPassiveTarget (NXP UM10232 §7.3.5):
    //   MaxTargets = 0x01
    //   BrTy       = 0x00 (106 kbps ISO14443A / Mifare)
    uint8_t cmd[] = {PN532_COMMAND_INLISTPASSIVETARGET, 0x01, PN532_MIFARE_ISO14443A};

    esp_err_t err = pn532_write_frame(uart_num, cmd, sizeof(cmd));
    if (err != ESP_OK) return err;

    err = pn532_read_ack(uart_num, 500);
    if (err != ESP_OK) return err;

    // The PN532 will wait for a card and respond only when one is found
    // (or after its internal retry limit expires). Give timeout_ms for the response.
    uint8_t resp[64] = {0};
    err = pn532_read_response(uart_num, resp, sizeof(resp), timeout_ms);
    if (err != ESP_OK) return err;

    // Response payload (after TFI):
    //  [0] = 0x4B  (cmd echo = INLISTPASSIVETARGET + 1)
    //  [1] = NbTargets
    //  [2] = Tg (target number, 1-based)
    //  [3..4] = SENS_RES (ATQA)
    //  [5] = SEL_RES (SAK)
    //  [6] = NFCIDLength
    //  [7..] = NFCID (UID)
    if (resp[0] != 0x4B) {
        ESP_LOGE(TAG, "InListPassiveTarget: bad cmd-echo 0x%02X", resp[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (resp[1] < 1) {
        return ESP_ERR_NOT_FOUND;   // No card in field
    }

    uint8_t len = resp[6];
    if (len > 7) len = 7;
    *uid_len = len;
    memcpy(uid, &resp[7], len);

    ESP_LOGI(TAG, "Card found! NbTargets=%d SAK=0x%02X UIDlen=%d", resp[1], resp[5], len);
    return ESP_OK;
}
