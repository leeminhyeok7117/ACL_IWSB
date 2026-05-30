#include "app_process.h"
#include "spi_ota.h"
#include "app_log.h"
#include "btl_interface.h"
#include "em_gpio.h"
#include "em_cmu.h"
#include <string.h>

ota_state_t ota_state    = OTA_IDLE;
static uint32_t fw_total_size = 0;
static uint32_t fw_written    = 0;
static uint8_t  fw_tag        = 0;

#define LED0_PORT   gpioPortF
#define LED0_PIN    4
#define LED1_PORT   gpioPortF
#define LED1_PIN    5

static bool led_state = false;

void led_init(void)
{
    CMU_ClockEnable(cmuClock_GPIO, true);
    GPIO_PinModeSet(LED0_PORT, LED0_PIN, gpioModePushPull, 0);
    GPIO_PinModeSet(LED1_PORT, LED1_PIN, gpioModePushPull, 0);
}

// ─────────────────────────────────────────────────────
// 1바이트 로그 핸들러 (CMD 0x05)
// AP_OBC가 [0x05][value] 형태로 전송
// ─────────────────────────────────────────────────────
static void led_toggle_alternate(void)
{
    if (!led_state) {
        GPIO_PinOutSet(LED0_PORT, LED0_PIN);    // LED0 ON
        GPIO_PinOutClear(LED1_PORT, LED1_PIN);  // LED1 OFF
        app_log("[LED] LED0 ON / LED1 OFF\n");
    } else {
        GPIO_PinOutClear(LED0_PORT, LED0_PIN);  // LED0 OFF
        GPIO_PinOutSet(LED1_PORT, LED1_PIN);    // LED1 ON
        app_log("[LED] LED0 OFF / LED1 ON\n");
    }
    led_state = !led_state;
}

static void handle_log(void)
{
    uint8_t val = spi_rx_buf[1];
    app_log("[LOG] AP_OBC sent 0x%02X (%d)\n", val, val);

    if (val == 0x01) {
        led_toggle_alternate();  // ← 0x01이면 LED 교차 토글
    }

    spi_tx_buf[0] = 0xAA;
}

// ─────────────────────────────────────────────────────
static void handle_prepare(void)
{
    memcpy(&fw_total_size, &spi_rx_buf[1], 4);
    fw_tag     = spi_rx_buf[5];
    fw_written = 0;

    app_log("[OTA] PREPARE received\n");
    app_log("[OTA] Firmware size : %lu bytes\n", fw_total_size);
    app_log("[OTA] Tag           : 0x%02X\n", fw_tag);
    app_log("[OTA] Erasing storage slot 0...\n");

    int32_t ret = bootloader_eraseStorageSlot(0);

    if (ret == BOOTLOADER_OK) {
        ota_state     = OTA_RECEIVING;
        spi_tx_buf[0] = 0xAA;
        app_log("[OTA] Erase OK. Ready to receive chunks.\n");
    } else {
        ota_state     = OTA_IDLE;
        spi_tx_buf[0] = 0xFF;
        app_log("[OTA] Erase FAILED (err=0x%lX). Aborting.\n", ret);
    }
}

// ─────────────────────────────────────────────────────
static void handle_write_chunk(void)
{
    if (ota_state != OTA_RECEIVING) {
        app_log("[OTA] WRITE_CHUNK ignored: not in RECEIVING state (state=%d)\n",
                ota_state);
        spi_tx_buf[0] = 0xFF;
        return;
    }

    uint8_t  *chunk      = &spi_rx_buf[1];
    uint32_t  chunk_size = 128;

    if (fw_written + chunk_size > fw_total_size) {
        chunk_size = fw_total_size - fw_written;
    }

    int32_t ret = bootloader_writeStorage(0, fw_written, chunk, chunk_size);

    if (ret == BOOTLOADER_OK) {
        fw_written   += chunk_size;
        spi_tx_buf[0] = 0xAA;

        // 진행률 로그 (매 청크마다)
        uint32_t pct = (fw_written * 100) / fw_total_size;
        app_log("[OTA] Chunk written: %lu / %lu bytes (%lu%%)\n",
                fw_written, fw_total_size, pct);

        if (fw_written >= fw_total_size) {
            ota_state = OTA_READY;
            app_log("[OTA] All chunks received. Firmware download complete.\n");
        }
    } else {
        ota_state     = OTA_IDLE;
        spi_tx_buf[0] = 0xFF;
        app_log("[OTA] Write FAILED at offset %lu (err=0x%lX). Aborting.\n",
                fw_written, ret);
    }
}

// ─────────────────────────────────────────────────────
static void handle_start_ota(void)
{
    if (ota_state != OTA_READY) {
        app_log("[OTA] START_OTA ignored: firmware not ready (state=%d)\n",
                ota_state);
        spi_tx_buf[0] = 0xFF;
        return;
    }

    app_log("[OTA] START_OTA received. Rebooting to install firmware...\n");
    spi_tx_buf[0] = 0xAA;
    ota_state     = OTA_INSTALL;
}

// ─────────────────────────────────────────────────────
static void handle_status(void)
{
    spi_tx_buf[0] = (uint8_t)ota_state;

    const char *state_str[] = {
        "IDLE", "RECEIVING", "READY", "INSTALL"
    };
    app_log("[STATUS] state=%s written=%lu/%lu\n",
            state_str[ota_state], fw_written, fw_total_size);
}

static void spi_verify_packet(void)
{
    // 1. CS IRQ 발생 확인
    if (spi_dbg.cs_fired) {
        app_log("[SPI-DBG] CS falling edge detected (total: %lu)\n",
                spi_dbg.cs_irq_count);
        spi_dbg.cs_fired = false;
    }

    // 2. 수신 바이트 수 검증
    if (spi_dbg.last_rx_bytes != SPI_BUFLEN) {
        app_log("[SPI-ERR] Byte count mismatch! expected=%d got=%lu\n",
                SPI_BUFLEN, spi_dbg.last_rx_bytes);
    } else {
        app_log("[SPI-DBG] Byte count OK (%lu bytes)\n",
                spi_dbg.last_rx_bytes);
    }

    // 3. 누적 통계
    app_log("[SPI-DBG] CS=%lu RX_IRQ=%lu PKT=%lu\n",
            spi_dbg.cs_irq_count,
            spi_dbg.rx_irq_count,
            spi_dbg.packet_count);

    // 4. 수신 데이터 hex dump (첫 16바이트)
    app_log("[SPI-DBG] RX dump: ");
    for (int i = 0; i < 16; i++) {
        app_log("%02X ", spi_rx_buf[i]);
    }
    app_log("\n");
}

// ─── CMD_ECHO 핸들러 (0x06) ───────────────────────
// AP_OBC가 [0x06][data 128B] 전송
// EFR32는 다음 트랜잭션에서 받은 data를 MISO로 돌려줌
static void handle_echo(void)
{
    app_log("[ECHO] Echo request received\n");
    app_log("[ECHO] RX: ");
    for (int i = 1; i <= 8; i++) {         // 검증용 앞 8바이트 출력
        app_log("%02X ", spi_rx_buf[i]);
    }
    app_log("\n");

    // 받은 데이터를 tx_buf에 복사 → 다음 CS에서 MISO로 전송
    memcpy(spi_tx_buf, &spi_rx_buf[1], SPI_BUFLEN - 1);

    app_log("[ECHO] Echo data loaded into TX buffer\n");
    app_log("[ECHO] AP_OBC가 dummy read를 한 번 더 해야 echo 데이터 수신 가능\n");
}

// ─────────────────────────────────────────────────────
void app_process_action(void)
{
    if (spi_packet_ready) {
        spi_packet_ready = false;

        // ← 매 패킷마다 SPI 수신 상태 검증
        spi_verify_packet();

        uint8_t cmd = spi_rx_buf[0];
        app_log("[SPI] CMD: 0x%02X\n", cmd);

        switch (cmd) {
            case 0x06:            handle_echo();        break;
            case CMD_LOG:         handle_log();         break;
            case CMD_PREPARE:     handle_prepare();     break;
            case CMD_WRITE_CHUNK: handle_write_chunk(); break;
            case CMD_START_OTA:   handle_start_ota();   break;
            case CMD_STATUS:      handle_status();      break;
            default:
                app_log("[SPI] Unknown CMD: 0x%02X\n", cmd);
                spi_tx_buf[0] = 0xFF;
                break;
        }

        spi_start_receive();
    }

    if (ota_state == OTA_INSTALL) {
        bootloader_rebootAndInstall();
    }
}
