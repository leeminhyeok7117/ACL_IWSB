#include "app_init.h"
#include "spi_ota.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_usart.h"
#include "app_log.h"

extern void led_init(void);

// ─── 전역 변수 ────────────────────────────────────
volatile bool  spi_packet_ready = false;
uint8_t        spi_rx_buf[SPI_BUFLEN];
uint8_t        spi_tx_buf[SPI_BUFLEN];

static volatile uint32_t bufpos = 0;

// ─── GPIO 초기화 ──────────────────────────────────
void initGPIO_SPI(void)
{
    CMU_ClockEnable(cmuClock_GPIO, true);

    // MOSI: PA1 입력
    GPIO_PinModeSet(SPI_MOSI_PORT, SPI_MOSI_PIN, gpioModeInput, 0);
    // MISO: PA0 출력 (slave → master)
    GPIO_PinModeSet(SPI_MISO_PORT, SPI_MISO_PIN, gpioModePushPull, 0);
    // CLK: PA2 입력
    GPIO_PinModeSet(SPI_CLK_PORT, SPI_CLK_PIN, gpioModeInput, 0);
    // CS:  PA3 입력, 풀업
    GPIO_PinModeSet(SPI_CS_PORT, SPI_CS_PIN, gpioModeInput, 1);

    // CS 하강 엣지 인터럽트 (CS_PIN=3, 홀수 → GPIO_ODD)
    GPIO_ExtIntConfig(SPI_CS_PORT, SPI_CS_PIN, SPI_CS_PIN,
                      false,  // rising edge: no
                      true,   // falling edge: yes
                      false); // 아직 활성화 안 함

    GPIO_ExtIntConfig(SPI_CS_PORT, SPI_CS_PIN, SPI_CS_PIN,
                      true,   // rising edge: yes  ← 추가
                      true,   // falling edge: yes
                      false);

    NVIC_ClearPendingIRQ(GPIO_ODD_IRQn);
    NVIC_EnableIRQ(GPIO_ODD_IRQn);
}

// ─── USART0 Slave 초기화 ──────────────────────────
void initUSART0_Slave(void)
{
    CMU_ClockEnable(SPI_CLK_USART, true);

    USART_InitSync_TypeDef init = USART_INITSYNC_DEFAULT;
    init.master    = false;            // Slave 모드
    init.msbf      = true;             // MSB first
    init.clockMode = usartClockMode0;  // CPOL=0, CPHA=0
    init.enable    = usartDisable;     // CS 전까지 비활성화

    USART_InitSync(SPI_USART, &init);

    // USART0 LOC0: TX=PA0, RX=PA1, CLK=PA2, CS=PA3
    SPI_USART->ROUTELOC0 =
        (1 << _USART_ROUTELOC0_TXLOC_SHIFT)  |   // PA0 = MISO
        (1 << _USART_ROUTELOC0_RXLOC_SHIFT)  |   // PA1 = MOSI
        (1 << _USART_ROUTELOC0_CLKLOC_SHIFT) |   // PA2 = CLK
        (1 << _USART_ROUTELOC0_CSLOC_SHIFT);     // PA3 = CS

    SPI_USART->ROUTEPEN =
        USART_ROUTEPEN_TXPEN |
        USART_ROUTEPEN_RXPEN |
        USART_ROUTEPEN_CLKPEN|
        USART_ROUTEPEN_CSPEN;

    NVIC_ClearPendingIRQ(USART1_RX_IRQn);
    NVIC_EnableIRQ(USART1_RX_IRQn);
}

// ─── 수신 사이클 시작 ────────────────────────────
void spi_start_receive(void)
{
    bufpos = 0;
    memset(spi_rx_buf, 0, SPI_BUFLEN);
    spi_tx_buf[0] = 0x00;  // 기본 응답: 0x00

    // CS 인터럽트 활성화 (CS 하강 엣지 대기)
    GPIO_IntClear(1 << SPI_CS_PIN);
    GPIO_IntEnable(1 << SPI_CS_PIN);
}

// ─── app_init ─────────────────────────────────────
void app_init(void)
{
    app_log("===========================\n");
    app_log(" Firmware Booted!\n");
    app_log("===========================\n");
    bootloader_init();
    led_init();
    initGPIO_SPI();
    initUSART0_Slave();

    spi_start_receive();  // 첫 수신 대기
    app_log("[SPI] Slave initialized. Waiting for master...\n");
}

// ════════════════════════════════════════════════════
// IRQ 핸들러 (예제 원본 구조 그대로 유지)
// ════════════════════════════════════════════════════

spi_debug_t spi_dbg = {0};

void GPIO_ODD_IRQHandler(void)
{
    GPIO_IntClear(1 << SPI_CS_PIN);

    if (GPIO_PinInGet(SPI_CS_PORT, SPI_CS_PIN) == 0) {
        // ── CS 하강 엣지: 전송 시작 ──────────────
        spi_dbg.cs_irq_count++;
        spi_dbg.cs_fired = true;

        USART_Enable(SPI_USART, usartEnable);
        USART_IntEnable(SPI_USART, USART_IEN_RXDATAV);
        SPI_USART->TXDATA = spi_tx_buf[0];
        GPIO_IntDisable(1 << SPI_CS_PIN);
        GPIO_IntEnable(1 << SPI_CS_PIN);  // 상승 엣지 대기 위해 재활성화

    } else {
        // ── CS 상승 엣지: 전송 완료 ──────────────
        USART_IntDisable(SPI_USART, USART_IEN_RXDATAV);
        USART_Enable(SPI_USART, usartDisable);
        GPIO_IntDisable(1 << SPI_CS_PIN);

        if (bufpos > 0) {
            spi_dbg.packet_count++;
            spi_dbg.last_rx_bytes = bufpos;
            spi_packet_ready = true;    // ← 몇 바이트든 처리
        }
    }
}

void USART1_RX_IRQHandler(void)
{
    if (bufpos < SPI_BUFLEN) {
        spi_rx_buf[bufpos] = (uint8_t)SPI_USART->RXDATA;
        bufpos++;
        spi_dbg.rx_irq_count++;

        if (bufpos < SPI_BUFLEN) {
            SPI_USART->TXDATA = spi_tx_buf[bufpos];
        }
        // ← bufpos == SPI_BUFLEN 도달해도 여기서 처리 안 함
        //    CS 상승 엣지에서 처리
    }
}
