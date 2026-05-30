#ifndef SPI_OTA_H
#define SPI_OTA_H

#include <stdint.h>
#include <stdbool.h>
#include "em_usart.h"
#include "em_gpio.h"

// ─── SPI 핀 (USART0 LOC0) ───────────────────────────
#define SPI_USART           USART1
#define SPI_CLK_USART       cmuClock_USART1
#define SPI_MISO_PORT       gpioPortA
#define SPI_MISO_PIN        7
#define SPI_MOSI_PORT       gpioPortA
#define SPI_MOSI_PIN        6
#define SPI_CLK_PORT        gpioPortA
#define SPI_CLK_PIN         8
#define SPI_CS_PORT         gpioPortA
#define SPI_CS_PIN          9

// ─── 버퍼 ────────────────────────────────────────────
#define SPI_BUFLEN          130

// ─── 커맨드 ─────────────────────────────────────────
#define CMD_PREPARE         0x01
#define CMD_WRITE_CHUNK     0x02
#define CMD_START_OTA       0x03
#define CMD_STATUS          0x04
#define CMD_LOG             0x05  // ← 신규: 1바이트 로그용

// ─── OTA 상태 ────────────────────────────────────────
typedef enum {
    OTA_IDLE = 0,
    OTA_RECEIVING,
    OTA_READY,
    OTA_INSTALL
} ota_state_t;

typedef struct {
    uint32_t cs_irq_count;      // CS 하강엣지 발생 횟수
    uint32_t rx_irq_count;      // RX IRQ 발생 횟수 (바이트 수)
    uint32_t packet_count;      // 완성된 패킷 수
    uint32_t last_rx_bytes;     // 마지막 패킷에서 수신한 바이트 수
    bool     cs_fired;          // CS IRQ 발생 플래그
} spi_debug_t;

extern spi_debug_t spi_dbg;

extern volatile bool  spi_packet_ready;
extern uint8_t        spi_rx_buf[SPI_BUFLEN];
extern uint8_t        spi_tx_buf[SPI_BUFLEN];
extern ota_state_t    ota_state;

void initGPIO_SPI(void);
void initUSART0_Slave(void);
void spi_start_receive(void);

#endif
