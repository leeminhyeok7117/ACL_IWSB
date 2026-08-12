/***************************************************************************//**
 * @file
 * @brief app_init.c — Master (Sink / OTA Server) — Automated OTA
 *******************************************************************************
 * SPDX-License-Identifier: Zlib
 ******************************************************************************/

#include "app_log.h"
#include "sl_app_common.h"
#include "stack/include/ember.h"
#include "app_process.h"
#include "app_init.h"
#include "iq_capture.h"   // [IQ] 리드백 프레임 크기 상수(IQ_READBACK_MAX 등)
#include "app_framework_common.h"
#include "psa/crypto.h"
#include "mbedtls/build_info.h"
#include "btl_interface.h"
#include "btl_interface_storage.h"
#include "fw_guard.h"
#include "em_i2c.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_usart.h"
#include "gpiointerrupt.h"
#include "em_ldma.h"
#include "em_rmu.h"   // [진단] 리셋 사유(RSTCAUSE)
#include "dmadrv.h"
#include <string.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
#define PSA_AES_KEY_ID            1

#define MASTER_NETWORK_PAN_ID     0xFFFF   // ★ 위성 RX 구펌웨어가 0xFFFF로 join → 반드시 유지
#define MASTER_TX_POWER           0

// ─── 실제 보드 배선 (검증된 동작 코드 기준) ────────────────────────────────
//   SDA = PC10 (LOC16),  SCL = PC11 (LOC14)
#define OBC_I2C_PERIPHERAL        I2C0
#define OBC_I2C_SDA_PORT          gpioPortC
#define OBC_I2C_SDA_PIN           10
#define OBC_I2C_SCL_PORT          gpioPortC
#define OBC_I2C_SCL_PIN           11
#define OBC_I2C_SDA_LOC           I2C_ROUTELOC0_SDALOC_LOC16
#define OBC_I2C_SCL_LOC           I2C_ROUTELOC0_SCLLOC_LOC14
#define OBC_I2C_SLAVE_ADDR        0x71   // 7비트 (AP_OBC 호스트 EFR32_ADDR=0x71과 일치)
#define OBC_RX_CHUNK_SIZE         128    // FW 패킷 124B(=1+2+1+120) 수용

// ─── OBC SPI 슬레이브 (I2C 와 병행 — 둘 중 아무 쪽으로나 명령/회수 가능) ──────
//   EFR32 = 슬레이브. OBC 가 마스터로 CLK/CS 를 공급한다.
//   USART0=디버그콘솔, USART1=외부플래시가 이미 점유 → USART2 사용.
//
//   [핀 근거] 실제 위성보드 스키매틱(efr32fg12p433f1024gm48):
//     PA0 = OBC_SPI_MOSI
//     PA1 = OBC_SPI_MISO
//     PA2 = OBC_SPI_SCLK
//     PA3 = OBC_SPI_CS_N
//     (PA4 = FLASH_SPI_CS_N, PC6/7/8 = FLASH SPI  → 외부플래시는 USART1 그대로)
//
//   ★★ PA0~PA3 에 도달할 수 있는 것은 USART0 과 USART1 뿐이다(데이터시트 AF 표).
//     USART1 은 외부플래시(PC6/7/8)가 쓰므로 → OBC SPI 는 **USART0** 이어야 한다.
//     USART0 은 개발보드에서 VCOM 디버그 콘솔이 쓰던 것인데, 이 위성보드에는
//     UART 콘솔 배선 자체가 없으므로 콘솔을 포기하고 SPI 로 전용한다.
//
//   ★★ 핵심: 동기 슬레이브 모드에서는 USART 의 TX/RX 핀 기능이 내부적으로 반전된다.
//     즉 ROUTELOC 의 의미가 마스터일 때와 반대다:
//        TXLOC 으로 지정한 핀  →  실제로는 **입력(MOSI)** 이 된다
//        RXLOC 으로 지정한 핀  →  실제로는 **출력(MISO)** 이 된다
//     따라서 "TXLOC = MOSI 핀", "RXLOC = MISO 핀" 으로 잡아야 한다.
//
//   [근거] Silicon Labs 공식 예제 BRD4253A_EFR32FG12_spi_slave_interrupt:
//        USART2->ROUTELOC0 = (US2MISO_LOC << RXLOC_SHIFT)    // RX  <- MISO(PA7)
//                          | (US2MOSI_LOC << TXLOC_SHIFT)    // TX  <- MOSI(PA6)
//                          | (US2CLK_LOC  << CLKLOC_SHIFT)
//                          | (US2CS_LOC   << CSLOC_SHIFT);
//     이 예제는 네 신호 모두 LOC 1 을 쓰며, GPIO 도 MOSI(PA6)=입력 /
//     MISO(PA7)=푸시풀출력 으로 설정한다(= 방향 반전을 확인해 주는 증거).
//
//   [데이터시트 AF 표] efr32fg12p_af_{ports,pins}.h (데이터시트에서 생성):
//        LOC1 :  TX=PA6   RX=PA7   CLK=PA8   CS=PA9
//     → 우리가 원하는 PA6/PA7/PA8/PA9 조합이 통째로 LOC1 하나에 들어온다.
//
//   ※ 과거에 RX_LOC0 / TX_LOC2 로 잡았던 것은 위 "방향 반전"을 몰라서 생긴 오류다.
//     그 설정에서는 EFR32 가 PA6(=OBC 의 MOSI)을 출력으로 몰아 충돌시키고,
//     PA7(=MISO)은 아무도 구동하지 않아 로직애널라이저에 0xFF 로 관측됐다.
#define OBC_SPI_PERIPHERAL        USART0
#define OBC_SPI_CLK_IRQn          USART0_RX_IRQn
#define OBC_SPI_IRQHandler        USART0_RX_IRQHandler
#define OBC_SPI_CMU_CLOCK         cmuClock_USART0

#define OBC_SPI_MOSI_PORT         gpioPortA   // OBC→EFR32 (슬레이브 입력)
#define OBC_SPI_MOSI_PIN          0
#define OBC_SPI_MISO_PORT         gpioPortA   // EFR32→OBC (슬레이브 출력)
#define OBC_SPI_MISO_PIN          1
#define OBC_SPI_CLK_PORT          gpioPortA   // OBC 공급
#define OBC_SPI_CLK_PIN           2
#define OBC_SPI_CS_PORT           gpioPortA   // OBC 공급(Low 활성)
#define OBC_SPI_CS_PIN            3

// 네 신호 모두 LOC0 = PA0/PA1/PA2/PA3. (슬레이브 방향 반전은 위 주석 참조)
#define OBC_SPI_TX_LOC            0U          // TX  라우팅 → PA0 = MOSI (실제로는 입력)
#define OBC_SPI_RX_LOC            0U          // RX  라우팅 → PA1 = MISO (실제로는 출력)
#define OBC_SPI_CLK_LOC           0U          // CLK → PA2
#define OBC_SPI_CS_LOC            0U          // CS  → PA3

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void     obc_i2c_slave_init(void);
static void     obc_spi_slave_init(void);
static void     obc_spi_cs_cb(uint8_t intNo);
static void     obc_spi_prime_tx(void);
static void     bootloader_storage_init(void);
static void     form_network(void);

// -----------------------------------------------------------------------------
//                                Global Variables
// -----------------------------------------------------------------------------
extern EmberKeyData security_key;
extern psa_key_id_t security_key_id;

volatile ota_master_state_t ota_state = OTA_IDLE;

volatile uint8_t  obc_rx_buffer[OBC_RX_CHUNK_SIZE + 8];
volatile uint16_t obc_rx_len     = 0;
volatile bool     obc_cmd_ready  = false;
// ★ 명령 확정 시점의 길이를 따로 걸어 둔다(latch).
//   SPI 는 CS High 콜백에서 다음 트랜잭션을 위해 obc_rx_len 을 0 으로 되돌리므로,
//   tick 이 obc_rx_len 을 그대로 읽으면 항상 0 이 되어 명령이 전부 무시된다.
volatile uint16_t obc_cmd_len    = 0;

// ★ [home 채널] 네트워크를 실제로 form 한 채널. 측정 상태머신이 "돌아갈 곳"의
//   유일한 기준점이다. 라운드마다 emberGetRadioChannel() 로 다시 읽으면 복귀가
//   한 번 어긋난 순간 측정 채널이 home 으로 굳어버린다(실측: 마스터가 ch20 에
//   눌러앉아 폴링·MEAS_CMD·ID_ANNOUNCE 가 전부 끊김).
uint8_t    g_home_channel    = 0;

uint32_t   gbl_image_size    = 0;
uint32_t   gbl_write_offset  = 0;
uint8_t    ota_image_tag     = 0xAA;

EmberNodeId ota_target_node  = EMBER_NULL_NODE_ID;

// ─── [IQ] OBC 리드백 스테이징 버퍼 (I2C / SPI 공용) ──────────────────────────
//   tick 컨텍스트(handle_cmd_iq_read)에서 쌓인 IQ 전체를 채우고,
//   각 버스의 IRQ 는 이 버퍼를 바이트 단위로 스트리밍만 한다(경쟁 회피).
//   프레임: [상태헤더 8B] + 레코드들(각 IQ_REC_SIZE 고정) — iq_capture.h 참조.
//   RAM: IQ_READBACK_MAX ≈ 33KB (FG12 RAM 256KB → 여유).
//   ※ 두 버스가 하나의 버퍼를 공유한다. OBC 가 동시에 양쪽으로 읽지는 않으므로
//     충분하며, 스테이징은 항상 tick(비선점) 에서만 일어난다.
#define OBC_TX_BUF_SIZE   IQ_READBACK_MAX
static volatile uint8_t  obc_tx_buf[OBC_TX_BUF_SIZE];
static volatile uint16_t obc_tx_len  = 0;   // 스테이징된 전체 프레임 길이(패딩 포함)
static volatile uint16_t obc_tx_payload = 0; // 헤더+레코드까지의 유효 길이(회수 판정용)
static volatile uint16_t i2c_tx_idx  = 0;   // I2C 다음 전송 바이트 인덱스
static volatile uint16_t spi_tx_idx  = 0;   // SPI 다음 전송 바이트 인덱스
static volatile bool     i2c_tx_done = false;  // I2C 로 프레임 한 바퀴 내보냈는가
static volatile bool     spi_tx_done = false;  // SPI 로 프레임 한 바퀴 내보냈는가
static volatile bool     i2c_is_read = false;

// 공용 스테이징 버퍼 접근자 — CLI 덤프가 같은 버퍼를 재사용해 RAM 중복을 막는다.
const uint8_t *obc_readback_buffer(uint16_t *len)
{
  if (len != NULL) *len = obc_tx_len;
  return (const uint8_t *)obc_tx_buf;
}

// ─── [진단] SPI 슬레이브 활동 카운터 ────────────────────────────────────────
//   SPI 슬레이브는 마스터가 클럭을 넣어주지 않으면 아무 일도 일어나지 않는다.
//   따라서 "동작하는가"를 확인하려면 무엇이 실제로 들어왔는지를 세어야 한다.
//   spi_stat CLI 로 확인 — 배선/클럭/프레이밍 중 어디가 문제인지 좁혀준다.
static volatile uint32_t spi_dbg_bytes    = 0;   // 수신한 총 바이트(=클럭 도달 증거)
static volatile uint32_t spi_dbg_cs_edges = 0;   // CS 상승 엣지 수(=프레이밍 증거)
static volatile uint32_t spi_dbg_frames   = 0;   // 내용이 있는 트랜잭션 수
static volatile uint16_t spi_dbg_last_len = 0;   // 마지막 프레임 길이
static volatile uint8_t  spi_dbg_last_cmd = 0;   // 마지막 프레임 선두 바이트
static volatile uint16_t spi_dbg_first_rx = 0xFFFFU;  // 최초 수신 바이트(0xFFFF=없음)
static volatile uint32_t spi_dbg_overrun   = 0;   // ★ 수신 오버런(RXOF) 횟수
                                                  //   0 이 아니면 클럭이 너무 빠르다.

void obc_spi_get_stats(obc_spi_stats_t *s)
{
  if (s == NULL) return;
  s->bytes    = spi_dbg_bytes;
  s->cs_edges = spi_dbg_cs_edges;
  s->frames   = spi_dbg_frames;
  s->last_len = spi_dbg_last_len;
  s->last_cmd = spi_dbg_last_cmd;
  s->first_rx = spi_dbg_first_rx;
  s->overrun  = spi_dbg_overrun;
  s->staged   = obc_tx_len;
  s->tx_idx   = spi_tx_idx;
  // 현재 핀 레벨(배선 확인용). CS 는 유휴 시 1, CLK/MOSI 는 0 이어야 정상.
  s->cs_level   = (uint8_t)GPIO_PinInGet(OBC_SPI_CS_PORT,   OBC_SPI_CS_PIN);
  s->clk_level  = (uint8_t)GPIO_PinInGet(OBC_SPI_CLK_PORT,  OBC_SPI_CLK_PIN);
  s->mosi_level = (uint8_t)GPIO_PinInGet(OBC_SPI_MOSI_PORT, OBC_SPI_MOSI_PIN);
}

void obc_spi_reset_stats(void)
{
  spi_dbg_bytes = 0; spi_dbg_cs_edges = 0; spi_dbg_frames = 0;
  spi_dbg_last_len = 0; spi_dbg_last_cmd = 0; spi_dbg_first_rx = 0xFFFFU;
  spi_dbg_overrun = 0;
}

// [지상 테스트] 물리 SPI 없이 명령 처리 경로만 검증한다.
//   ISR 이 하는 일(수신 버퍼 채우기 + 길이 latch + ready 플래그)을 그대로 흉내내어,
//   길이 검증 → 디스패치 → 리드백 스테이징까지가 맞는지 확인할 수 있다.
//   ※ 배선/클럭/프레이밍은 검증하지 못한다(그건 마스터가 있어야 한다).
void obc_spi_inject(const uint8_t *frame, uint16_t len)
{
  if (frame == NULL || len == 0U) return;
  if (len > sizeof(obc_rx_buffer)) len = sizeof(obc_rx_buffer);
  for (uint16_t i = 0; i < len; i++) obc_rx_buffer[i] = frame[i];
  obc_cmd_len   = len;
  obc_cmd_ready = true;     // 다음 tick 에서 process_obc_command() 가 처리
}

// SPI 슬레이브 송신 선적재(priming).
//   ★ SPI 슬레이브는 "클럭이 들어오는 순간" 이미 시프트 레지스터에 있던 값을
//     내보낸다. 즉 트랜잭션의 첫 바이트는 ISR 이 실을 틈이 없다(ISR 은 첫 바이트를
//     받은 뒤에야 다음 바이트를 싣는다). 따라서 미리 obc_tx_buf[0] 을 적재해 두지
//     않으면 마스터가 받는 프레임이 1바이트씩 밀려 magic(0xA5) 검사부터 깨진다.
//     트랜잭션이 끝날 때마다(CS High) 다시 선적재해 항상 프레임 선두부터 나가게 한다.
// 다음에 내보낼 바이트를 꺼낸다(SPI/I2C 공용 규칙).
//   ★ 이어 읽기(chunked read): 인덱스가 트랜잭션을 넘어 이어지므로 OBC 가
//     프레임을 여러 번에 나눠 읽어도 자연스럽게 연결된다.
//   ★ 재읽기(re-read): 페이로드 끝에 도달하면 인덱스가 처음으로 되돌아간다.
//     OBC 가 읽다가 어긋났다고 판단하면 그냥 계속 읽기만 하면 프레임 선두부터
//     다시 나온다 — 재전송을 요청하는 별도 명령이 필요 없다.
//     (프레임은 다음 스윕이 끝날 때까지 그대로 남아 있다 → 수 초의 재시도 여유)
// 내보낼 배치가 없을 때 MISO 에 실을 값.
//   ★ 0x00 으로 두면 "정상 동작하며 보낼 게 없는 상태" 와 "선이 Low 에 붙어
//     있는 고장" 을 구분할 수 없다. 0/1 이 번갈아 나오는 값을 쓰면 파형만 봐도
//     갈리고, OBC 도 슬레이브 생존을 확인할 수 있다.
//   0x5A = 01011010. 프레임 magic(0xA5)과 헷갈리지 않도록 다른 값으로 골랐다.
#define OBC_SPI_IDLE_BYTE   0x5AU

// ─── SPI 송신 DMA ───────────────────────────────────────────────────────────
//  [왜 DMA 인가]
//    SPI 슬레이브는 마스터가 클럭을 넣는 시점을 고를 수 없다. 바이트가 나갈
//    때마다 "다음 바이트"가 송신 레지스터에 미리 들어가 있어야 하는데, 이를
//    인터럽트로 채우면 바이트당 여유가 6.8us(1.17MHz 기준)뿐이다.
//    Connect 무선 인터럽트는 우선순위가 더 높고 수십 us 를 점유할 수 있어,
//    그 사이 클럭이 들어오면 송신 레지스터가 빈 채로 나가 0x00 이 섞인다
//    (실측 확인: 10바이트 버스트에서 2바이트 결손).
//    DMA 는 CPU 개입 없이 하드웨어가 직접 채우므로 인터럽트 지연과 무관하다.
//
//  [구성] TXDATA 를 쓰는 byte 디스크립터 체인 → TXBL 요청 1회당 1바이트를 채운다.
//    · 배치가 있으면 : 프레임 전체를 반복 → 마스터가 이어 읽기/재읽기 모두 가능
//    · 배치가 없으면 : 유휴 버퍼를 반복 → 슬레이브 생존 표시
//    · LDMA descriptor 1개의 최대 전송 수(2048)를 넘는 프레임은 여러
//      descriptor 로 나눠 연결한다.
static unsigned int       spi_dma_ch;
static bool               spi_dma_ok = false;
static LDMA_TransferCfg_t spi_dma_cfg;
#define OBC_SPI_DMA_DESC_MAX \
  ((OBC_TX_BUF_SIZE + LDMA_DESCRIPTOR_MAX_XFER_SIZE - 1U) / LDMA_DESCRIPTOR_MAX_XFER_SIZE)
static LDMA_Descriptor_t  spi_dma_desc[OBC_SPI_DMA_DESC_MAX];
// 유휴 반복용 버퍼.
//   ★ 1바이트 디스크립터를 자기 링크로 돌리면 바이트마다 LDMA 가 디스크립터를
//     메모리에서 다시 읽어와야 하고(=버스 경합에 취약) 인터럽트도 초당 수십만
//     번 발생한다. 실측에서 연속 7바이트 결손이 난 원인이 이것이다.
//     충분히 큰 버퍼를 한 디스크립터로 덮어 재적재 빈도를 1/N 로 줄인다.
#define OBC_SPI_IDLE_BUF_LEN  256U
static uint8_t            spi_dma_idle_buf[OBC_SPI_IDLE_BUF_LEN];

static bool obc_spi_dma_done_cb(unsigned int ch, unsigned int seq, void *user)
{
  (void)ch; (void)seq; (void)user;
  // 페이로드를 한 바퀴 다 내보냈다 = OBC 가 프레임 전체를 받아갔다.
  if (obc_tx_payload > 0U) spi_tx_done = true;
  return true;   // true = 전송 계속(자기 링크라 다음 바퀴가 이어진다)
}

// src 를 len 바이트만큼 USART TXDATA 로 무한 반복 송신하도록 DMA 를 재무장한다.
//   want_done : 한 바퀴 끝날 때 콜백을 받을지 여부.
//     실데이터 프레임(수십 KB)은 한 바퀴에 한 번이라 부담이 없지만,
//     유휴 반복에는 불필요하므로 꺼서 인터럽트 부하를 없앤다.
static void obc_spi_dma_arm(const volatile uint8_t *src, uint16_t len,
                            bool want_done)
{
  uint16_t offset = 0U;
  unsigned int desc_count = 0U;
  Ecode_t dma_status;

  if (!spi_dma_ok || len == 0U) return;
  DMADRV_StopTransfer(spi_dma_ch);
  // 이전 idle/frame 바이트가 FIFO에 남으면 새 프레임의 magic 앞에 붙으므로 제거한다.
  OBC_SPI_PERIPHERAL->CMD = USART_CMD_CLEARTX;

  spi_dma_cfg  = (LDMA_TransferCfg_t)
                 LDMA_TRANSFER_CFG_PERIPHERAL(ldmaPeripheralSignal_USART0_TXBL);

  while (offset < len) {
    uint16_t chunk = (uint16_t)(len - offset);
    if (chunk > LDMA_DESCRIPTOR_MAX_XFER_SIZE) {
      chunk = LDMA_DESCRIPTOR_MAX_XFER_SIZE;
    }

    spi_dma_desc[desc_count] = (LDMA_Descriptor_t)
      LDMA_DESCRIPTOR_LINKREL_M2P_BYTE((const void *)(src + offset),
                                       &(OBC_SPI_PERIPHERAL->TXDATA),
                                       chunk, 1);
    spi_dma_desc[desc_count].xfer.doneIfs = 0U;
    offset = (uint16_t)(offset + chunk);
    desc_count++;
  }

  // 마지막 descriptor 에서 첫 descriptor 로 되돌아가 같은 프레임을 반복한다.
  spi_dma_desc[desc_count - 1U].xfer.linkAddr =
    (1 - (int32_t)desc_count) * LDMA_DESCRIPTOR_NON_EXTEND_SIZE_WORD;
  spi_dma_desc[desc_count - 1U].xfer.doneIfs = want_done ? 1U : 0U;

  dma_status = DMADRV_LdmaStartTransfer((int)spi_dma_ch, &spi_dma_cfg,
                                        &spi_dma_desc[0],
                                        want_done ? obc_spi_dma_done_cb : NULL,
                                        NULL);
  if (dma_status != ECODE_EMDRV_DMADRV_OK) {
    spi_dma_ok = false;
    app_log_error("SPI: DMA start failed (0x%lX) — using IRQ TX fallback\n",
                  (unsigned long)dma_status);
    obc_spi_prime_tx();
  }
}

// 유휴(배치 없음) 상태로 되돌린다 — 유휴 바이트를 반복 송신.
static void obc_spi_dma_idle(void)
{
  for (unsigned i = 0; i < OBC_SPI_IDLE_BUF_LEN; i++) {
    spi_dma_idle_buf[i] = OBC_SPI_IDLE_BYTE;
  }
  obc_spi_dma_arm(spi_dma_idle_buf, OBC_SPI_IDLE_BUF_LEN, false);
}

static uint8_t obc_next_tx_byte(volatile uint16_t *idx, volatile bool *done)
{
  if (obc_tx_payload == 0U) return OBC_SPI_IDLE_BYTE;
  uint8_t b = obc_tx_buf[*idx];
  if (++(*idx) >= obc_tx_payload) {
    *done = true;      // 한 바퀴 전부 내보냄 = 회수 완료로 간주
    *idx  = 0;         // 처음으로 되돌려 재읽기 가능하게 한다
  }
  return b;
}

static void obc_spi_prime_tx(void)
{
  if (spi_dma_ok) return;   // DMA 가 송신을 전담 → 소프트웨어 선적재 불필요
  OBC_SPI_PERIPHERAL->CMD = USART_CMD_CLEARTX;   // 이전 트랜잭션 잔여 바이트 제거
  OBC_SPI_PERIPHERAL->TXDATA = obc_next_tx_byte(&spi_tx_idx, &spi_tx_done);
}

// tick 에서 호출: 쌓인 IQ 레코드 전부를 리드백 프레임으로 스테이징(전부 pop).
//   ★ OBC 는 0x05 만 보내므로 스윕이 끝날 때 우리가 알아서 이 함수를 부른다.
void obc_stage_iq_readback(void)
{
  uint16_t n = iq_readback_drain_all((uint8_t *)obc_tx_buf, sizeof(obc_tx_buf));
  obc_tx_len = (n > 0U) ? n : (uint16_t)sizeof(obc_tx_buf);
  // 실제 의미 있는 길이 = 헤더 + 레코드들. 뒤쪽은 0 패딩이라 OBC 가 읽을 필요가 없다.
  //   회수 완료 판정을 이 길이 기준으로 해야 OBC 가 "필요한 만큼만" 읽고 끝낼 수 있다.
  //   유효 길이 = 헤더 + 레코드들. 뒤쪽은 0 패딩이라 OBC 가 읽을 필요가 없다.
  obc_tx_payload = (uint16_t)(IQ_FRAME_HDR + (uint16_t)obc_tx_buf[5] * IQ_REC_SIZE);
  if (obc_tx_payload > obc_tx_len) obc_tx_payload = obc_tx_len;
  i2c_tx_idx  = 0;  i2c_tx_done = false;
  spi_tx_idx  = 0;  spi_tx_done = false;   // 새 배치 → 프레임 선두(magic)부터
  if (spi_dma_ok) {
    obc_spi_dma_arm(obc_tx_buf, obc_tx_payload, true);  // 프레임 전체 반복 송신
  } else {
    obc_spi_prime_tx();
  }
}

// ACK/timeout 뒤에는 이전 프레임을 폐기하고 SPI MISO를 0x5A idle로 복귀시킨다.
void obc_release_iq_readback(void)
{
  obc_tx_payload = 0U;
  obc_tx_len = 0U;
  i2c_tx_idx = 0U;  i2c_tx_done = false;
  spi_tx_idx = 0U;  spi_tx_done = false;

  if (spi_dma_ok) {
    obc_spi_dma_idle();
  } else {
    obc_spi_prime_tx();
  }
}

// 스테이징된 프레임이 버스로 빠져나갔는가?
//   ★ OBC 가 "다 받았다"고 알려주는 명령이 없으므로, 송신 인덱스가 유효 페이로드
//     끝에 도달했는지를 직접 관찰해 회수 완료를 판정한다(SPI/I2C 어느 쪽이든).
//     33KB 전부가 아니라 "헤더 + 레코드" 까지만 읽으면 회수된 것으로 본다.
bool obc_readback_consumed(void)
{
  return spi_tx_done || i2c_tx_done;
}

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void emberAfInitCallback(void)
{
  EmberStatus em_status = EMBER_ERR_FATAL;

  psa_crypto_init();
  app_log_info("\n=== Master (Sink / OTA Server) — Automated OTA ===\n");

  // ─── [진단] 왜 부팅했는가 ───────────────────────────────────────────────
  //   실측에서 마스터가 미션 도중 printf 문장 중간에 재부팅했다. 로그를 전혀
  //   남기지 않고 죽으므로 사유를 하드웨어 레지스터에서 읽어야 한다.
  //     WDOG    : tick 이 128 초 이상 굶었거나, 하드폴트 후 기본 핸들러 무한루프
  //     LOCKUP  : 폴트 처리 중 다시 폴트(더블 폴트)
  //     SYSREQ  : 우리 코드의 NVIC_SystemReset() (fw_guard 자가리셋 등)
  //     POR/EXT : 전원 재인가 / 리셋핀 — 배선·전원 문제
  {
    // ★ emlib 의 RMU_ResetCauseGet() 은 resetCauseMasks[] 로 "가짜" 비트를
    //   걸러내는데, 조합에 따라 통째로 0 이 나온다(실측: RSTCAUSE=0x00000000).
    //   원인 판별에는 걸러지지 않은 RAW 레지스터가 필요하다. 둘 다 찍는다.
    uint32_t raw = RMU->RSTCAUSE;
    uint32_t rc  = RMU_ResetCauseGet();
    app_log_info("[BOOT] RSTCAUSE raw=0x%08lX\n", (unsigned long)raw);
    app_log_info("[BOOT] RSTCAUSE=0x%08lX%s%s%s%s%s%s\n",
                 (unsigned long)rc,
                 (rc & RMU_RSTCAUSE_WDOGRST)    ? " WDOG"   : "",
                 (rc & RMU_RSTCAUSE_LOCKUPRST)  ? " LOCKUP" : "",
                 (rc & RMU_RSTCAUSE_SYSREQRST)  ? " SYSREQ" : "",
                 (rc & RMU_RSTCAUSE_PORST)      ? " POR"    : "",
                 (rc & RMU_RSTCAUSE_EXTRST)     ? " EXT"    : "",
                 ((rc & (RMU_RSTCAUSE_DVDDBOD | RMU_RSTCAUSE_AVDDBOD
                         | RMU_RSTCAUSE_DECBOD)) != 0U) ? " BOD" : "");
    // 다음 부팅에서 이번 사유가 섞이지 않도록 반드시 지운다.
    RMU_ResetCauseClear();
  }

  // ─── [롤백 가드] 최우선: 부트로더 init 후 즉시 probation/롤백 판단 ─────────
  //   TX는 코디네이터라 자체 OTA로 불량 펌웨어 설치 시 전 네트워크가 마비되고
  //   추가 OTA 경로까지 사라진다(단일 장애점). 롤백 가드가 특히 중요.
  //   fw_guard가 워치독(WDOG0, ~128s)도 함께 무장 → tick에서 fw_guard_feed_watchdog().
  //   네트워크 form보다 먼저 수행 — 롤백할 거면 form은 의미 없음.
  {
    int32_t btl = bootloader_init();
    if (btl != BOOTLOADER_OK) {
      app_log_error("bootloader_init FAILED: 0x%lX (rollback guard limited)\n", btl);
    }
    fw_guard_init();   // 롤백 트리거 시 복귀하지 않음
  }

  // ─── Security Key 설정 ───────────────────────────────────────────────────
  security_key_id = PSA_AES_KEY_ID;
  psa_key_attributes_t key_attr = psa_key_attributes_init();
  psa_status_t psa_status = psa_get_key_attributes(security_key_id, &key_attr);
  if (psa_status == PSA_ERROR_INVALID_HANDLE) {
    app_log_info("No PSA AES key found, creating one.\n");
    psa_reset_key_attributes(&key_attr);
    key_attr = psa_key_attributes_init();
    psa_set_key_id(&key_attr, security_key_id);
    psa_set_key_algorithm(&key_attr, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_usage_flags(&key_attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_type(&key_attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&key_attr, 128);

#ifdef PSA_KEY_LOCATION_SLI_SE_OPAQUE
    psa_set_key_lifetime(&key_attr,
                         PSA_KEY_LIFETIME_FROM_PERSISTENCE_AND_LOCATION(
                           PSA_KEY_LIFETIME_PERSISTENT,
                           PSA_KEY_LOCATION_SLI_SE_OPAQUE));
#else
#ifdef MBEDTLS_PSA_CRYPTO_STORAGE_C
    psa_set_key_lifetime(&key_attr,
                         PSA_KEY_LIFETIME_FROM_PERSISTENCE_AND_LOCATION(
                           PSA_KEY_LIFETIME_PERSISTENT,
                           PSA_KEY_LOCATION_LOCAL_STORAGE));
#else
    psa_set_key_lifetime(&key_attr,
                         PSA_KEY_LIFETIME_FROM_PERSISTENCE_AND_LOCATION(
                           PSA_KEY_LIFETIME_VOLATILE,
                           PSA_KEY_LOCATION_LOCAL_STORAGE));
#endif
#endif

    psa_status = psa_import_key(&key_attr,
                                security_key.contents,
                                (size_t)EMBER_ENCRYPTION_KEY_SIZE,
                                &security_key_id);
    psa_reset_key_attributes(&key_attr);
    if (psa_status == PSA_SUCCESS) {
      app_log_info("Security key import OK, id: %lu\n", security_key_id);
    } else {
      app_log_error("Security key import FAIL: %ld\n", psa_status);
    }
  } else {
    psa_reset_key_attributes(&key_attr);   // 리소스 해제
    app_log_info("PSA AES key exists, reusing.\n");
  }

  em_status = emberSetPsaSecurityKey(security_key_id);
  (void)em_status;

  // ─── Network 초기화 ───────────────────────────────────────────────────────
  em_status = emberNetworkInit();
  app_log_info("emberNetworkInit: 0x%02X\n", em_status);

  if (em_status == EMBER_NOT_JOINED) {
    // 최초 부팅 or NVM3 초기화 후 → 네트워크 형성
    form_network();
  } else if (em_status == EMBER_SUCCESS) {
    // NVM3에서 복원 중 — NETWORK_UP 콜백에서 emberPermitJoining(0xFF) 호출.
    // SDK 규정: emberPermitJoining()은 NETWORK_UP 이후에만 호출 가능.
    // RX는 5초 간격 재시도를 하므로 NETWORK_UP까지의 간격을 충분히 커버함.
    app_log_info("Resuming saved network. NodeID=0x%04X\n", emberGetNodeId());
  } else {
    // 복원 실패 — 강제 형성 폴백
    app_log_error("emberNetworkInit err: 0x%02X. Forming new network.\n", em_status);
    form_network();
  }

  // ─── OTA 상태 초기화 ─────────────────────────────────────────────────────
  ota_state        = OTA_IDLE;
  gbl_image_size   = 0;
  gbl_write_offset = 0;
  ota_target_node  = EMBER_NULL_NODE_ID;

  // ─── Bootloader Storage 초기화 ──────────────────────────────────────────
  bootloader_storage_init();

  // ─── I2C Slave 초기화 ────────────────────────────────────────────────────
  obc_i2c_slave_init();
  obc_spi_slave_init();   // [IWSB] SPI 슬레이브 병행 초기화

  app_log_info("Master init complete.\n");
  app_log_info("OTA via AP_OBC I2C: erase(0x03) → stream(0x02) → START(0x04,target).\n");

#if defined(EMBER_AF_PLUGIN_BLE)
  bleConnectionInfoTableInit();
#endif
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

// ─── [진단] 하드폴트 포착 ───────────────────────────────────────────────────
//   startup 파일의 기본 HardFault_Handler 는 무한루프라, 폴트가 나면 128 초 뒤
//   워치독으로 죽는다 → RSTCAUSE 가 WDOG 로 나와 "tick 굶주림" 과 구분되지 않는다.
//   여기서 가로채 폴트 사실과 복귀 주소(스택의 PC)를 남기고 즉시 재부팅한다.
//   콘솔이 RTT(메모리 링버퍼)라 폴트 컨텍스트에서도 기록이 남는다.
void HardFault_Handler(void)
{
  uint32_t *sp;
  __asm volatile ("tst lr, #4      \n"
                  "ite eq          \n"
                  "mrseq %0, msp   \n"
                  "mrsne %0, psp   \n" : "=r" (sp));
  // 예외 스택 프레임: [0]R0 [1]R1 [2]R2 [3]R3 [4]R12 [5]LR [6]PC [7]xPSR
  app_log_error("\n[FAULT] HardFault! PC=0x%08lX LR=0x%08lX xPSR=0x%08lX\n",
                (unsigned long)sp[6], (unsigned long)sp[5], (unsigned long)sp[7]);
  app_log_error("[FAULT] CFSR=0x%08lX HFSR=0x%08lX MMFAR=0x%08lX BFAR=0x%08lX\n",
                (unsigned long)SCB->CFSR, (unsigned long)SCB->HFSR,
                (unsigned long)SCB->MMFAR, (unsigned long)SCB->BFAR);
  // RTT 는 메모리 링버퍼라 flush 개념이 없다 — 바로 재부팅해도 기록은 남는다.
  NVIC_SystemReset();   // SYSREQ 로 재부팅 → 다음 RSTCAUSE 가 WDOG 가 아님으로 구분됨
}

static void form_network(void)
{
  EmberNetworkParameters params;
  memset(&params, 0, sizeof(params));
  params.radioChannel = (uint8_t)emberGetDefaultChannel();
  params.panId        = MASTER_NETWORK_PAN_ID;
  params.radioTxPower = MASTER_TX_POWER;

  EmberStatus st = emberFormNetwork(&params);
  if (st == EMBER_SUCCESS) {
    g_home_channel = params.radioChannel;   // ★ 복귀 기준점 확정(여기서만 정한다)
    app_log_info("Network FORMED on ch=%d, PAN=0x%04X\n",
                 params.radioChannel, params.panId);
    emberPermitJoining(0xFF);
  } else {
    app_log_error("emberFormNetwork FAILED: 0x%02X\n", st);
  }
}

static void bootloader_storage_init(void)
{
  // bootloader_init() 는 emberAfInitCallback 초반(롤백 가드)에서 이미 수행됨.
  BootloaderStorageSlot_t slot_info;
  int32_t ret = bootloader_getStorageSlotInfo(0, &slot_info);
  if (ret != BOOTLOADER_OK) {
    app_log_error("getStorageSlotInfo FAILED: 0x%lX\n", ret);
    return;
  }
  app_log_info("Storage slot 0: addr=0x%08lX, len=%lu bytes\n",
               slot_info.address, slot_info.length);

  // 커스텀 보드(버튼 없음): 지상 모드 제거. SLOT0에 잔여 이미지가 있어도
  //   무시하고 항상 IDLE로 시작 → AP_OBC가 erase(0x03)→stream(0x02)→START(0x04)로
  //   전 과정 제어. (지상모드 무한 진입/START 거부 버그 방지)
  ota_state = OTA_IDLE;
  app_log_info("OTA ready. Waiting for AP_OBC I2C commands (erase→stream→start).\n");
}

/**************************************************************************//**
 * SPI Slave 초기화 — OBC와의 통신 (I2C 와 병행)
 *
 *  [프로토콜] I2C 와 동일한 명령/프레임을 쓴다.
 *    · 명령 전송 : OBC 가 CS Low → 명령 바이트들 전송 → CS High
 *                  (수신 바이트는 obc_rx_buffer 에 쌓이고 CS High 에서 처리 요청)
 *    · 데이터 회수: OBC 가 0x06 을 보낸 뒤(수 ms 대기) 더미 바이트를 클럭하면
 *                  그 클럭에 맞춰 스테이징된 프레임이 MISO 로 나간다.
 *
 *  [SPI 슬레이브의 원리적 제약]
 *    슬레이브는 스스로 전송을 시작할 수 없다(클럭이 마스터 것이므로).
 *    따라서 "사이클 완료 플래그"는 프레임 앞 8바이트 상태 헤더로 전달하고,
 *    OBC 가 폴링해서 읽어 가는 방식이 된다(iq_capture.h 프레임 정의 참조).
 *****************************************************************************/
static void obc_spi_slave_init(void)
{
  CMU_ClockEnable(cmuClock_GPIO, true);
  CMU_ClockEnable(OBC_SPI_CMU_CLOCK, true);

  // MISO 만 출력, 나머지는 마스터가 구동하므로 입력.
  // ★ 반드시 풀 저항을 건다(플로팅 금지).
  //   OBC 미연결 상태(지상 eval 보드)나 케이블 단선 시 이 핀들이 뜨면, 노이즈가
  //   CS 엣지로 잡히고 CLK 노이즈가 쓰레기 바이트를 USART2 로 밀어 넣는다.
  //   그 결과 존재하지도 않는 OBC 명령이 실행된다(예: 가짜 0x05 → 혼자 미션 시작
  //   → 측정 채널을 돌아다녀 RX 가 join 하지 못함).
  //   CS 는 비활성이 High 이므로 풀업, CLK/MOSI 는 SPI mode0 유휴가 Low 이므로 풀다운.
  //   ★ MOSI 는 풀저항을 걸지 않는다(공식 spidrv / 공식 예제 모두 gpioModeInput).
  //     풀저항을 걸면 마스터가 그 선을 잡고 있는 힘과 저항분배로 겨루게 되어
  //     DC 레벨이 문턱 쪽으로 끌려오고, 무선 송신 순간의 노이즈만으로도 문턱을
  //     넘나들어 MOSI 에만 좁은 스파이크가 찍힌다(실측 확인).
  //     CLK/CS 는 미연결 시 오동작(허위 클럭·허위 CS)을 막아야 하므로 풀을 유지한다.
  GPIO_PinModeSet(OBC_SPI_MOSI_PORT, OBC_SPI_MOSI_PIN, gpioModeInput, 0);   // 풀 없음
  GPIO_PinModeSet(OBC_SPI_MISO_PORT, OBC_SPI_MISO_PIN, gpioModePushPull,  0);
  GPIO_PinModeSet(OBC_SPI_CLK_PORT,  OBC_SPI_CLK_PIN,  gpioModeInputPull, 0);  // 풀다운
  GPIO_PinModeSet(OBC_SPI_CS_PORT,   OBC_SPI_CS_PIN,   gpioModeInputPull, 1);  // 풀업

  USART_InitSync_TypeDef init = USART_INITSYNC_DEFAULT;
  init.master    = false;              // ★ 슬레이브
  // 슬레이브에서는 이 값이 실제 속도를 정하지 않는다(클럭은 마스터가 공급).
  //   설계 기준값으로만 둔다. 실제 상한은 두 가지에 걸린다:
  //     1) 하드웨어: 동기 슬레이브 최대 = HFPERCLK/8 = 38.4MHz/8 = 4.8 MHz
  //     2) 소프트웨어: 바이트마다 ISR 이 돈다. 2MHz = 바이트당 4us 인데, 라디오
  //        ISR 이 선점하면 그 사이 수신 바이트가 밀려 오버런(RXOF)이 날 수 있다.
  //        USART RX 버퍼가 2단이라 ~12us 까지는 버티지만 여유가 크지 않다.
  //   → 오버런은 조용히 프레임을 깨뜨리므로 아래에서 RXOF 를 세어 spi_stat 로 본다.
  init.baudrate  = 2000000;            // 2 MHz 기준
  init.msbf      = true;               // MSB first (일반적인 SPI 기본)
  init.clockMode = usartClockMode0;    // CPOL=0, CPHA=0
  init.enable    = usartDisable;
  USART_InitSync(OBC_SPI_PERIPHERAL, &init);

  // MOSI=RX, MISO=TX, CLK, CS 를 모두 같은 LOC 로 라우팅.
  // ★ 신호마다 LOC 가 다르다(슬레이브라 MOSI/MISO 방향이 반대이므로) — 위 주석 참조.
  OBC_SPI_PERIPHERAL->ROUTELOC0 = (OBC_SPI_RX_LOC  << _USART_ROUTELOC0_RXLOC_SHIFT)
                                | (OBC_SPI_TX_LOC  << _USART_ROUTELOC0_TXLOC_SHIFT)
                                | (OBC_SPI_CLK_LOC << _USART_ROUTELOC0_CLKLOC_SHIFT)
                                | (OBC_SPI_CS_LOC  << _USART_ROUTELOC0_CSLOC_SHIFT);
  OBC_SPI_PERIPHERAL->ROUTEPEN  = USART_ROUTEPEN_RXPEN | USART_ROUTEPEN_TXPEN
                                | USART_ROUTEPEN_CLKPEN | USART_ROUTEPEN_CSPEN;

  // ※ CSMA(=CS 에 따른 모드 전환)는 마스터 전용 기능이라 슬레이브에선 건드리지
  //   않는다. CS 핀이 라우팅되어 있으면 CS Low 인 동안에만 시프트가 일어난다.
  //   트랜잭션 종료(CS High)는 아래 GPIO 인터럽트로 감지한다.

  USART_IntClear(OBC_SPI_PERIPHERAL, _USART_IF_MASK);
  USART_IntEnable(OBC_SPI_PERIPHERAL, USART_IEN_RXDATAV | USART_IEN_RXOF);
  NVIC_ClearPendingIRQ(OBC_SPI_CLK_IRQn);
  NVIC_EnableIRQ(OBC_SPI_CLK_IRQn);

  USART_Enable(OBC_SPI_PERIPHERAL, usartEnable);

  // ─── 송신 DMA 기동 ────────────────────────────────────────────────────────
  //   실패해도 치명적이지 않다(기존 인터럽트 방식으로 자동 폴백). 다만 그 경우
  //   대량 전송에서 바이트 결손이 생길 수 있으므로 로그로 분명히 남긴다.
  Ecode_t dma_init_status = DMADRV_Init();
  Ecode_t dma_alloc_status = ECODE_EMDRV_DMADRV_NOT_INITIALIZED;
  if (dma_init_status == ECODE_EMDRV_DMADRV_OK
      || dma_init_status == ECODE_EMDRV_DMADRV_ALREADY_INITIALIZED) {
    dma_alloc_status = DMADRV_AllocateChannel(&spi_dma_ch, NULL);
  }
  if ((dma_init_status == ECODE_EMDRV_DMADRV_OK
       || dma_init_status == ECODE_EMDRV_DMADRV_ALREADY_INITIALIZED)
      && dma_alloc_status == ECODE_EMDRV_DMADRV_OK) {
    spi_dma_ok = true;
    obc_spi_dma_idle();     // 배치가 없는 동안은 유휴 바이트를 반복 송신
  } else {
    spi_dma_ok = false;
    app_log_error("SPI: DMA unavailable (init=0x%lX alloc=0x%lX)"
                  " — falling back to IRQ TX (byte loss possible)\n",
                  (unsigned long)dma_init_status,
                  (unsigned long)dma_alloc_status);
    obc_spi_prime_tx();
  }


  // CS 상승 에지 = 트랜잭션 종료 → 명령 해석 요청.
  GPIOINT_Init();
  GPIOINT_CallbackRegister(OBC_SPI_CS_PIN, obc_spi_cs_cb);
  GPIO_ExtIntConfig(OBC_SPI_CS_PORT, OBC_SPI_CS_PIN, OBC_SPI_CS_PIN,
                    true /*rising*/, false /*falling*/, true /*enable*/);

  app_log_info("SPI Slave initialized (USART0 LOC%u: MOSI=PA0 MISO=PA1 CLK=PA2 CS=PA3)\n",
               (unsigned)OBC_SPI_TX_LOC);
}

/**************************************************************************//**
 * SPI Slave IRQ — 바이트 단위 전이중 처리
 *
 *  SPI 는 한 바이트를 주고받을 때마다 RXDATAV 가 뜬다.
 *   · 받은 바이트: 명령 버퍼(obc_rx_buffer)에 축적. CS 가 High 로 올라가면
 *     (= 트랜잭션 종료) obc_cmd_ready 를 세워 tick 에서 해석한다.
 *   · 보낼 바이트: 스테이징된 리드백 프레임을 순서대로 밀어낸다.
 *  ※ I2C 경로와 동일한 버퍼/명령 처리를 공유하므로 동작이 일관된다.
 *****************************************************************************/
void OBC_SPI_IRQHandler(void)
{
  uint32_t flags = USART_IntGet(OBC_SPI_PERIPHERAL);

  // ★ 수신 오버런: ISR 이 제때 못 비워 바이트를 잃었다는 뜻이고, 그러면 프레임이
  //   통째로 밀린다(마스터는 알 방법이 없다). 세어 두고 spi_stat 로 확인한다.
  if (flags & USART_IF_RXOF) {
    spi_dbg_overrun++;
    USART_IntClear(OBC_SPI_PERIPHERAL, USART_IF_RXOF);
  }

  while (OBC_SPI_PERIPHERAL->STATUS & USART_STATUS_RXDATAV) {
    uint8_t rx = (uint8_t)OBC_SPI_PERIPHERAL->RXDATA;
    spi_dbg_bytes++;                 // [진단] 클럭이 실제로 들어오는지 확인용
    if (spi_dbg_first_rx == 0xFFFFU) spi_dbg_first_rx = rx;

    // 수신: 명령 바이트 축적(오버플로는 버림).
    if (obc_rx_len < sizeof(obc_rx_buffer)) {
      obc_rx_buffer[obc_rx_len++] = rx;
    }

    // 송신: DMA 가 담당하므로 여기서 TXDATA 를 건드리면 안 된다(이중 기록).
    //   DMA 실패로 폴백한 경우에만 소프트웨어로 채운다.
    if (!spi_dma_ok) {
      OBC_SPI_PERIPHERAL->TXDATA = obc_next_tx_byte(&spi_tx_idx, &spi_tx_done);
    }
  }

  USART_IntClear(OBC_SPI_PERIPHERAL, flags);
}

/**************************************************************************//**
 * CS 상승 에지 콜백 = 트랜잭션 종료 → 받은 명령을 tick 에서 해석하도록 요청.
 *   USART 에는 "CS 해제" 이벤트가 없으므로 GPIO 인터럽트로 감지한다.
 *****************************************************************************/
static void obc_spi_cs_cb(uint8_t intNo)
{
  (void)intNo;
  spi_dbg_cs_edges++;              // [진단] CS 엣지가 잡히는지 확인용
  if (obc_rx_len > 0U) {
    spi_dbg_last_len = obc_rx_len;
    spi_dbg_last_cmd = obc_rx_buffer[0];
    spi_dbg_frames++;
  }
  if (obc_rx_len > 0U) {
    obc_cmd_len   = obc_rx_len;   // ★ 길이를 걸어 둔 뒤에 rx_len 을 되돌린다
    obc_cmd_ready = true;         // tick 에서 process_obc_command() 가 해석
  }
  obc_rx_len = 0;
  obc_spi_prime_tx();       // (폴백 모드 전용) 다음 트랜잭션 선적재
}

/**************************************************************************//**
 * I2C Slave 초기화 — OBC와의 통신
 *****************************************************************************/
static void obc_i2c_slave_init(void)
{
  CMU_ClockEnable(cmuClock_GPIO, true);
  CMU_ClockEnable(cmuClock_I2C0, true);

  GPIO_PinModeSet(OBC_I2C_SDA_PORT, OBC_I2C_SDA_PIN, gpioModeWiredAndPullUpFilter, 1);
  GPIO_PinModeSet(OBC_I2C_SCL_PORT, OBC_I2C_SCL_PIN, gpioModeWiredAndPullUpFilter, 1);

  OBC_I2C_PERIPHERAL->ROUTELOC0 = OBC_I2C_SDA_LOC | OBC_I2C_SCL_LOC;
  OBC_I2C_PERIPHERAL->ROUTEPEN  = I2C_ROUTEPEN_SDAPEN | I2C_ROUTEPEN_SCLPEN;

  I2C_Init_TypeDef i2c_init = I2C_INIT_DEFAULT;
  i2c_init.enable = false;
  I2C_Init(OBC_I2C_PERIPHERAL, &i2c_init);

  I2C_SlaveAddressSet(OBC_I2C_PERIPHERAL, (uint8_t)(OBC_I2C_SLAVE_ADDR << 1));
  I2C_SlaveAddressMaskSet(OBC_I2C_PERIPHERAL, 0xFE);

  I2C_IntClear(OBC_I2C_PERIPHERAL, _I2C_IF_MASK);
  I2C_IntEnable(OBC_I2C_PERIPHERAL,
                I2C_IEN_ADDR     |
                I2C_IEN_RXDATAV  |
                I2C_IEN_SSTOP    |
                I2C_IEN_BUSERR   |
                I2C_IEN_ARBLOST |
                I2C_IEN_ACK      |   // [IQ] read 방향: master 가 우리 바이트 ACK → 다음 바이트
                I2C_IEN_NACK);       // [IQ] read 방향: master NACK → 읽기 종료
  NVIC_ClearPendingIRQ(I2C0_IRQn);
  NVIC_EnableIRQ(I2C0_IRQn);

  I2C_Enable(OBC_I2C_PERIPHERAL, true);
  app_log_info("I2C Slave initialized at 0x%02X (SDA=PC10/LOC16, SCL=PC11/LOC14)\n",
               OBC_I2C_SLAVE_ADDR);
}

/**************************************************************************//**
 * I2C0 IRQ 핸들러 — 검증된 동작 코드 기준 (write 수신 전용)
 *
 * 호스트는 write 트랜잭션만 사용한다(read 미사용).
 *   - ADDR: 주소 바이트 소비 후 ACK, rx_len 리셋
 *   - RXDATAV: STATUS가 valid인 동안 while 루프로 전부 드레인하며 ACK
 *   - SSTOP: 마지막 바이트가 RXDATA로 시프트되어 들어올 시간을 짧게 확보한 뒤
 *            남은 바이트를 완전히 드레인 → obc_cmd_ready 세팅
 *            (EFR32 I2C 슬레이브의 'STOP 직전 마지막 바이트 누락' 문제 회피)
 * BUSERR / ARBLOST: ABORT 후 상태 초기화.
 *****************************************************************************/
void I2C0_IRQHandler(void)
{
  uint32_t flags = I2C_IntGet(OBC_I2C_PERIPHERAL);

  // ─── 버스 에러 / 중재 실패 → 즉시 복구 ──────────────────────────────────
  if (flags & (I2C_IF_BUSERR | I2C_IF_ARBLOST)) {
    OBC_I2C_PERIPHERAL->CMD = I2C_CMD_ABORT;
    I2C_IntClear(OBC_I2C_PERIPHERAL, flags);
    obc_rx_len  = 0;
    i2c_is_read = false;
    return;
  }

  // ─── 주소 프레임 수신 (bit0 = R/W: 1=master read) ───────────────────────
  if (flags & I2C_IF_ADDR) {
    uint8_t abyte = OBC_I2C_PERIPHERAL->RXDATA;   // 주소+R/W 바이트 소비
    i2c_is_read = (abyte & 0x01U) != 0U;
    I2C_IntClear(OBC_I2C_PERIPHERAL, I2C_IFC_ADDR);
    obc_rx_len = 0;                          // 방향 무관 리셋(잔여 write 데이터 방지)
    if (i2c_is_read) {
      // [IQ read] 스테이징된 프레임의 첫 바이트를 로드(없으면 0xFF 패딩).
      i2c_tx_idx = 0;
      OBC_I2C_PERIPHERAL->TXDATA = obc_next_tx_byte(&i2c_tx_idx, &i2c_tx_done);
    }
    OBC_I2C_PERIPHERAL->CMD = I2C_CMD_ACK;   // 주소 ACK
  }

  // ─── [IQ read] master 가 직전 바이트를 ACK → 다음 바이트 전송 ────────────
  if (flags & I2C_IF_ACK) {
    I2C_IntClear(OBC_I2C_PERIPHERAL, I2C_IFC_ACK);
    if (i2c_is_read) {
      OBC_I2C_PERIPHERAL->TXDATA = obc_next_tx_byte(&i2c_tx_idx, &i2c_tx_done);
    }
  }
  // ─── [IQ read] master NACK → 더 보낼 필요 없음(방향 플래그는 STOP 에서 리셋) ─
  //   ※ 여기서 i2c_is_read 를 지우면 곧이어 오는 SSTOP 이 이 트랜잭션을 write 로
  //     오인해 잔여 obc_rx_len 으로 명령을 중복 처리한다 → 지우지 않는다.
  if (flags & I2C_IF_NACK) {
    I2C_IntClear(OBC_I2C_PERIPHERAL, I2C_IFC_NACK);
  }

  // ─── write 경로(검증된 동작, 변경 없음): 수신 바이트 드레인 ──────────────
  //   read 중엔 RXDATAV 가 뜨지 않으므로 이 루프는 실행되지 않는다.
  while (OBC_I2C_PERIPHERAL->STATUS & I2C_STATUS_RXDATAV) {
    if (obc_rx_len < sizeof(obc_rx_buffer)) {
      obc_rx_buffer[obc_rx_len++] = OBC_I2C_PERIPHERAL->RXDATA;
    } else {
      (void)OBC_I2C_PERIPHERAL->RXDATA;   // 버퍼 풀 → 폐기
    }
    OBC_I2C_PERIPHERAL->CMD = I2C_CMD_ACK;
  }

  // ─── STOP 조건 ─────────────────────────────────────────────────────────
  if (flags & I2C_IF_SSTOP) {
    // write 트랜잭션 종료 시에만 마지막 바이트 확보/드레인(read 종료는 스킵).
    if (!i2c_is_read) {
      for (volatile int i = 0; i < 100; i++) {
        if (OBC_I2C_PERIPHERAL->STATUS & I2C_STATUS_RXDATAV) break;
      }
      while (OBC_I2C_PERIPHERAL->STATUS & I2C_STATUS_RXDATAV) {
        if (obc_rx_len < sizeof(obc_rx_buffer)) {
          obc_rx_buffer[obc_rx_len++] = OBC_I2C_PERIPHERAL->RXDATA;
        } else {
          (void)OBC_I2C_PERIPHERAL->RXDATA;
        }
      }
      if (obc_rx_len > 0) {
        obc_cmd_len   = obc_rx_len;   // SPI 경로와 동일하게 길이를 걸어 둔다
        obc_cmd_ready = true;
      }
      // ※ obc_rx_len 은 여기서 건드리지 않는다 — ADDR 에서 이미 리셋되며,
      //   검증된 I2C write 경로의 동작을 그대로 보존하기 위함.
    }
    I2C_IntClear(OBC_I2C_PERIPHERAL, I2C_IFC_SSTOP);
    i2c_is_read = false;   // 트랜잭션 종료 → 방향 리셋
  }

  // 처리하지 않은 잔여 플래그 정리
  I2C_IntClear(OBC_I2C_PERIPHERAL,
               flags & ~(I2C_IFC_ADDR | I2C_IFC_SSTOP | I2C_IFC_ACK | I2C_IFC_NACK));
}
