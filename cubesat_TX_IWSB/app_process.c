/***************************************************************************//**
 * @file
 * @brief app_process.c — Master (Sink / OTA Server) — Coordinator Polling OTA
 *
 * [자동 복구 설계]
 *   C-2: slave_table NVM3 영속화 — TX 리셋 후 device_id 목록 자동 복원.
 *        node_id 는 ID_ANNOUNCE 수신 후 갱신 (node_id = NULL_NODE 인 슬레이브는 폴링 제외).
 *   C-3: poll 사이클 완료 = poll_cycle_count >= count_pollable_slaves().
 *        slave 수에 무관하게 항상 정확한 사이클 간격 유지.
 *   H-4: GBL write 전에 storage slot 경계 검사.
 *   M-1: bootload 콜백 status 0x08 매직넘버 제거.
 *   M-2: OTA 진행률 ota_tx_progress_pct 세션 간 리셋.
 *   M-3: volatile obc_rx_buffer → 로컬 복사 후 처리.
 *   L-1: 전송 진행률 100% 정확히 출력 (endIndex+1 기준).
 *******************************************************************************
 * SPDX-License-Identifier: Zlib
 ******************************************************************************/

#include PLATFORM_HEADER
#include "stack/include/ember.h"
#include "em_chip.h"
#include "app_log.h"
#include "sl_app_common.h"
#include "app_framework_common.h"
#include "sl_simple_led_instances.h"
#include "app_process.h"
#include "app_init.h"
#include <stdlib.h>
#include <string.h>
#include "sl_sleeptimer.h"
#include "btl_interface.h"
#include "btl_interface_storage.h"
#include "ota-unicast-bootloader-server.h"
#include "nvm3_default.h"
#include "fw_guard.h"
#include "em_usart.h"
#include "sl_power_manager.h"   // [IQ] 측정 중 sleep 차단(RX 와 타이밍 정합)
#include "sl_iostream_usart_vcom_config.h"   // 콘솔 주변장치(USART2) 참조
#include "iq_capture.h"   // [IQ] RAIL IQ 캡처 코어 + IQ 리포트 프로토콜

// [디버깅] VCOM(USART0) TX가 다 빠질 때까지 대기 → 로그 즉시 표시(sleep로 갇힘 방지)
//   ★ 비행 안전: 무한 대기 금지. VCOM 이 비활성이거나 USART0 클럭이 꺼진 빌드에서는
//     TXC 가 영원히 서지 않아 이 자리에서 완전히 멈춘다(재부팅 외 복구 불가).
//     최대 대기 횟수를 둬서 어떤 경우에도 반드시 빠져나오게 한다.
//     (115200bps 기준 1바이트 ≈ 87us. 100k 회전이면 넉넉히 상한을 넘긴다.)
//   ★ USART0 은 이제 OBC SPI 슬레이브가 쓴다. 콘솔은 USART2(VCOM)로 옮겼으므로
//     여기서도 USART0 을 보면 안 된다(SPI 상태를 읽게 되어 무의미).
static inline void log_flush(void)
{
  for (uint32_t guard = 0; guard < 100000U; guard++) {
    if (SL_IOSTREAM_USART_VCOM_PERIPHERAL->STATUS & USART_STATUS_TXC) return;
  }
}

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
// ─── AP_OBC I2C 명령 코드 (호스트 sdrc_I2C.c 와 동일) ────────────────────────
#define OBC_CMD_DATA          0x01   // 데이터/상태 (1바이트)
#define OBC_CMD_FW_UPDATE     0x02   // FW 이미지 스트리밍 패킷
#define OBC_CMD_ERASE         0x03   // 스토리지 슬롯 erase
#define OBC_CMD_START         0x04   // OTA 트리거 [cc][target] target:0=TX자체,1~4=RX
#define OBC_CMD_IQ_START      0x05   // IQ 측정 캠페인 트리거 [cc][dur_s] (dur_s초 동안 반복)
#define OBC_CMD_IQ_READ       0x06   // IQ 레코드 1건 리드백 준비(이후 OBC 가 I2C read)
#define OBC_CMD_DUMMY         0x00   // 호스트 dummy write (무시)

#define OBC_TARGET_TX_SELF    0x00   // START target=0 → TX 자체 펌웨어 설치

// ─── FW 스트리밍 패킷 포맷 (호스트 i2c_FW_buf_t 와 동일) ─────────────────────
//   [cc:1][packet_num:2 LE][len:1][buf_data:N]
//   호스트 버전에 따라 N=120 또는 128 → 청크 크기를 가정하지 않고 누적 오프셋으로 처리.
//   data_len 상한만 128로 둔다(버퍼 136B = 4 헤더 + 최대 132 수용).
#define FW_PKT_HEADER_SIZE    4U      // cc + packet_num(2) + len(1)
#define FW_MAX_CHUNK          128U    // 한 패킷 최대 데이터 (120/128 모두 수용)
#define FW_LAST_PACKET_NUM    0xFFFFU // 마지막 패킷 센티넬

#define CUSTOM_ENDPOINT          0x02

#define OTA_SLAVE_PREPARE_TIMEOUT_MS   10000U
#define OTA_BOOTLOAD_DELAY_MS          2000U
// 배포/부트로드 단계 무응답 상한 (플러그인이 완료 콜백을 끝내 안 부르는 경우 대비)
#define OTA_DISTRIBUTE_TIMEOUT_MS      300000U  // 5분: TX_INTERVAL 25ms로 ~50s 전송 + 재시도/드롭 여유
#define OTA_BOOTLOAD_WAIT_TIMEOUT_MS   30000U   // bootload 요청 완료 대기 상한

#define DEVICE_ID_NONE  0xFFU

#define POLL_RESPONSE_TIMEOUT_MS  1500U
#define POLL_CYCLE_INTERVAL_MS    5000U

// ─── [롤백 가드] 헬스 확인 타이밍 ────────────────────────────────────────────
//   TX는 코디네이터라 스택 up이 RX 존재와 무관(form은 혼자 성립) → 스푸리어스
//   롤백 위험 없음. 스택이 안정적으로 up 유지되면 healthy로 간주.
#define HEALTH_STABILITY_MS       15000U   // 스택 up 연속 유지 시 healthy 확정
#define BOOT_HEALTH_TIMEOUT_MS    180000U  // 이 시간 내 healthy 못 되면 probation시 self-reset

// OBC I2C 버퍼 크기 (app_init.c 와 동일해야 함)
#define OBC_LOCAL_BUF_SIZE  (128 + 8)

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void process_obc_command(void);
static void handle_cmd_fw_packet(const uint8_t *buf, uint16_t len);
static void handle_cmd_erase(const uint8_t *buf, uint16_t len);
static void handle_cmd_data(const uint8_t *buf, uint16_t len);
static void handle_cmd_start(const uint8_t *buf, uint16_t len);
static void handle_cmd_iq_start(const uint8_t *buf, uint16_t len);   // [IQ]
static void handle_cmd_iq_read(const uint8_t *buf, uint16_t len);    // [IQ]
static void send_slave_prepare_msg(EmberNodeId target);
static void ota_state_machine_tick(void);
static void start_ota_distribution(void);
static void bootload_req_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data);

static void    register_slave(uint8_t device_id, EmberNodeId node_id);
static EmberNodeId get_node_id_by_device_id(uint8_t device_id);

// ─── [C-2] NVM3 slave_table 영속화 ──────────────────────────────────────────
static void save_slave_table_nvm3(void);
static void load_slave_table_nvm3(void);

// ─── [C-3] 폴링 ──────────────────────────────────────────────────────────────
static uint8_t count_pollable_slaves(void);
static void poll_next_slave(void);
static void poll_tick(void);
static void poll_cycle_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data);

// -----------------------------------------------------------------------------
//                                Global Variables
// -----------------------------------------------------------------------------
EmberMessageOptions tx_options = EMBER_OPTIONS_ACK_REQUESTED | EMBER_OPTIONS_SECURITY_ENABLED;

extern volatile ota_master_state_t ota_state;
extern volatile uint8_t  obc_rx_buffer[];
extern volatile uint16_t obc_rx_len;
extern volatile bool     obc_cmd_ready;
extern volatile uint16_t obc_cmd_len;
extern uint32_t    gbl_image_size;
extern uint32_t    gbl_write_offset;
extern uint8_t     ota_image_tag;
extern EmberNodeId ota_target_node;

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
static uint32_t ota_timer_start     = 0;
static bool     slave_prepare_acked = false;

static sl_sleeptimer_timer_handle_t bootload_req_timer;
static bool bootload_req_pending = false;

// ─── [롤백 가드] 헬스 확인 상태 ──────────────────────────────────────────────
static bool     health_confirmed = false;
static bool     boot_tick_set    = false;
static uint32_t boot_tick        = 0;   // 부팅 기준 tick
static uint32_t stack_up_tick    = 0;   // 스택 up 시작 tick (0=down)


// ─── Slave 테이블 ─────────────────────────────────────────────────────────────
static SlaveEntry slave_table[MAX_SLAVES];

// ─── CLI로 선택된 OTA 타겟 ────────────────────────────────────────────────────
static uint8_t ota_target_device_id = DEVICE_ID_NONE;

// ─── FW 스트리밍 수신 상태 ────────────────────────────────────────────────────
// 호스트가 packet_num 0,1,2,...로 스트리밍하고 마지막 패킷은 0xFFFF로 표시.
// 마지막 패킷은 packet_num이 0xFFFF로 덮어써지므로 오프셋은 누적값으로 추적한다.
static uint16_t fw_next_seq = 0;   // 다음에 기대하는 packet_num (마지막 제외)

// ─── OTA prepare 재시도 (RX 오프라인 시 무한대기 방지) ───────────────────────
#define MAX_PREPARE_RETRIES   3U
static uint8_t  prepare_retries = 0;

// ─── [C-2] slave_table 초기화 완료 여부 (첫 tick에서 NVM3 로드) ───────────────
static bool slave_table_loaded = false;

// ─── [M-2] OTA 전송 진행률 (세션 간 리셋) ─────────────────────────────────────
static uint32_t ota_tx_progress_pct = 0;

// ─── [H-4] storage slot 크기 (PREPARE 시 1회 읽기) ───────────────────────────
static uint32_t storage_slot_size = 0;

// ─── [C-3] 폴링 상태 ─────────────────────────────────────────────────────────
static uint8_t  poll_idx              = 0;
static uint8_t  poll_cycle_count      = 0;   // 이번 사이클에서 처리된 slave 수
static uint8_t  poll_current_dev_id   = 0;   // 현재 대기 중인 slave device_id (로그용)
static bool     poll_waiting          = false;
static uint32_t poll_anchor           = 0;
static bool     poll_running          = false;
static sl_sleeptimer_timer_handle_t poll_cycle_timer;

// -----------------------------------------------------------------------------
//   RSSI 측정 로그 — 외부 SPI 플래시 (날개 전개 감지용)
//
//   설계 메모:
//   - append-only 링이 아닌 "정지형" 로그. 영역(2MB=262144 레코드)이 가득 차면
//     더 쓰지 않는다(측정 데이터 보존 우선). clear 로 비운 뒤 재사용.
//   - NOR 플래시는 1→0 만 가능 → 섹터 단위 erase 후 순차 기록. 레코드(8B)가
//     섹터(4096B)에 512개 정확히 들어가므로 레코드가 섹터 경계를 가로지르지 않음.
//   - 쓰기 커서(절대주소)는 NVM3 에 영속화 → 전원 차단/리셋 후에도 이어쓰기.
//   - bootloader_*RawStorage 만 사용(직접 SPI 드라이버 X) → OTA/golden 경로와 동일.
//   - 칩이 기대보다 작으면(<4MB) 로깅을 조용히 비활성화하여 golden 영역 침범을 원천차단.
// -----------------------------------------------------------------------------
static uint32_t s_rssi_write_addr = RSSI_LOG_BASE_ADDR;
static uint32_t s_rssi_page       = 4096;
static bool     s_rssi_ready      = false;

bool rssi_log_ready(void) { return s_rssi_ready; }

void rssi_log_init(void)
{
  s_rssi_ready = false;

  BootloaderStorageInformation_t info;
  bootloader_getStorageInfo(&info);
  if (info.info == NULL) {
    app_log_error("[RSSI] no storage info — logging DISABLED.\n");
    return;
  }
  s_rssi_page = info.info->pageSize ? info.info->pageSize : 4096;
  uint32_t part = info.info->partSize;

  // 칩 크기 검증: 우리 영역이 칩을 벗어나면 비활성(golden 보호 보험).
  if (part == 0 || (RSSI_LOG_BASE_ADDR + RSSI_LOG_REGION_SIZE) > part) {
    app_log_error("[RSSI] flash too small (part=%lu) — logging DISABLED.\n",
                  (unsigned long)part);
    return;
  }
  // 레코드가 섹터에 정수배로 들어가야 경계 처리가 단순.
  if (s_rssi_page == 0 || (s_rssi_page % sizeof(rssi_record_t)) != 0) {
    app_log_error("[RSSI] page(%lu) not multiple of record — logging DISABLED.\n",
                  (unsigned long)s_rssi_page);
    return;
  }

  // 쓰기 커서 복원(없거나 손상 시 영역 시작으로).
  uint32_t cursor = RSSI_LOG_BASE_ADDR;
  Ecode_t ec = nvm3_readData(nvm3_defaultHandle, NVM3_KEY_RSSI_LOG_CURSOR,
                             &cursor, sizeof(cursor));
  if (ec != ECODE_NVM3_OK
      || cursor < RSSI_LOG_BASE_ADDR
      || cursor > (RSSI_LOG_BASE_ADDR + RSSI_LOG_REGION_SIZE)
      || ((cursor - RSSI_LOG_BASE_ADDR) % sizeof(rssi_record_t)) != 0) {
    cursor = RSSI_LOG_BASE_ADDR;
  }
  s_rssi_write_addr = cursor;
  s_rssi_ready = true;
  app_log_info("[RSSI] log ready. page=%lu part=%lu count=%lu\n",
               (unsigned long)s_rssi_page, (unsigned long)part,
               (unsigned long)rssi_log_count());
}

uint32_t rssi_log_count(void)
{
  if (!s_rssi_ready) return 0;
  return (s_rssi_write_addr - RSSI_LOG_BASE_ADDR) / sizeof(rssi_record_t);
}

bool rssi_log_append(const rssi_record_t *rec)
{
  if (!s_rssi_ready || rec == NULL) return false;

  // 영역 가득 → 정지(덮어쓰지 않음). clear 후 재사용.
  if (s_rssi_write_addr + sizeof(rssi_record_t)
      > RSSI_LOG_BASE_ADDR + RSSI_LOG_REGION_SIZE) {
    return false;
  }

  // 섹터의 첫 레코드면 그 섹터를 먼저 erase(append-only NOR).
  if ((s_rssi_write_addr % s_rssi_page) == 0) {
    if (bootloader_eraseRawStorage(s_rssi_write_addr, s_rssi_page) != BOOTLOADER_OK) {
      app_log_error("[RSSI] erase fail @0x%lX\n", (unsigned long)s_rssi_write_addr);
      return false;
    }
  }
  if (bootloader_writeRawStorage(s_rssi_write_addr,
                                 (uint8_t *)rec, sizeof(*rec)) != BOOTLOADER_OK) {
    app_log_error("[RSSI] write fail @0x%lX\n", (unsigned long)s_rssi_write_addr);
    return false;
  }
  s_rssi_write_addr += sizeof(*rec);

  // 커서 영속화(전원안전). 측정은 간헐적이라 매 레코드 저장해도 부담 적음.
  nvm3_writeData(nvm3_defaultHandle, NVM3_KEY_RSSI_LOG_CURSOR,
                 &s_rssi_write_addr, sizeof(s_rssi_write_addr));
  return true;
}

bool rssi_log_read(uint32_t index, rssi_record_t *out)
{
  if (!s_rssi_ready || out == NULL || index >= rssi_log_count()) return false;
  uint32_t addr = RSSI_LOG_BASE_ADDR + index * sizeof(rssi_record_t);
  return (bootloader_readRawStorage(addr, (uint8_t *)out, sizeof(*out))
          == BOOTLOADER_OK);
}

void rssi_log_clear(void)
{
  s_rssi_write_addr = RSSI_LOG_BASE_ADDR;
  nvm3_writeData(nvm3_defaultHandle, NVM3_KEY_RSSI_LOG_CURSOR,
                 &s_rssi_write_addr, sizeof(s_rssi_write_addr));
  // 물리 erase 는 다음 append 시 섹터 단위로 lazy 수행 → 즉시 전체 erase 불필요.
}

// -----------------------------------------------------------------------------
//   RSSI 측정 캠페인 — TX(coordinator) 오케스트레이터
//
//   (송신원 t, 채널 ch) 라운드를 순회:
//     t = 0(=TX 자신), 1..MAX_SLAVES(=RX device_id),  ch = MEAS_CH_FIRST..LAST
//   각 라운드: MEAS_CMD 브로드캐스트 → 전 노드 ch 전환 → 송신원 비콘 N개 →
//              나머지 측정 → home 복귀 → 리포트 수집 → 외부 플래시 저장.
//   캠페인 동안 폴링은 일시 중단(emberAfTickCallback 에서 분기).
//   ※ 측정 채널 전환은 모두 tick 컨텍스트(메시지/타이머 콜백 아님)에서 수행.
// -----------------------------------------------------------------------------
typedef enum {
  MEAS_TX_IDLE = 0,
  MEAS_TX_ROUND_CMD,   // MEAS_CMD 브로드캐스트 송신(채널 전환은 아직 안 함)
  MEAS_TX_CMD_WAIT,    // ★ 방송이 실제 전파로 나갈 시간 확보 후 채널 전환
  MEAS_TX_ON_CHANNEL,  // 비콘 송신(t==TX) 또는 청취
  MEAS_TX_COLLECT      // home 복귀 후 리포트 수집
} meas_tx_state_t;

// ★ emberMessageSend() 는 큐에 넣고 즉시 반환한다(비동기). 송신이 실제로 끝나기
//   전에 채널을 바꾸면 그 브로드캐스트가 유실되거나 엉뚱한 채널로 나간다.
//   → 리스너들이 MEAS_CMD 를 못 받아 홈 채널에 남고, TX 만 측정 채널로 떠나
//     서로 만나지 못한다(측정 실패 + 부모 부재로 인식되어 재가입 유발).
//   CSMA/백오프까지 감안한 여유를 두고 채널을 전환한다.
#define MEAS_CMD_TX_MS   30U

static meas_tx_state_t meas_tx_state = MEAS_TX_IDLE;
static uint8_t   meas_tx_t        = 0;   // 현재 송신원 device_id (0=TX)
static uint8_t   meas_tx_ch       = 0;   // 현재 채널(리스트에서 선택된 실제 값)
static uint8_t   meas_tx_chi      = 0;   // 현재 채널 인덱스(meas_ch_list 안)
static uint8_t   meas_tx_seq      = 0;   // 라운드 시퀀스(증가)

// [IQ] 스윕할 채널 목록(축소판) — 21채널 전부 대신 대표 채널만.
//   한 스윕 레코드 수 = 20링크 × N채널. 링버퍼(IQ_RING_LEN-1)에 다 담기게 개수 조정.
//   채널 플랜: base 915MHz + 0.65MHz 간격 (rail_config.c) → 채널 N = 915 + N*0.65 MHz.
//   아래 5채널 = 915.0 / 918.25 / 921.5 / 924.75 / 928.0 MHz (915 대역 전체에 분포).
//   한 스윕 = 20링크 × 5채널 = 100 레코드(약 19초). 링버퍼(127칸)에 전부 수용.
//   원하는 채널로 자유롭게 바꿀 수 있음(0..20 범위).
//   ※ 채널 수를 늘리면 20×N ≤ IQ_RING_LEN-1(127) 을 유지할 것.
static const uint8_t meas_ch_list[] = { 0U, 5U, 10U, 15U, 20U };
#define MEAS_N_CHANNELS  ((uint8_t)(sizeof(meas_ch_list) / sizeof(meas_ch_list[0])))
static uint16_t  meas_tx_home     = 0;
static uint32_t  meas_tx_t0       = 0;   // 윈도/수집 시작 tick
static uint32_t  meas_tx_lastbcn  = 0;
static uint8_t   meas_tx_bcnsent  = 0;
static int32_t   meas_tx_rssi_sum = 0;   // TX 자신이 청취한 누적
static uint16_t  meas_tx_rssi_cnt = 0;
static int8_t    meas_tx_last_lqi = 0;

// [IQ] TX 자신이 청취자일 때의 캡처 버퍼(rx_id=0=TX).
static iq_sample_t meas_tx_iq_buf[IQ_SAMPLES_PER_LINK];
static uint16_t    meas_tx_iq_n   = 0;
static bool        meas_tx_iq_done = false;

// ─── [IQ] 연속 송신(TX stream) 상태 ─────────────────────────────────────────
//   ★ 안전 최우선: 스트림이 켜진 채로 남으면 그 채널을 계속 점유(재밍)한다.
//     따라서 stop 은 슬롯 종료·중단·OTA 선점 등 모든 경로에서 무조건 호출하고,
//     idempotent(이미 꺼져 있으면 무해)하게 만든다.
static bool meas_tx_streaming = false;

// ─── [안전장치] 송신 스트림 감시 타이머 ──────────────────────────────────────
//   스트림의 시작/정지는 tick 이 담당한다. 그런데 tick 이 오래 굶으면(예: CLI
//   iq_dump 가 수천 줄을 UART 로 뱉는 동안, 혹은 다른 긴 블로킹 작업) 스트림이
//   정지 시점을 놓치고 계속 송신한다. 그러면 Connect MAC 이 아무것도 보내지
//   못해 송신 큐가 가득 차고, 이후 모든 전송이 0x39(MAC_TRANSMIT_QUEUE_FULL)로
//   영구히 실패한다 — 실제로 발생했던 장애다.
//   tick 에 의존하지 않는 sleeptimer 콜백(인터럽트 문맥)으로 강제 정지시킨다.
#define MEAS_STREAM_GUARD_MS   400U   // 정상 스트림 길이(210ms)의 약 2배
static sl_sleeptimer_timer_handle_t meas_stream_guard;
static volatile bool meas_stream_guard_armed = false;
static volatile bool meas_stream_guard_fired = false;

static void meas_stream_guard_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  iq_stream_abort();               // 최소 조치: 송신만 즉시 멈춘다(ISR 안전)
  meas_stream_guard_fired = true;  // 채널 원복/수신 재개는 tick 이 수행
}

static void meas_tx_stream_start(uint8_t channel)
{
  if (meas_tx_streaming) return;
  // RAIL 레벨 PN9 스트림(스택 상태 검사 없음 — iq_capture.h 주석 참조).
  if (iq_stream_start(channel)) {
    meas_tx_streaming = true;
    // tick 이 굶어도 반드시 꺼지도록 감시 타이머 무장.
    if (sl_sleeptimer_start_timer_ms(&meas_stream_guard, MEAS_STREAM_GUARD_MS,
                                     meas_stream_guard_cb, NULL, 0, 0)
        == SL_STATUS_OK) {
      meas_stream_guard_armed = true;
    }
  } else {
    app_log_error("[MEAS] TX stream start failed (ch=%u)\n", (unsigned)channel);
  }
}

static void meas_tx_stream_stop(void)
{
  if (meas_stream_guard_armed) {
    sl_sleeptimer_stop_timer(&meas_stream_guard);
    meas_stream_guard_armed = false;
  }
  if (!meas_tx_streaming) return;
  // 중지 후 같은 측정 채널에서 수신 재개(슬롯 종료 시 호출측이 home 으로 복귀).
  iq_stream_stop(meas_tx_ch & 0x7FU);
  meas_tx_streaming = false;
}

// 자동 모드: 캠페인을 주기적으로 반복(날개 전개를 시간에 따라 추적).
//   [IQ 전환] 기본 OFF — 측정은 OBC 의 OBC_CMD_IQ_START(지속시간) 로 트리거한다.
static bool      meas_auto_enabled = false;   // 기본 OFF (OBC 트리거 기반)
static uint32_t  meas_last_end_tick = 0;      // 마지막 캠페인 종료 tick (간격 측정용)
static bool      meas_first_run     = true;   // 부팅 후 첫 자동 시작 즉시 허용

// [IQ] OBC 트리거 측정 윈도: 지정 지속시간 동안 캠페인을 백투백으로 반복.
static bool      iq_win_active   = false;
static uint32_t  iq_win_deadline = 0;         // 스윕 강제 중단 마감시한 tick

// ─── [IQ] 캠페인 동안 tick 고속 유지 ────────────────────────────────────────
//   ★ EM1 요구만으로는 부족하다. EM1 은 "깊은 잠(EM2)"만 막을 뿐, CPU 는 여전히
//     다음 인터럽트까지 얕은 잠을 잔다. TX 를 깨우는 것은 하트비트(2000ms)뿐이라
//     상태 전이 1회에 최대 2초가 걸린다 → 한 라운드(상태 4단계)에 최대 8초,
//     25라운드면 200초. 실측에서 30초 안에 3~4라운드밖에 못 돈 이유가 이것.
//   → 측정 동안만 MEAS_TICK_MS 주기 타이머로 CPU 를 강제로 깨워 상태머신이
//     설계대로(250ms 슬롯 / 500ms 수집) 진행하도록 보장한다.
//     (RX 의 미가입-시 1초 웨이크업 타이머와 같은 기법, 주기만 훨씬 짧게)
#define MEAS_TICK_MS   10U

static bool      meas_tx_no_sleep_req = false;
static sl_sleeptimer_timer_handle_t meas_tx_wake_timer;

static void meas_tx_wake_cb(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;   // 빈 콜백 — CPU 를 깨우는 것 자체가 목적
}

static void meas_tx_set_sleep_block(bool block)
{
  if (block && !meas_tx_no_sleep_req) {
    sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
    sl_sleeptimer_start_periodic_timer_ms(&meas_tx_wake_timer, MEAS_TICK_MS,
                                          meas_tx_wake_cb, NULL, 0, 0);
    meas_tx_no_sleep_req = true;
  } else if (!block && meas_tx_no_sleep_req) {
    sl_sleeptimer_stop_timer(&meas_tx_wake_timer);
    sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1);
    meas_tx_no_sleep_req = false;
  }
}

// 콜백에서 들어온 레코드를 tick 에서 flash 에 기록하기 위한 소형 큐
// (콜백 컨텍스트에서 flash erase/write 금지 → tick 에서 drain).
#define MEAS_RECQ_LEN 16U
static rssi_record_t    meas_recq[MEAS_RECQ_LEN];
static volatile uint8_t meas_recq_head = 0;
static volatile uint8_t meas_recq_tail = 0;

static void meas_tx_tick(void);   // 전방 선언(tick 콜백에서 호출)

static inline uint16_t meas_uptime_s(void)
{
  return (uint16_t)(sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count()) / 1000U);
}

static void meas_recq_push(uint8_t tx_id, uint8_t rx_id, uint8_t ch,
                           int8_t rssi, uint8_t lqi, uint8_t seq)
{
  uint8_t next = (uint8_t)((meas_recq_head + 1U) % MEAS_RECQ_LEN);
  if (next == meas_recq_tail) return;   // 가득 → 드롭(다음 라운드 재측정 가능)
  rssi_record_t *r = &meas_recq[meas_recq_head];
  r->tx_id   = tx_id; r->rx_id = rx_id; r->channel = ch;
  r->rssi    = rssi;  r->lqi   = lqi;   r->seq     = seq;
  r->tstamp_s = meas_uptime_s();
  meas_recq_head = next;
}

// ─────────────────────────────────────────────────────────────────────────────
//  [IQ] Master 집계 저장소 — 완성된 링크 IQ 레코드 링 버퍼 (OBC 가 I2C로 드레인)
//
//   경로: 청취 노드 → (프래그먼트 IQ_REPORT) → TX 조립(iq_asm) → 완성 시 링에 push.
//         OBC 가 OBC_CMD_IQ_READ(06) 한 번으로 쌓인 전체를 한 덩어리로 회수한다.
//   RAM: 레코드 261B × RING 32 ≈ 8.4KB (FG12 RAM 256KB → 여유).
//   IQ_RING_LEN 은 iq_capture.h 에 정의(리드백 버퍼 크기 계산과 공유).
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t          iq_batch_seq = 0;   // 배치 일련번호(리드백 헤더 buf[6])
static iq_record_t      iq_ring[IQ_RING_LEN];
static volatile uint8_t iq_ring_head = 0;   // push 위치
static volatile uint8_t iq_ring_tail = 0;   // pop 위치

// 청취자(rx_id)별 프래그먼트 조립 버퍼. rx_id: 0=TX, 1..MAX_SLAVES=RX.
static iq_record_t      iq_asm[MAX_SLAVES + 1U];
static uint8_t          iq_asm_mask[MAX_SLAVES + 1U];   // 수신한 frag 비트마스크
static uint8_t          iq_asm_nfrag[MAX_SLAVES + 1U];  // 기대 frag 수

static uint8_t iq_ring_count(void)
{
  return (uint8_t)((iq_ring_head - iq_ring_tail) & (uint8_t)(IQ_RING_LEN - 1U));
}

// 링을 즉시 비운다(배치 회수 포기 시 등).
void iq_ring_reset(void)
{
  iq_ring_tail = iq_ring_head;
}

// 완성 레코드를 링에 push(가득 차면 가장 오래된 것을 덮어써 최신 유지).
static void iq_ring_push(const iq_record_t *rec)
{
  iq_ring[iq_ring_head] = *rec;
  iq_ring_head = (uint8_t)((iq_ring_head + 1U) & (uint8_t)(IQ_RING_LEN - 1U));
  if (iq_ring_head == iq_ring_tail) {   // overrun → tail 전진(가장 오래된 폐기)
    iq_ring_tail = (uint8_t)((iq_ring_tail + 1U) & (uint8_t)(IQ_RING_LEN - 1U));
  }
}

// [I2C] 쌓인 IQ 레코드 "전부"를 한 프레임으로 직렬화하여 buf 에 채운다(app_init.c 호출).
//   프레임: [n_records:1] + 레코드들(각 IQ_REC_SIZE 고정)
//           레코드 = [tx_id][rx_id][ch][seq][n_samp][iq: IQ_SAMPLES_PER_LINK*4]
//           (유효 샘플은 n_samp 개, 나머지는 0 패딩 → 레코드 크기 고정으로 OBC 파싱 단순)
//   호출 후 링은 비워진다(전부 pop). 반환값 = 항상 IQ_READBACK_MAX(고정 길이).
//   ★ OBC 는 06 write 후 read 한 번으로 IQ_READBACK_MAX 바이트를 통째로 받고,
//     맨 앞 [n_records] 만큼만 파싱하면 된다(뒤는 0).
uint16_t iq_readback_drain_all(uint8_t *buf, uint16_t buf_size)
{
  if (buf == NULL || buf_size < IQ_READBACK_MAX) return 0;
  memset(buf, 0, IQ_READBACK_MAX);

  // ── 상태 헤더(사이클 완료 플래그) — iq_capture.h 프레임 정의 참조 ──────────
  buf[0] = IQ_FRAME_MAGIC;
  buf[1] = iq_mission_is_active();
  buf[2] = (uint8_t)iq_mission_sweeps_done();
  buf[3] = (uint8_t)IQ_MISSION_SWEEPS;
  buf[4] = iq_mission_batch_ready();
  // buf[5] = n_records  (아래에서 채움)

  uint8_t  n   = 0;
  uint16_t off = IQ_FRAME_HDR;
  while (iq_ring_count() > 0U && n < (uint8_t)(IQ_RING_LEN - 1U)) {
    const iq_record_t *r = &iq_ring[iq_ring_tail];
    iq_ring_tail = (uint8_t)((iq_ring_tail + 1U) & (uint8_t)(IQ_RING_LEN - 1U));

    uint8_t ns = (r->n_samp > IQ_SAMPLES_PER_LINK) ? (uint8_t)IQ_SAMPLES_PER_LINK
                                                   : r->n_samp;
    buf[off + 0] = r->tx_id;
    buf[off + 1] = r->rx_id;
    buf[off + 2] = r->channel;
    buf[off + 3] = r->seq;
    buf[off + 4] = ns;
    for (uint8_t k = 0; k < ns; k++) {
      uint8_t *p = &buf[off + IQ_REC_HDR + k * 4U];
      p[0] = (uint8_t)((uint16_t)r->iq[k].i & 0xFF);
      p[1] = (uint8_t)(((uint16_t)r->iq[k].i >> 8) & 0xFF);
      p[2] = (uint8_t)((uint16_t)r->iq[k].q & 0xFF);
      p[3] = (uint8_t)(((uint16_t)r->iq[k].q >> 8) & 0xFF);
    }
    off += IQ_REC_SIZE;                    // 고정 스트라이드(빈 샘플은 0 패딩)
    n++;
  }
  buf[5] = n;                              // 레코드 개수 기록
  buf[6] = ++iq_batch_seq;                 // 배치 일련번호 — OBC 가 새 배치인지 구분
  buf[7] = 0;                              // 예약
  // ★ 여기서 iq_batch_taken() 을 부르지 않는다.
  //   이 함수는 "링 → 프레임 버퍼로 옮겼다"는 뜻일 뿐, OBC 가 데이터를 정상
  //   수신했다는 뜻이 아니다. 회수 완료 판정은 OBC 가 0x06 을 보내줬을 때만
  //   내린다 — 그래야 OBC 가 읽고 검증하기 전에 덮어쓰지 않는다.
  return IQ_READBACK_MAX;
}

uint8_t iq_readback_count(void) { return iq_ring_count(); }

// [IQ] 청취자 프래그먼트 리포트 조립(콜백에서 호출).
//   payload: [1]seq [2]tx_id [3]ch [4]rx_id [5]frag_idx [6]n_frags [7]n_samp [8..]iq
static void meas_tx_on_iq_report(const uint8_t *p, uint8_t len)
{
  if (len < IQ_REPORT_HDR) return;
  uint8_t rx_id     = p[4];
  uint8_t frag_idx  = p[5];
  uint8_t n_frags   = p[6];
  uint8_t nsamp     = p[7];
  if (rx_id > MAX_SLAVES || frag_idx >= 8U || n_frags == 0U || n_frags > 8U) return;
  if ((uint16_t)(IQ_REPORT_HDR + nsamp * 4U) > len) return;   // 길이 정합성

  iq_record_t *a = &iq_asm[rx_id];
  if (frag_idx == 0U) {          // 새 조립 시작
    a->tx_id   = p[2];
    a->rx_id   = rx_id;
    a->channel = p[3];
    a->seq     = p[1];
    a->n_samp  = 0U;
    iq_asm_mask[rx_id]  = 0U;
    iq_asm_nfrag[rx_id] = n_frags;
  }
  // 조립 일관성: seq 가 바뀌었으면(라운드 교체) 이전 조각 폐기 후 재시작.
  if (a->seq != p[1] || iq_asm_nfrag[rx_id] != n_frags) {
    return;   // 어긋난 조각 무시(다음 frag_idx==0 에서 재동기화)
  }

  uint16_t off = (uint16_t)frag_idx * IQ_FRAG_SAMPLES;
  for (uint8_t k = 0; k < nsamp && (off + k) < IQ_SAMPLES_PER_LINK; k++) {
    const uint8_t *s = &p[IQ_REPORT_HDR + k * 4U];
    a->iq[off + k].i = (int16_t)((uint16_t)s[0] | ((uint16_t)s[1] << 8));
    a->iq[off + k].q = (int16_t)((uint16_t)s[2] | ((uint16_t)s[3] << 8));
  }
  uint16_t end = off + nsamp;
  if (end > IQ_SAMPLES_PER_LINK) end = IQ_SAMPLES_PER_LINK;
  if (end > a->n_samp) a->n_samp = (uint8_t)end;

  iq_asm_mask[rx_id] |= (uint8_t)(1U << frag_idx);
  uint8_t full = (uint8_t)((1U << n_frags) - 1U);
  if ((iq_asm_mask[rx_id] & full) == full) {   // 모든 조각 도착 → 링에 확정
    iq_ring_push(a);
    iq_asm_mask[rx_id] = 0U;   // 재조립 방지
  }
}

// 리스너의 MEAS_REPORT 적재(콜백에서 호출). p: [1]seq [2]tx_id [3]ch [4]rx_id [5]rssi [6]lqi [7]cnt
__attribute__((unused))
static void meas_tx_on_report(const uint8_t *p, uint8_t len)
{
  if (len < 8) return;
  meas_recq_push(p[2], p[4], p[3], (int8_t)p[5], p[6], p[1]);
}

// TX 가 측정 채널에서 RX 비콘을 들었을 때 누적(콜백에서 호출).
static void meas_tx_on_beacon(const EmberIncomingMessage *m)
{
  if (m->length < 4) return;
  if (meas_tx_state == MEAS_TX_ON_CHANNEL
      && m->payload[2] == meas_tx_t
      && (m->payload[3] & 0x7FU) == (meas_tx_ch & 0x7FU)) {
    meas_tx_rssi_sum += m->rssi;
    meas_tx_last_lqi  = (int8_t)m->lqi;
    meas_tx_rssi_cnt++;
  }
}

bool meas_campaign_active(void) { return meas_tx_state != MEAS_TX_IDLE; }

static bool meas_tx_ota_busy(void);   // 전방 선언

void meas_campaign_start(void)
{
  if (meas_tx_state != MEAS_TX_IDLE) return;     // 이미 진행 중
  if (meas_tx_ota_busy()) {
    app_log_info("[MEAS] OTA busy — campaign deferred.\n");
    return;                                      // OTA 중엔 측정 금지
  }
  // [IQ 전환] IQ 측정은 외부 플래시를 쓰지 않는다(결과는 RAM 링→OBC I2C).
  //   rssi_log_ready() 여부와 무관하게 진행. (RSSI 병행 로깅은 여전히 flash 사용)
  if (!iq_capture_ready()) {
    app_log_error("[MEAS] IQ capture not ready (RAIL handle) — campaign aborted.\n");
    return;
  }
  meas_tx_t   = MEAS_TX_DEVICE_ID;   // 첫 송신원 = TX 자신
  meas_tx_chi = 0;                   // 첫 채널 = 리스트[0]
  meas_tx_ch  = meas_ch_list[0];
  meas_tx_seq++;
  meas_tx_set_sleep_block(true);     // 캠페인 동안 tick 고속 유지(RX 와 타이밍 정합)
  meas_tx_state = MEAS_TX_ROUND_CMD;
  app_log_info("[MEAS] campaign start (t=0..%u, channels=%u)\n",
               (unsigned)MAX_SLAVES, (unsigned)MEAS_N_CHANNELS);
}

static void meas_tx_send_cmd(void)
{
  uint8_t cmd[5] = {
    MSG_TYPE_MEAS_CMD, meas_tx_seq, meas_tx_t,
    (uint8_t)(meas_tx_ch | MEAS_BAND_915), MEAS_N_BEACONS
  };
  emberMessageSend(EMBER_BROADCAST_ADDRESS, CUSTOM_ENDPOINT, 0,
                   sizeof(cmd), cmd, EMBER_OPTIONS_NONE);
}

// [구] 단발 비콘 송신 — 연속 송신(TX stream) 전환으로 더 이상 쓰지 않는다.
//   (참고용 보존: RSSI 병행 측정을 되살릴 때 재사용 가능)
__attribute__((unused))
static void meas_tx_send_beacon(uint8_t idx)
{
  uint8_t msg[5] = {
    MSG_TYPE_MEAS_BEACON, meas_tx_seq, meas_tx_t,
    (uint8_t)(meas_tx_ch | MEAS_BAND_915), idx
  };
  emberMessageSend(EMBER_BROADCAST_ADDRESS, CUSTOM_ENDPOINT, 0,
                   sizeof(msg), msg, EMBER_OPTIONS_NONE);
}

// 다음 (t,ch) 라운드로 진행(채널 먼저 스윕, 끝나면 송신원 증가). 끝이면 IDLE.
static void meas_tx_advance(void)
{
  if (meas_tx_chi + 1U < MEAS_N_CHANNELS) {
    meas_tx_chi++;                     // 리스트 내 다음 채널
    meas_tx_ch = meas_ch_list[meas_tx_chi];
  } else {
    meas_tx_chi = 0;                   // 채널 다 돌면 처음으로
    meas_tx_ch  = meas_ch_list[0];
    meas_tx_t++;                       // 다음 송신원
  }
  meas_tx_seq++;
  if (meas_tx_t > MAX_SLAVES) {     // 0..MAX_SLAVES 송신원 모두 완료
    meas_tx_state      = MEAS_TX_IDLE;
    meas_tx_set_sleep_block(false);   // 저전력 복귀
    meas_last_end_tick = sl_sleeptimer_get_tick_count();   // 자동 재시작 간격 기준
    // ※ 여기서 찍던 rssi_log_count() 는 (지금 비활성인) 외부플래시 RSSI 로그
    //   개수라 IQ 와 무관하게 항상 0 이어서 오해를 샀다 → IQ 개수로 교체.
    app_log_info("[MEAS] sweep complete. IQ records=%u\n",
                 (unsigned)iq_readback_count());
  } else {
    meas_tx_state = MEAS_TX_ROUND_CMD;
  }
}

// OTA 진행 중 여부(측정과 배타). tick 의 ota_busy 와 동일 기준.
static bool meas_tx_ota_busy(void)
{
  return (ota_state != OTA_IDLE
          && ota_state != OTA_FW_READY_MANUAL
          && ota_state != OTA_FW_READY);
}

// 캠페인을 즉시 중단하고 홈 채널로 안전 복귀(OTA 가 끼어들 때 호출).
// 캠페인 즉시 중단 + 홈 채널 안전 복귀. (OTA 선점 / 마감시한 초과 공통 경로)
static void meas_tx_abort(void)
{
  if (meas_tx_state == MEAS_TX_IDLE) return;
  meas_tx_stream_stop();   // ★ 최우선: 스트림이 켜진 채 남으면 채널을 계속 점유한다
  emberSetRadioChannelExtended(meas_tx_home, false);   // 측정 채널에 갇히지 않도록 복귀(home 기본 0)
  meas_tx_state      = MEAS_TX_IDLE;
  meas_tx_set_sleep_block(false);   // 저전력 복귀
  meas_last_end_tick = sl_sleeptimer_get_tick_count();
  // 사유는 호출측이 별도로 로그한다(OTA 선점/마감시한 등) — 여기선 사실만 기록.
  app_log_info("[MEAS] campaign aborted. returned to home channel.\n");
}

// 자동 모드: 스택 up + 비-OTA + 간격 경과 시 다음 캠페인 시작.
static void meas_auto_tick(void)
{
  if (!meas_auto_enabled || meas_campaign_active()) return;
  if (!emberStackIsUp()) return;
  uint32_t now = sl_sleeptimer_get_tick_count();
  if (meas_first_run
      || sl_sleeptimer_tick_to_ms(now - meas_last_end_tick) >= (uint32_t)MEAS_AUTO_GAP_S * 1000U) {
    meas_first_run = false;
    meas_campaign_start();
  }
}

// 자동 모드 on/off (CLI 에서 호출).
void meas_auto_set(bool en)
{
  meas_auto_enabled = en;
  if (en) meas_first_run = true;   // 켜면 즉시 1회 시작 허용
}
bool meas_auto_get(void) { return meas_auto_enabled; }

// ─────────────────────────────────────────────────────────────────────────────
//  [IWSB 미션] OBC 가 명령 하나(0x05)를 주면 전 과정을 자동 수행한다.
//
//   1) 먼저 수집 버퍼가 비어 있는지 확인 — 안 비었으면 아직 회수 안 된 데이터가
//      있다는 뜻이므로 시작하지 않는다(덮어써서 잃는 것을 방지).
//   2) 비어 있으면 스윕을 IQ_MISSION_SWEEPS(100) 회 반복한다.
//   3) ★ 한 스윕이 끝날 때마다 그 배치를 OBC 로 내려보낸다. 100회분(약 2.6MB)을
//      한꺼번에 담을 RAM 이 없으므로 "한 스윕 = 한 배치" 단위로 흘려보낸다.
//   4) 배치를 OBC 가 가져갈 때까지 다음 스윕을 시작하지 않는다(유실 방지).
//      단 OBC 가 영영 안 가져가면 미션이 멈추므로 대기 상한을 둔다.
//
//   ※ EFR32 는 SPI 슬레이브라 스스로 전송을 시작할 수 없다. 따라서 "내려보낸다"는
//     것은 "배치를 즉시 회수 가능한 상태로 준비해 두고, OBC 가 폴링해서 가져간다"
//     는 의미다. 준비 여부/남은 스윕 수는 프레임 헤더에 실어 OBC 가 알 수 있다.
// ─────────────────────────────────────────────────────────────────────────────
//   ※ IQ_MISSION_SWEEPS 는 app_process.h 에 정의(리드백 프레임 헤더와 공유).
#define IQ_SWEEP_DEADLINE_S    60U      // 스윕 1회가 이 시간을 넘으면 강제 중단
#define IQ_BATCH_PICKUP_MS     30000U   // 배치 회수 대기 상한(초과 시 버리고 계속)
#define IQ_START_WAIT_MS       180000U  // 스윕 시작 대기 상한(OTA/조인 대기) — 초과 시 미션 포기

// ★ sl_sleeptimer_ms_to_tick() 의 인자는 uint16_t 다(최대 65535 ms).
//   이 값을 65 초 넘게 올리면 인자가 조용히 잘려 마감시한이 엉뚱하게 짧아지고
//   모든 스윕이 즉시 중단된다. 컴파일 단계에서 막는다.
//   (PICKUP/START_WAIT 는 tick_to_ms 비교라 64비트 내부연산 → 영향 없음)
_Static_assert(IQ_SWEEP_DEADLINE_S * 1000U <= 65535U,
               "IQ_SWEEP_DEADLINE_S too large for sl_sleeptimer_ms_to_tick(uint16_t)");

static bool     iq_mission_active = false;
static uint16_t iq_sweeps_done    = 0;      // 완료한 스윕 수
static bool     iq_batch_ready    = false;  // 회수 대기 중인 배치가 있는가
static uint32_t iq_batch_tick     = 0;      // 배치가 준비된 시각(대기 상한 판정용)
static uint32_t iq_wait_tick      = 0;      // 스윕 시작을 기다리기 시작한 시각
static bool     iq_wait_logged    = false;  // 대기 사유 로그 1회만 출력

// [지상 테스트 전용] 배치 자동 회수.
//   실제 OBC 는 0x06 폴링으로 배치를 가져가므로 사람 개입이 없다. CLI 시험에서는
//   사람이 iq_dump 를 대신 쳐야 하는데 100 스윕이면 100번이라 현실적이지 않다.
//   이 플래그를 켜면 tick 이 배치를 자동 회수하고 스윕당 한 줄만 요약 출력한다.
//   ※ 비행 기본값은 OFF — OBC 가 회수하기 전에 데이터를 버리면 안 되기 때문이다.
static bool iq_auto_drain = false;
void iq_auto_drain_set(bool on) { iq_auto_drain = on; }
bool iq_auto_drain_get(void)    { return iq_auto_drain; }

uint8_t  iq_mission_is_active(void) { return iq_mission_active ? 1U : 0U; }
uint16_t iq_mission_sweeps_done(void) { return iq_sweeps_done; }
uint8_t  iq_mission_batch_ready(void) { return iq_batch_ready ? 1U : 0U; }

// OBC(또는 CLI)가 배치를 실제로 가져갔을 때 호출 — 다음 스윕 진행을 허가한다.
void iq_batch_taken(void)
{
  iq_batch_ready = false;
}

// 스윕 1회 시작 시도. 전제조건이 안 맞으면 아무것도 바꾸지 않고 false 를 돌려주어
// 다음 tick 에서 재시도하게 한다. (조건을 미리 확인하므로 tick 마다 로그가 도배되지 않음)
static bool iq_start_sweep(uint32_t now)
{
  if (!emberStackIsUp() || meas_tx_ota_busy() || !iq_capture_ready()) return false;
  meas_campaign_start();
  if (!meas_campaign_active()) return false;   // 시작 실패(다른 캠페인 진행 중 등)

  iq_win_deadline = now + sl_sleeptimer_ms_to_tick(IQ_SWEEP_DEADLINE_S * 1000U);
  iq_win_active   = true;
  iq_wait_logged  = false;
  return true;
}

// [IWSB 미션 시작] OBC 0x05 / CLI iq_start 진입점.
//   dur_s 인자는 하위호환을 위해 남겨두되 미션 모드에서는 쓰지 않는다.
//
//   ★ 정책: "5 는 어떤 상황에서든 미션 1회를 온전히 수행한다."
//     - 링에 남은 이전 데이터가 있으면 → 버리고 새로 시작(회수 실패한 찌꺼기).
//     - 아직 스윕을 시작할 수 없는 상황(OTA 중/미조인)이면 → 거부하지 않고 예약해
//       두고, 조건이 풀리는 즉시 iq_win_tick() 이 자동으로 시작한다.
void iq_meas_trigger(uint16_t dur_s)
{
  (void)dur_s;

  if (iq_mission_active) {
    app_log_info("[IWSB] already running (%u/%u sweeps)\n",
                 (unsigned)iq_sweeps_done, (unsigned)IQ_MISSION_SWEEPS);
    return;
  }
  // 이전 미션의 잔여 레코드는 새 미션 데이터와 섞이면 안 되므로 비우고 시작한다.
  uint8_t stale = iq_readback_count();
  if (stale > 0U) {
    app_log_error("[IWSB] discarding %u stale records from previous mission\n",
                  (unsigned)stale);
    iq_ring_reset();
  }

  uint32_t now = sl_sleeptimer_get_tick_count();
  iq_mission_active = true;
  iq_sweeps_done    = 0;
  iq_batch_ready    = false;
  iq_win_active     = false;
  iq_wait_tick      = now;
  iq_wait_logged    = false;

  app_log_info("[IWSB] mission start — %u sweeps, batch after each\n",
               (unsigned)IQ_MISSION_SWEEPS);
  if (!iq_start_sweep(now)) {
    app_log_info("[IWSB] waiting for radio (stack=%u, ota=%u) — will start automatically.\n",
                 (unsigned)(emberStackIsUp() ? 1U : 0U),
                 (unsigned)(meas_tx_ota_busy() ? 1U : 0U));
    iq_wait_logged = true;
  }
}

// 미션 중단(수동/오류).
void iq_mission_abort(void)
{
  if (!iq_mission_active) return;
  iq_mission_active = false;
  iq_win_active     = false;
  meas_tx_abort();
  app_log_info("[IWSB] mission aborted at sweep %u\n", (unsigned)iq_sweeps_done);
}

// [IWSB 미션] 스윕 완료 감시 → 배치 준비 → 회수 확인 → 다음 스윕.
//   ※ 캠페인 진행 중에도 호출되어야 마감시한 검사가 동작하므로,
//     tick 콜백에서 campaign active/inactive 양쪽 경로 모두에서 부른다.
static void iq_win_tick(void)
{
  if (!iq_win_active && !iq_mission_active) return;

  uint32_t now = sl_sleeptimer_get_tick_count();

  // ── 스윕 진행 중: 마감시한만 감시 ──────────────────────────────────────────
  if (meas_campaign_active()) {
    if (iq_win_active && (int32_t)(now - iq_win_deadline) >= 0) {
      meas_tx_abort();
      iq_win_active = false;
      app_log_error("[IWSB] sweep deadline exceeded — aborted. records=%u\n",
                    (unsigned)iq_readback_count());
    }
    return;
  }

  // ── 스윕이 방금 끝났다면 배치를 만들고 "즉시 스테이징"한다 ────────────────
  //   ★ OBC 는 0x05 로 시작만 시키고, 회수 준비 명령은 보내지 않는다.
  //     따라서 스윕이 끝나는 즉시 우리가 알아서 리드백 프레임을 만들어 둔다.
  //   ★ 레코드가 0개여도 반드시 스테이징한다.
  //     "스윕 1회 = 플래그 1회 = 0x06 1회" 라는 핸드셰이크를 깨지 않기 위함이다.
  //     (0개일 때 건너뛰면 OBC 는 그 스윕에 대한 플래그를 영영 못 보고, 스윕
  //      번호만 조용히 건너뛰어 OBC 쪽 상태기계와 어긋난다.)
  if (iq_win_active) {
    iq_win_active  = false;
    iq_sweeps_done++;
    uint8_t nrec   = iq_readback_count();
    obc_stage_iq_readback();   // 명령 없이 자동 준비(링 → 프레임 버퍼)
    iq_batch_ready = true;
    iq_batch_tick  = now;
    iq_wait_tick   = now;      // 다음 스윕 시작 대기 상한은 이 시점부터 센다
    app_log_info("[IWSB] sweep %u/%u done. batch=%u records (staged)\n",
                 (unsigned)iq_sweeps_done, (unsigned)IQ_MISSION_SWEEPS,
                 (unsigned)nrec);
  }

  if (!iq_mission_active) return;

  // ── [지상 테스트] 자동 회수 모드: OBC 없이 사람 대신 0x06 을 대신 쳐 준다 ──
  if (iq_batch_ready && iq_auto_drain) {
    app_log_info("[IWSB] auto-drain sweep %u/%u\n",
                 (unsigned)iq_sweeps_done, (unsigned)IQ_MISSION_SWEEPS);
    iq_batch_taken();
  }

  // ── 배치가 회수(0x06)될 때까지 진행하지 않는다 ────────────────────────────
  //   ★ 이 검사는 반드시 "미션 종료 판정"보다 앞에 있어야 한다.
  //     뒤에 두면 마지막(100번째) 배치를 OBC 가 안 가져갔을 때 종료 판정에서
  //     먼저 return 되어 아래 타임아웃에 영영 도달하지 못하고 미션이 멈춘다.
  if (iq_batch_ready) {
    if (sl_sleeptimer_tick_to_ms(now - iq_batch_tick) >= IQ_BATCH_PICKUP_MS) {
      // OBC 가 끝내 안 가져감 → 미션이 멈추지 않도록 버리고 진행.
      app_log_error("[IWSB] batch not picked up in %us — dropping, continuing.\n",
                    (unsigned)(IQ_BATCH_PICKUP_MS / 1000U));
      iq_ring_reset();
      iq_batch_ready = false;
    } else {
      return;   // 아직 0x06 대기 중
    }
  }

  // ── 미션 종료 판정 (배치까지 회수된 뒤에만 도달한다) ──────────────────────
  if (iq_sweeps_done >= IQ_MISSION_SWEEPS) {
    iq_mission_active = false;
    app_log_info("[IWSB] mission complete — %u sweeps.\n",
                 (unsigned)IQ_MISSION_SWEEPS);
    return;
  }

  // ── 다음 스윕 시작 ────────────────────────────────────────────────────────
  //   시작 못 하면(OTA 중/미조인) 다음 tick 에 재시도. 단 무한 대기는 막는다.
  if (iq_start_sweep(now)) {
    iq_wait_tick = now;
    return;
  }
  if (!iq_wait_logged) {
    app_log_info("[IWSB] sweep deferred (stack=%u, ota=%u) — retrying.\n",
                 (unsigned)(emberStackIsUp() ? 1U : 0U),
                 (unsigned)(meas_tx_ota_busy() ? 1U : 0U));
    iq_wait_logged = true;
  }
  if (sl_sleeptimer_tick_to_ms(now - iq_wait_tick) >= IQ_START_WAIT_MS) {
    iq_mission_active = false;
    app_log_error("[IWSB] mission gave up — could not start a sweep in %us (at sweep %u).\n",
                  (unsigned)(IQ_START_WAIT_MS / 1000U), (unsigned)iq_sweeps_done);
  }
}

static void meas_tx_drain_recq(void)
{
  // tick 당 일부만 flash 기록(섹터 erase 로 인한 장시간 stall 방지).
  uint8_t budget = 2;
  while (budget-- && meas_recq_tail != meas_recq_head) {
    rssi_log_append(&meas_recq[meas_recq_tail]);
    meas_recq_tail = (uint8_t)((meas_recq_tail + 1U) % MEAS_RECQ_LEN);
  }
}

static void meas_tx_tick(void)
{
  meas_tx_drain_recq();
  uint32_t now = sl_sleeptimer_get_tick_count();

  switch (meas_tx_state) {
    case MEAS_TX_IDLE:
      return;

    case MEAS_TX_ROUND_CMD:
      meas_tx_send_cmd();                       // 전 노드에 라운드 통지(브로드캐스트)
      meas_tx_home     = emberGetRadioChannel();
      meas_tx_rssi_sum = 0; meas_tx_rssi_cnt = 0; meas_tx_last_lqi = 0;
      meas_tx_bcnsent  = 0;
      meas_tx_iq_n     = 0; meas_tx_iq_done = false;   // [IQ] 라운드 초기화
      // ★ 채널 전환은 다음 상태에서 — 방송이 실제로 나갈 시간을 준다.
      meas_tx_t0    = now;
      meas_tx_state = MEAS_TX_CMD_WAIT;
      return;

    case MEAS_TX_CMD_WAIT:
      // 브로드캐스트가 전파로 나갈 시간(MEAS_CMD_TX_MS) 경과 후 채널 전환.
      if (sl_sleeptimer_tick_to_ms(now - meas_tx_t0) < MEAS_CMD_TX_MS) {
        return;
      }
      if (emberSetRadioChannelExtended(meas_tx_ch & 0x7FU, false) != EMBER_SUCCESS) {
        meas_tx_advance();                      // 채널 전환 실패 → 라운드 스킵
        return;
      }
      meas_tx_t0      = now;
      meas_tx_lastbcn = now;
      meas_tx_state   = MEAS_TX_ON_CHANNEL;
      return;

    case MEAS_TX_ON_CHANNEL: {
      uint32_t el = sl_sleeptimer_tick_to_ms(now - meas_tx_t0);
      // ─── TX 가 송신원이면: 슬롯 동안 연속 송신(TX stream) ───────────────────
      //   짧은 비콘 대신 끊김 없는 스트림을 쏘아, 리스너가 어느 시점에 캡처해도
      //   반드시 신호가 담기게 한다.
      if (meas_tx_t == MEAS_TX_DEVICE_ID) {
        if (!meas_tx_streaming && el >= MEAS_STREAM_START_MS
            && el < MEAS_STREAM_STOP_MS) {
          meas_tx_stream_start(meas_tx_ch & 0x7FU);
        } else if (meas_tx_streaming && el >= MEAS_STREAM_STOP_MS) {
          meas_tx_stream_stop();                 // 슬롯 종료 전 반드시 정지
        }
      }
      // [IQ] TX 가 청취자면 슬롯 안에서 IQ 버스트 1회 캡처(rx_id=0=TX).
      //   스트림이 한창일 때(MEAS_CAPTURE_AT_MS) 잡는다.
      if (meas_tx_t != MEAS_TX_DEVICE_ID && !meas_tx_iq_done
          && el >= MEAS_CAPTURE_AT_MS && iq_capture_ready()) {
        // 캡처 후에도 같은 측정 채널에서 수신을 이어간다(슬롯 종료 시 home 복귀).
        meas_tx_iq_n = iq_capture_burst(meas_tx_ch & 0x7FU,
                                        meas_tx_ch & 0x7FU,
                                        meas_tx_iq_buf,
                                        IQ_SAMPLES_PER_LINK, IQ_DECIM,
                                        IQ_CAPTURE_TIMEOUT_MS);
        meas_tx_iq_done = true;
      }
      if (el >= MEAS_SLOT_MS) {
        meas_tx_stream_stop();                                 // 보험: 무조건 정지
        emberSetRadioChannelExtended(meas_tx_home, false);     // home 복귀(스택 RX 재개)
        // [IQ] TX 가 청취자였고 샘플을 얻었으면 자기 레코드를 링에 확정.
        if (meas_tx_t != MEAS_TX_DEVICE_ID && meas_tx_iq_n > 0) {
          iq_record_t rec;
          rec.tx_id   = meas_tx_t;
          rec.rx_id   = MEAS_TX_DEVICE_ID;
          rec.channel = (uint8_t)(meas_tx_ch | MEAS_BAND_915);
          rec.seq     = meas_tx_seq;
          rec.n_samp  = (meas_tx_iq_n > IQ_SAMPLES_PER_LINK)
                          ? (uint8_t)IQ_SAMPLES_PER_LINK : (uint8_t)meas_tx_iq_n;
          memcpy(rec.iq, meas_tx_iq_buf, (size_t)rec.n_samp * sizeof(iq_sample_t));
          iq_ring_push(&rec);
        }
        meas_tx_t0    = now;
        meas_tx_state = MEAS_TX_COLLECT;
      }
      return;
    }

    case MEAS_TX_COLLECT:
      // 이 동안 리스너 MEAS_REPORT 가 콜백→큐→drain 으로 flash 에 저장됨.
      if (sl_sleeptimer_tick_to_ms(now - meas_tx_t0) >= MEAS_COLLECT_MS) {
        meas_tx_advance();
      }
      return;
  }
}

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

/**************************************************************************//**
 * Incoming message callback
 *****************************************************************************/
void emberAfIncomingMessageCallback(EmberIncomingMessage *message)
{
  if (message->endpoint != CUSTOM_ENDPOINT
      && message->endpoint != SL_SENSOR_SINK_ENDPOINT) {
    return;
  }
  if (message->length < 1) return;

  uint8_t msg_type = message->payload[0];

  // ─── RX device_id 알림 ──────────────────────────────────────────────────
  if (msg_type == MSG_TYPE_ID_ANNOUNCE) {
    if (message->length < 2) {
      app_log_error("ID_ANNOUNCE: too short from 0x%04X\n", message->source);
      return;
    }
    register_slave(message->payload[1], message->source);
    // [EUI64] RX가 EUI-64를 함께 보냈으면 g_eui64_map[] 채우기용으로 출력.
    //   출력 순서 = map 입력 순서(LSB first) → 그대로 복사-붙여넣기 가능.
    if (message->length >= 10) {
      app_log_info("  RX id=%d EUI64 (map order, LSB first): {",
                   message->payload[1]);
      for (int i = 2; i <= 9; i++) {
        app_log_info("0x%02X%s", message->payload[i], (i < 9) ? ", " : "");
      }
      app_log_info("}\n");
    }
    log_flush();   // [디버깅] slave 등록/EUI64 로그 즉시 표시
    return;
  }

  // ─── RX 폴링 응답 ────────────────────────────────────────────────────────
  if (msg_type == MSG_TYPE_POLL_RESPONSE) {
    if (message->length < 3) return;
    uint8_t dev_id = message->payload[1];
    uint8_t fw_ver = message->payload[2];
    for (uint8_t i = 0; i < MAX_SLAVES; i++) {
      if (slave_table[i].registered && slave_table[i].device_id == dev_id) {
        slave_table[i].online     = true;
        slave_table[i].fw_version = fw_ver;
        break;
      }
    }
    // [텔레메트리] RX guard 상태(4번째 바이트): golden 보유/probation/슬롯(A/B).
    //   active 슬롯이 A↔B로 바뀌면 golden이 새로 덮어써졌다는 뜻(로그 없는 RX 검증용).
    if (message->length >= 4) {
      uint8_t g = message->payload[3];
      app_log_info("[POLL] RX id=%d (node=0x%04X) online. fw=0x%02X | golden=%s(%c) prob=%d\n",
                   dev_id, message->source, fw_ver,
                   (g & 0x01) ? "Y" : "N",
                   (g & 0x04) ? 'B' : 'A',
                   (g & 0x02) ? 1 : 0);
    } else {
      app_log_info("[POLL] RX id=%d (node=0x%04X) online. fw=0x%02X\n",
                   dev_id, message->source, fw_ver);
    }
    poll_waiting = false;   // 응답 수신 → 대기 해제
    return;
  }

  // ─── [RSSI 측정] RX 비콘 청취(측정 채널) / 리스너 리포트 수신 ───────────────
  if (msg_type == MSG_TYPE_MEAS_BEACON) {
    meas_tx_on_beacon(message);
    return;
  }
  if (msg_type == MSG_TYPE_MEAS_REPORT) {
    meas_tx_on_report(message->payload, message->length);
    return;
  }
  // ─── [IQ 측정] 청취 노드의 IQ 프래그먼트 리포트 → 조립 후 링에 확정 ──────────
  if (msg_type == MSG_TYPE_IQ_REPORT) {
    meas_tx_on_iq_report(message->payload, (uint8_t)message->length);
    return;
  }

  // ─── Slave OTA 준비 ACK ─────────────────────────────────────────────────
  if (msg_type == MSG_TYPE_OTA_PREPARE_ACK) {
    app_log_info("Slave 0x%04X OTA prepare ACK.\n", message->source);
    slave_prepare_acked = true;
    return;
  }

  // ─── 일반 센서 데이터 ───────────────────────────────────────────────────
  if (message->endpoint != SL_SENSOR_SINK_ENDPOINT) return;
  if ((tx_options & EMBER_OPTIONS_SECURITY_ENABLED)
      && !(message->options & EMBER_OPTIONS_SECURITY_ENABLED)) {
    return;
  }

  app_log_info("RX data from 0x%04X:", message->source);
  for (int j = 0; j < message->length; j++) {
    app_log_info(" %02X", message->payload[j]);
  }

  int32_t  temperature             = 0;
  uint32_t humidity                = 0;
  bool     temperature_is_negative = false;
  int32_t  temperature_decimal     = 0;
  uint32_t humidity_decimal        = 0;

  temperature = (int32_t)emberFetchLowHighInt32u(message->payload);
  humidity    = emberFetchLowHighInt32u(message->payload + 4);

  if (temperature < 0) temperature_is_negative = true;
  temperature_decimal = abs(temperature) - (abs(temperature) / 1000) * 1000;
  temperature = abs(temperature / 1000);
  humidity_decimal = humidity - (humidity / 1000) * 1000;
  humidity = humidity / 1000;

  app_log_info(" Temp: %s%ld.%03ldC Hum: %lu.%03lu%%\n",
               (temperature_is_negative ? "-" : "+"),
               temperature, temperature_decimal,
               humidity, humidity_decimal);
}

/**************************************************************************//**
 * Message sent callback
 *****************************************************************************/
void emberAfMessageSentCallback(EmberStatus status, EmberOutgoingMessage *message)
{
  (void)message;
  if (status != EMBER_SUCCESS) {
    app_log_info("TX fail: 0x%02X\n", status);
  }
}

/**************************************************************************//**
 * Stack status callback
 *****************************************************************************/
void emberAfStackStatusCallback(EmberStatus status)
{
  switch (status) {
    case EMBER_NETWORK_UP:
      app_log_info("Network UP. NodeID=0x%04X\n", emberGetNodeId());
      emberPermitJoining(0xFF);
      // 폴링: 3초 후 첫 사이클 시작 (slave들이 rejoin/announce 할 시간)
      poll_running = false;
      sl_sleeptimer_stop_timer(&poll_cycle_timer);
      sl_sleeptimer_start_timer_ms(&poll_cycle_timer, 3000,
                                   poll_cycle_timer_cb, NULL, 0, 0);
      break;
    case EMBER_NETWORK_DOWN:
      app_log_info("Network DOWN.\n");
      poll_running = false;
      sl_sleeptimer_stop_timer(&poll_cycle_timer);
      break;
    default:
      app_log_info("Stack status: 0x%02X\n", status);
      break;
  }
}

/**************************************************************************//**
 * Child join callback
 *****************************************************************************/
void emberAfChildJoinCallback(EmberNodeType nodeType, EmberNodeId nodeId)
{
  app_log_info("Node joined: nodeID=0x%04X, type=0x%02X (wait for ID_ANNOUNCE)\n",
               nodeId, nodeType);
}

// ─── [버그픽스] 워치독 하트비트 ──────────────────────────────────────────────
//   증상: 등록된 RX가 오프라인일 때 TX가 그 RX를 폴링 → poll_waiting(응답 대기)
//   상태로 진입. 이 구간엔 CPU를 깨울 타이머가 없어(poll_cycle_timer는 정지됨)
//   EM2에서 계속 잠 → tick 멈춤 → ~128s 후 워치독이 TX를 리셋(스푸리어스).
//   해결: 항상 도는 주기 타이머로 CPU를 깨워 tick(=fw_guard_feed_watchdog) 보장.
//   ※ 이 콜백은 feed를 하지 않고 wake만 한다. 진짜 hang(루프에 갇힘)이면 tick에
//      도달 못 해 feed 안 됨 → 워치독이 정상적으로 잡는다(방어 유지).
static sl_sleeptimer_timer_handle_t s_heartbeat_timer;
static void heartbeat_timer_cb(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;   // 빈 콜백 — EM2에서 CPU를 깨우는 것만이 목적
}

/**************************************************************************//**
 * Tick callback
 *****************************************************************************/
void emberAfTickCallback(void)
{
  fw_guard_feed_watchdog();   // 워치독 급이기 (tick 살아있음 보고, ~128s hang 시 리셋)

  // ─── [C-2] 첫 tick에서 NVM3 slave_table 로드 + 하트비트 타이머 시작 ───────
  if (!slave_table_loaded) {
    slave_table_loaded = true;
    load_slave_table_nvm3();
    rssi_log_init();   // [RSSI] 외부 플래시 로그 지오메트리 검증 + 커서 복원
    // 2초 주기로 CPU를 깨워 어떤 폴링 상태에서도 tick이 멈추지 않게 한다.
    sl_sleeptimer_start_periodic_timer_ms(&s_heartbeat_timer, 2000,
                                          heartbeat_timer_cb, NULL, 0, 0);
  }

  // ─── [롤백 가드] 헬스 확인 / 부팅 타임아웃 self-reset ─────────────────────
  //   TX 자체 OTA 직후(probation) 새 펌웨어가 스택 up을 15초 유지하면 healthy
  //   확정 → golden 캡처(이 시점 SLOT0=방금 설치한 TX 이미지). 코디네이터라
  //   스택 up은 RX 존재와 무관하므로 스푸리어스 롤백 없음.
  if (!boot_tick_set) {
    boot_tick     = sl_sleeptimer_get_tick_count();
    boot_tick_set = true;
  }
  if (!health_confirmed) {
    uint32_t now = sl_sleeptimer_get_tick_count();
    if (emberStackIsUp()) {
      if (stack_up_tick == 0) stack_up_tick = now;
      if (sl_sleeptimer_tick_to_ms(now - stack_up_tick) >= HEALTH_STABILITY_MS) {
        fw_guard_confirm_healthy();   // pending일 때만 golden 캡처
        health_confirmed = true;
      }
    } else {
      stack_up_tick = 0;   // 안정성 측정 리셋
      if (fw_guard_is_on_probation()
          && sl_sleeptimer_tick_to_ms(now - boot_tick) >= BOOT_HEALTH_TIMEOUT_MS) {
        app_log_error("[GUARD] Boot health timeout — self-reset (probation count).\n");
        log_flush();
        NVIC_SystemReset();
      }
    }
  }

  if (emberStackIsUp()) {
    sl_led_turn_on(&sl_led_led0);
  } else {
    sl_led_turn_off(&sl_led_led0);
  }

  // ─── permit-join 주기적 재확인 (resume 타이밍 갭 보험) ────────────────────
  //   permit-join은 NVM3에 저장되지 않아 resume 시 닫힘(0)으로 시작한다.
  //   NETWORK_UP 콜백에서 열지만, 만일의 누락/경합에 대비해 주기적으로 재확인.
  //   idempotent(이미 열려 있으면 무해)하므로 안전. RX가 항상 join 가능하게 보장.
  {
    static uint32_t s_last_permit_tick = 0;
    static bool     s_permit_init      = false;
    uint32_t now = sl_sleeptimer_get_tick_count();
    if (emberStackIsUp()
        && (!s_permit_init
            || sl_sleeptimer_tick_to_ms(now - s_last_permit_tick) > 30000U)) {
      emberPermitJoining(0xFF);
      s_last_permit_tick = now;
      s_permit_init      = true;
    }
  }

  // ─── [안전장치] 스트림 감시 타이머가 발동했다면 캠페인을 안전 종료 ────────
  //   tick 이 굶어 스트림이 슬롯을 넘겨 켜져 있던 상황. 송신은 콜백이 이미
  //   멈췄으므로, 여기서는 채널 원복 + 수신 재개까지 마무리한다.
  if (meas_stream_guard_fired) {
    meas_stream_guard_fired = false;
    meas_stream_guard_armed = false;
    meas_tx_streaming       = false;   // 콜백이 이미 껐다
    app_log_error("[MEAS] stream guard fired (tick starved) — aborting campaign.\n");
    meas_tx_abort();                   // home 채널 복귀 + 수신 재개
  }

  // ─── [비행 안전] IQ 캡처 후 수신 복구 실패 상태면 계속 되살린다 ──────────
  //   방치하면 스택은 수신 중이라 믿는데 라디오는 꺼져 있어 노드가 조용히
  //   네트워크에서 이탈한다(자체 복구 경로가 없다).
  if (iq_radio_is_deaf()) {
    if (iq_radio_recover()) {
      app_log_info("[IQ] radio RX recovered.\n");
    }
  }

  // ─── [M-3] volatile 버퍼 → 로컬 복사 후 OBC 명령 처리 ──────────────────
  if (obc_cmd_ready) {
    obc_cmd_ready = false;
    process_obc_command();
  }

  // ─── 폴링 / RSSI 측정 ─────────────────────────────────────────────────────
  //   캠페인 진행 중엔 폴링을 멈추고 측정 상태머신을 구동(라디오/채널 경합 방지).
  //   OTA 가 캠페인 도중 시작되면 즉시 중단+홈 복귀(측정 채널에 갇히지 않게).
  bool ota_busy = (ota_state != OTA_IDLE
                   && ota_state != OTA_FW_READY_MANUAL
                   && ota_state != OTA_FW_READY);
  if (meas_campaign_active()) {
    if (ota_busy) {
      meas_tx_abort();
    } else {
      meas_tx_tick();
      iq_win_tick();                  // [IQ] 마감시한 감시(진행 중에도 필요)
    }
  } else if (!ota_busy) {
    iq_win_tick();                    // [IQ] 스윕 완료 감지 → 창 닫기
    meas_auto_tick();                 // 자동 모드(기본 OFF): 주기적 재시작
    if (!meas_campaign_active() && poll_running) {
      poll_tick();
    }
  }

  ota_state_machine_tick();
}

/**************************************************************************//**
 * [OTA Server] 이미지 세그먼트 읽기
 *****************************************************************************/
bool emberAfPluginOtaUnicastBootloaderServerGetImageSegmentCallback(
  uint32_t startIndex, uint32_t endIndex,
  uint8_t  imageTag, uint8_t *imageSegment)
{
  (void)imageTag;
  uint32_t len = endIndex - startIndex + 1;
  int32_t  ret = bootloader_readStorage(0, startIndex, imageSegment, len);

  if (ret != BOOTLOADER_OK) {
    app_log_error("OTA: readStorage fail at %lu, err=0x%lX\n", startIndex, ret);
    return false;
  }

  // ─── [L-1 + M-2] 진행률 100%까지 정확히 출력, 세션 간 초기화 ─────────────
  if (gbl_image_size > 0) {
    uint32_t pct = ((endIndex + 1) * 100) / gbl_image_size;
    if (pct / 10 > ota_tx_progress_pct / 10) {
      app_log_info("OTA TX: %lu%% (%lu/%lu bytes)\n",
                   pct, endIndex + 1, gbl_image_size);
      ota_tx_progress_pct = pct;
    }
  }
  return true;
}

/**************************************************************************//**
 * [OTA Server] 이미지 배포 완료
 *****************************************************************************/
void emberAfPluginOtaUnicastBootloaderServerImageDistributionCompleteCallback(
  EmberAfOtaUnicastBootloaderStatus status)
{
  if (status == EMBER_OTA_UNICAST_BOOTLOADER_STATUS_SUCCESS) {
    app_log_info("OTA: Distribution SUCCESS. Sending bootload req in 500ms...\n");
    bootload_req_pending = false;
    sl_sleeptimer_start_timer_ms(&bootload_req_timer, 500,
                                 bootload_req_timer_cb, NULL, 0, 0);
    ota_state = OTA_REQUEST_BOOTLOAD;
  } else {
    app_log_error("OTA: Distribution FAILED, status=0x%02X\n", status);
    ota_state = OTA_ERROR;
  }
}

/**************************************************************************//**
 * [OTA Server] 부트로드 요청 완료
 * [M-1] 매직넘버 0x08 제거 — 공식 enum SUCCESS 만 처리
 *****************************************************************************/
void emberAfPluginOtaUnicastBootloaderServerRequestTargetBootloadCompleteCallback(
  EmberAfOtaUnicastBootloaderStatus status)
{
  if (status == EMBER_OTA_UNICAST_BOOTLOADER_STATUS_SUCCESS) {
    app_log_info("=== OTA DONE for node 0x%04X (device_id=%d) ===\n",
                 ota_target_node, ota_target_device_id);
    app_log_info("RX reboots & rejoins automatically. TX stays up.\n");
  } else {
    app_log_error("OTA: Bootload request FAILED, status=0x%02X\n", status);
  }
  // TX 자체 리셋 안 함(코디네이터 유지). 이미지가 슬롯에 남아있으므로
  // 같은 펌웨어를 다른 RX에 START(0x04)로 즉시 재배포 가능 → FW_READY 유지.
  ota_target_device_id = DEVICE_ID_NONE;
  ota_target_node      = EMBER_NULL_NODE_ID;
  ota_state            = OTA_FW_READY;
}

void emberAfFrequencyHoppingStartClientCompleteCallback(EmberStatus status)
{
  if (status != EMBER_SUCCESS) {
    app_log_error("FH Client sync failed: 0x%02X\n", status);
  } else {
    app_log_info("FH Client Sync Success\n");
  }
}

void emberAfEnergyScanCompleteCallback(int8_t mean, int8_t min,
                                       int8_t max, uint16_t variance)
{
  app_log_info("Energy scan: mean=%d min=%d max=%d var=%d\n",
               mean, min, max, variance);
}

void send_sweep_start_msg(EmberNodeId target)
{
  uint8_t msg[2] = { 0xE0, 0x01 };
  EmberStatus status = emberMessageSend(target, CUSTOM_ENDPOINT, 0,
                                        sizeof(msg), msg, tx_options);
  app_log_info("Sweep to 0x%04X: 0x%02X\n", target, status);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Slave 테이블 Public API (app_cli.c 에서 호출)
// ─────────────────────────────────────────────────────────────────────────────

bool set_ota_target_by_device_id(uint8_t device_id)
{
  EmberNodeId node = get_node_id_by_device_id(device_id);
  if (node == EMBER_NULL_NODE_ID) {
    app_log_error("ota_target: device_id=%d not registered or node_id unknown.\n",
                  device_id);
    app_log_info("  Check 'slave_list'. Slave may need to rejoin.\n");
    return false;
  }
  ota_target_device_id = device_id;
  app_log_info("OTA target: device_id=%d → node=0x%04X\n", device_id, node);
  app_log_info("GBL: %s | Press BTN0 to start.\n",
               (ota_state == OTA_FW_READY_MANUAL || ota_state == OTA_FW_READY)
               ? "Ready" : "Not loaded");
  return true;
}

// ─── [TEST-ONLY ota_start] CLI로 OTA 시작 (I2C 없이 지상 테스트용) ───────────
//   Simplicity Commander로 SLOT0(0x84000)에 GBL을 적재한 뒤 이 함수로 크기를
//   자동 스캔(END 태그)하여 타겟 RX로 배포를 시작한다. (예전 BTN0 지상모드 대체)
//   ※ 실위성 운용에서는 AP_OBC I2C(START 0x04)만 사용 → 이 함수/CLI 전부 제거.
bool ota_start_test(uint8_t device_id)
{
  if (device_id < 1 || device_id > MAX_SLAVES) {
    app_log_error("ota_start: invalid device_id=%d (1~%d)\n", device_id, MAX_SLAVES);
    return false;
  }
  if (ota_state != OTA_IDLE && ota_state != OTA_FW_READY) {
    app_log_error("ota_start: busy (state=%d). Try later.\n", ota_state);
    return false;
  }
  EmberNodeId node = get_node_id_by_device_id(device_id);
  if (node == EMBER_NULL_NODE_ID) {
    app_log_error("ota_start: device_id=%d not registered/online yet.\n", device_id);
    return false;
  }

  BootloaderStorageSlot_t slot;
  if (bootloader_getStorageSlotInfo(0, &slot) != BOOTLOADER_OK) {
    app_log_error("ota_start: getStorageSlotInfo failed.\n");
    return false;
  }

  // SLOT0의 GBL 크기 = END 태그(0xFC0404FC)까지 스캔
  uint32_t img_size = 0, off = 0;
  uint8_t  buf[8];
  while (off + 8 <= slot.length) {
    if (bootloader_readStorage(0, off, buf, 8) != BOOTLOADER_OK) { img_size = 0; break; }
    uint32_t tag = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                 | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    uint32_t len = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8)
                 | ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    if (len > slot.length) { img_size = 0; break; }
    off += 8 + len;
    if (tag == 0xFC0404FCUL) { img_size = off; break; }
  }
  if (img_size == 0) {
    app_log_error("ota_start: no valid GBL in SLOT0 — load it via Commander first.\n");
    return false;
  }
  if (bootloader_verifyImage(0, NULL) != BOOTLOADER_OK) {
    app_log_error("ota_start: SLOT0 verifyImage FAILED (bad/partial GBL).\n");
    return false;
  }

  gbl_image_size       = img_size;
  ota_image_tag        = 0xAA;
  ota_target_device_id = device_id;
  ota_target_node      = node;
  ota_state            = OTA_FW_READY;
  app_log_info("ota_start: SLOT0 GBL=%lu B → device_id=%d (node=0x%04X). Distributing...\n",
               (unsigned long)img_size, device_id, node);
  start_ota_distribution();
  return true;
}

void print_slave_list(void)
{
  app_log_info("╔══════════════════════════════════════════════════╗\n");
  app_log_info("║          Registered Slave List                   ║\n");
  app_log_info("╠══════════════════════════════════════════════════╣\n");

  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_SLAVES; i++) {
    if (slave_table[i].registered) {
      char marker = (slave_table[i].device_id == ota_target_device_id) ? '*' : ' ';
      const char *node_str = (slave_table[i].node_id == EMBER_NULL_NODE_ID)
                             ? "0x???? (rejoining)" : "";
      if (slave_table[i].node_id != EMBER_NULL_NODE_ID) {
        app_log_info("║ %c [%d] id=%-2d node=0x%04X fw=0x%02X %s   ║\n",
                     marker, i,
                     slave_table[i].device_id,
                     slave_table[i].node_id,
                     slave_table[i].fw_version,
                     slave_table[i].online ? "[ONL]" : "[OFF]");
      } else {
        app_log_info("║ %c [%d] id=%-2d node=%-18s           ║\n",
                     marker, i,
                     slave_table[i].device_id,
                     node_str);
      }
      count++;
    }
  }
  if (count == 0) {
    app_log_info("║  (No slaves registered yet)                      ║\n");
  }

  app_log_info("╠══════════════════════════════════════════════════╣\n");
  app_log_info("║ OTA target : device_id=%-2d                        ║\n",
               (ota_target_device_id == DEVICE_ID_NONE) ? 0 : ota_target_device_id);
  app_log_info("║ GBL state  : %-36s ║\n",
               (ota_state == OTA_FW_READY_MANUAL) ? "Ready (Ground mode)"  :
               (ota_state == OTA_FW_READY)         ? "Ready (Space mode)"   :
               (ota_state == OTA_IDLE)              ? "Not loaded"           :
               "In progress");
  app_log_info("╚══════════════════════════════════════════════════╝\n");
}

void polling_restart(void)
{
  sl_sleeptimer_stop_timer(&poll_cycle_timer);
  poll_idx         = 0;
  poll_cycle_count = 0;
  poll_waiting     = false;
  poll_running     = false;
  app_log_info("[POLL] Restarted.\n");

  if (count_pollable_slaves() == 0) {
    app_log_info("[POLL] No pollable slaves (node_id unknown). Retry in %ums.\n",
                 POLL_CYCLE_INTERVAL_MS);
    sl_sleeptimer_start_timer_ms(&poll_cycle_timer, POLL_CYCLE_INTERVAL_MS,
                                 poll_cycle_timer_cb, NULL, 0, 0);
    return;
  }
  poll_running = true;
  poll_next_slave();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static: NVM3 slave_table 영속화
// ─────────────────────────────────────────────────────────────────────────────

/**************************************************************************//**
 * [C-2] NVM3에 device_id 배열 저장.
 * register_slave() 호출 시마다 갱신.
 *****************************************************************************/
static void save_slave_table_nvm3(void)
{
  uint8_t ids[MAX_SLAVES] = {0};
  for (uint8_t i = 0; i < MAX_SLAVES; i++) {
    ids[i] = slave_table[i].registered ? slave_table[i].device_id : 0;
  }
  Ecode_t ec = nvm3_writeData(nvm3_defaultHandle,
                              NVM3_KEY_SLAVE_TABLE, ids, sizeof(ids));
  if (ec != ECODE_NVM3_OK) {
    app_log_error("NVM3 slave_table save FAILED: 0x%lX\n", ec);
  }
}

/**************************************************************************//**
 * [C-2] NVM3에서 device_id 배열 로드. (merge 방식 — 라이브 등록값 보존)
 *
 * 첫 tick 시 호출. node_id 는 ID_ANNOUNCE 수신 후 갱신되므로 NULL로 둠.
 *
 * [경쟁 조건 방지] init 직후 ~ 첫 tick 사이에 ID_ANNOUNCE 가 먼저 도착해
 *   register_slave() 로 이미 유효한 node_id 가 등록됐을 수 있다.
 *   이 경우 NVM3 값으로 덮어쓰면 방금 학습한 node_id 가 NULL로 지워진다.
 *   → 이미 등록된 device_id 는 건너뛰고, 미등록 device_id 만 추가한다.
 *****************************************************************************/
static void load_slave_table_nvm3(void)
{
  uint8_t ids[MAX_SLAVES] = {0};
  Ecode_t ec = nvm3_readData(nvm3_defaultHandle,
                             NVM3_KEY_SLAVE_TABLE, ids, sizeof(ids));
  if (ec != ECODE_NVM3_OK) {
    app_log_info("NVM3 slave_table: not found (first boot or cleared).\n");
    return;
  }

  for (uint8_t k = 0; k < MAX_SLAVES; k++) {
    uint8_t dev = ids[k];
    if (dev < 1 || dev > MAX_SLAVES) continue;

    // 이미 라이브로 등록된 device_id 면 보존 (node_id 덮어쓰지 않음)
    if (get_node_id_by_device_id(dev) != EMBER_NULL_NODE_ID) continue;
    bool already = false;
    for (uint8_t i = 0; i < MAX_SLAVES; i++) {
      if (slave_table[i].registered && slave_table[i].device_id == dev) {
        already = true;
        break;
      }
    }
    if (already) continue;

    // 빈 슬롯에 추가 (node_id 는 rejoin 후 ID_ANNOUNCE 로 채워짐)
    for (uint8_t i = 0; i < MAX_SLAVES; i++) {
      if (!slave_table[i].registered) {
        slave_table[i].device_id  = dev;
        slave_table[i].node_id    = EMBER_NULL_NODE_ID;
        slave_table[i].registered = true;
        slave_table[i].online     = false;
        slave_table[i].fw_version = 0;
        app_log_info("NVM3: Restored slave device_id=%d (node_id pending rejoin).\n",
                     dev);
        break;
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static: Slave 테이블 내부
// ─────────────────────────────────────────────────────────────────────────────

static void register_slave(uint8_t device_id, EmberNodeId node_id)
{
  if (device_id < 1 || device_id > MAX_SLAVES) {
    app_log_error("register_slave: invalid device_id=%d\n", device_id);
    return;
  }

  for (uint8_t i = 0; i < MAX_SLAVES; i++) {
    if (slave_table[i].registered && slave_table[i].device_id == device_id) {
      if (slave_table[i].node_id != node_id) {
        app_log_info("Slave id=%d: node_id 0x%04X → 0x%04X\n",
                     device_id, slave_table[i].node_id, node_id);
        slave_table[i].node_id = node_id;
        save_slave_table_nvm3();  // node_id 변경 시만 저장 (불필요한 NVM3 쓰기 방지)
      } else {
        app_log_info("Slave id=%d (node=0x%04X) re-announced.\n",
                     device_id, node_id);
      }
      slave_table[i].online = true;
      return;
    }
  }

  for (uint8_t i = 0; i < MAX_SLAVES; i++) {
    if (!slave_table[i].registered) {
      slave_table[i].device_id  = device_id;
      slave_table[i].node_id    = node_id;
      slave_table[i].registered = true;
      slave_table[i].online     = true;
      app_log_info("Slave registered: id=%d, node=0x%04X (slot %d)\n",
                   device_id, node_id, i);
      save_slave_table_nvm3();
      return;
    }
  }

  app_log_error("register_slave: table full!\n");
}

static EmberNodeId get_node_id_by_device_id(uint8_t device_id)
{
  for (uint8_t i = 0; i < MAX_SLAVES; i++) {
    if (slave_table[i].registered && slave_table[i].device_id == device_id) {
      return slave_table[i].node_id;
    }
  }
  return EMBER_NULL_NODE_ID;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static: OBC I2C 명령 처리
// ─────────────────────────────────────────────────────────────────────────────

/**************************************************************************//**
 * [비행 안전] 명령 프레임 길이 검증 — 우발적 파괴 명령 차단.
 *
 *   SPI 리드백은 마스터가 33KB 를 클럭으로 뽑아가는 동작이고, 그동안 마스터가
 *   내보내는 dummy 바이트가 우리 수신 버퍼를 가득 채운다. 즉 "읽기"만 해도
 *   길이 136 짜리 가짜 명령 프레임이 하나 만들어진다. 그 선두 바이트가 우연히
 *   0x03(ERASE) 이면 OTA 스토리지 슬롯이 통째로 지워진다 — 궤도상에서 복구
 *   불가능한 사고다. 버스 노이즈/클럭 글리치도 같은 결과를 낼 수 있다.
 *
 *   실제 명령은 길이가 정해져 있으므로 길이가 맞지 않으면 실행하지 않는다.
 *   (dummy 로 채워진 136바이트 프레임은 어떤 명령의 길이와도 일치하지 않는다.)
 *****************************************************************************/
static bool obc_cmd_len_ok(uint8_t cmd, uint16_t len)
{
  switch (cmd) {
    case OBC_CMD_DUMMY:     return true;                 // 무시되는 명령
    case OBC_CMD_DATA:      return (len <= 2U);          // [cc]
    case OBC_CMD_ERASE:     return (len == 2U);          // [cc][slot]  ★ 파괴적
    case OBC_CMD_START:     return (len == 2U);          // [cc][target] ★ 파괴적
    case OBC_CMD_IQ_START:  return (len <= 2U);          // [cc](+dur)
    case OBC_CMD_IQ_READ:   return (len <= 2U);          // [cc]
    // [cc][packet_num:2][len:1][data:N] — N 은 호스트 버전에 따라 120 또는 128.
    //   따라서 최대 정상 길이 = 4 + FW_MAX_CHUNK. OTA 스트리밍을 막지 않도록
    //   반드시 이 상한을 지킬 것(너무 좁게 잡으면 OTA 가 통째로 죽는다).
    case OBC_CMD_FW_UPDATE: return (len >= 4U && len <= (4U + FW_MAX_CHUNK));
    default:                return false;
  }
}

/**************************************************************************//**
 * [M-3] obc_rx_buffer(volatile) → 로컬 배열로 복사 후 파싱
 *****************************************************************************/
static void process_obc_command(void)
{
  uint8_t  local_buf[OBC_LOCAL_BUF_SIZE];
  // ★ obc_rx_len 이 아니라 명령 확정 시점에 걸어 둔 obc_cmd_len 을 쓴다.
  //   (SPI 는 CS High 에서 obc_rx_len 을 0 으로 되돌리므로 그대로 읽으면 항상 0)
  uint16_t local_len = obc_cmd_len;
  if (local_len > sizeof(local_buf)) local_len = sizeof(local_buf);
  memcpy(local_buf, (const uint8_t *)obc_rx_buffer, local_len);

  if (local_len < 1) {
    return;   // 빈 트랜잭션 무시
  }

  uint8_t cmd = local_buf[0];

  // 길이가 규격과 다르면 실행하지 않는다(위 obc_cmd_len_ok 주석 참조).
  if (!obc_cmd_len_ok(cmd, local_len)) {
    app_log_error("OBC: rejected CMD 0x%02X (bad len=%u)\n",
                  cmd, (unsigned)local_len);
    return;
  }

  switch (cmd) {
    case OBC_CMD_DUMMY:
      // 호스트가 스트리밍 직전 보내는 10바이트 dummy write → 무시
      break;
    case OBC_CMD_DATA:      handle_cmd_data(local_buf, local_len);      break;
    case OBC_CMD_FW_UPDATE: handle_cmd_fw_packet(local_buf, local_len); break;
    case OBC_CMD_ERASE:     handle_cmd_erase(local_buf, local_len);     break;
    case OBC_CMD_START:     handle_cmd_start(local_buf, local_len);     break;
    case OBC_CMD_IQ_START:  handle_cmd_iq_start(local_buf, local_len);  break;
    case OBC_CMD_IQ_READ:   handle_cmd_iq_read(local_buf, local_len);   break;
    default:
      app_log_error("OBC: Unknown CMD 0x%02X (len=%d)\n", cmd, local_len);
      break;
  }
}

/**************************************************************************//**
 * CMD 0x05 (iq_start): IWSB 미션 트리거.
 *   인자 없이 항상 IQ_MISSION_SWEEPS 회 스윕을 수행하며, 스윕 1회가 끝날 때마다
 *   배치를 OBC 가 회수할 수 있게 준비한다. 어떤 상태에서 눌러도 미션 1회가
 *   온전히 수행되도록 잔여 데이터는 비우고, 시작 불가 상황은 예약 후 자동 재시도.
 *****************************************************************************/
static void handle_cmd_iq_start(const uint8_t *buf, uint16_t len)
{
  (void)buf; (void)len;   // 미션 모드에서는 인자를 쓰지 않는다(항상 100 스윕)
  obc_cmd_iq_start_mirror();
}

/**************************************************************************//**
 * [IWSB] 0x05 진입점 — OBC(SPI/I2C)와 CLI 가 "같은 코드"를 타도록 분리했다.
 *   eval 보드에서 OBC 없이 CLI 만으로 전 과정을 동일하게 재현하기 위함.
 *   OTA 중이어도 거부하지 않는다 — 미션을 예약해 두고 OTA 가 끝나는 즉시
 *   iq_win_tick() 이 자동으로 첫 스윕을 시작한다(OTA 와의 배타성은 그대로 유지).
 *****************************************************************************/
void obc_cmd_iq_start_mirror(void)
{
  iq_meas_trigger(0);
}

/**************************************************************************//**
 * CMD 0x06 (iq_read): 쌓인 IQ 레코드 "전부"를 I2C 리드백 버퍼에 준비.
 *   이후 OBC 가 I2C read 한 번으로 전체 프레임을 읽어간다(I2C IRQ 가 스트리밍).
 *   프레임: [n_records] + 레코드들(각 IQ_REC_SIZE 고정). 호출 후 링은 비워짐.
 *   ※ OBC 는 06 write 후 tick 반영까지 수 ms 지연 뒤 read 할 것.
 *****************************************************************************/
static void handle_cmd_iq_read(const uint8_t *buf, uint16_t len)
{
  (void)buf; (void)len;

  if (!iq_mission_batch_ready()) {
    // 준비된 배치가 없는데 0x06 이 왔다 — 중복 ACK 이거나 미션 밖. 무시.
    app_log_info("[IWSB] 0x06 ignored (no batch pending).\n");
    return;
  }
  // ★ OBC 가 "데이터 정상 수신" 을 확인해 준 것. 이제서야 다음 스윕을 허가한다.
  app_log_info("[IWSB] 0x06 ACK — batch %u accepted, next sweep.\n",
               (unsigned)iq_mission_sweeps_done());
  iq_batch_taken();
}

/**************************************************************************//**
 * CMD 0x01 (data): 상태/데이터 요청
 *   호스트 i2c_data_buf_t = [cc] (1바이트 write).
 *   현재는 상태 로그만. (응답 read 경로는 데이터 프로토콜 확정 시 구현 — TODO)
 *****************************************************************************/
static void handle_cmd_data(const uint8_t *buf, uint16_t len)
{
  (void)buf; (void)len;
  app_log_info("OBC DATA: state=%d, offset=%lu/%lu, target_dev=%d\n",
               ota_state, gbl_write_offset, gbl_image_size,
               ota_target_device_id);
}

/**************************************************************************//**
 * CMD 0x04 (start): OTA 자동 트리거 — [cc][target]
 *   target == 0x00      → TX 자체 펌웨어 설치 (verify→install→reboot)
 *   target == 1~MAX     → 해당 device_id RX 로 OTA 배포 시작(상태머신이 자동 진행)
 *
 * ★ CLI/BTN0 없이 AP_OBC 명령만으로 전 과정 자동화하는 진입점.
 *   사전조건: erase(0x03) → FW 스트리밍(0x02) 완료 → ota_state==OTA_FW_READY
 *****************************************************************************/
static void handle_cmd_start(const uint8_t *buf, uint16_t len)
{
  if (len < 2) {
    app_log_error("OBC START: too short (need [cc][target])\n");
    return;
  }
  uint8_t target = buf[1];

  if (ota_state != OTA_FW_READY) {
    app_log_error("OBC START: FW not ready (state=%d). Stream FW first.\n", ota_state);
    return;
  }

  // ─── TX 자체 설치 ────────────────────────────────────────────────────────
  if (target == OBC_TARGET_TX_SELF) {
    app_log_info("OBC START: TX SELF-UPDATE install...\n");
    if (bootloader_verifyImage(0, NULL) != BOOTLOADER_OK) {
      app_log_error("Self-update verifyImage FAILED — abort (keep current fw).\n");
      ota_state = OTA_FW_READY;   // 현재 펌웨어 유지, 재시도 가능
      return;
    }
    if (bootloader_setImageToBootload(0) != BOOTLOADER_OK) {
      app_log_error("Self-update setImageToBootload FAILED.\n");
      ota_state = OTA_FW_READY;
      return;
    }
    // [롤백 가드] 설치 직전 probation 무장 → 새 TX 펌웨어가 다음 부팅에서 검증받음.
    //   (verifyImage 통과 = verify-before-install 방어. 부팅 후 스택 up 15s 유지하면
    //    healthy 확정 + 이 SLOT0(TX 이미지)를 golden 캡처. 미확정 5회 부팅 시 롤백.)
    fw_guard_arm_pending();
    app_log_info("Self-update verified. Rebooting to install...\n");
    log_flush();
    bootloader_rebootAndInstall();   // 복귀 안 함
    return;
  }

  // ─── RX OTA 배포 ─────────────────────────────────────────────────────────
  if (target < 1 || target > MAX_SLAVES) {
    app_log_error("OBC START: invalid target=%u (0=self,1~%d=RX)\n",
                  target, MAX_SLAVES);
    return;
  }
  EmberNodeId node = get_node_id_by_device_id(target);
  if (node == EMBER_NULL_NODE_ID) {
    app_log_error("OBC START: device_id=%u not registered/online yet.\n", target);
    return;   // RX가 아직 announce 안 함 → ota_state는 FW_READY 유지, OBC가 재시도 가능
  }

  ota_target_device_id = target;
  ota_target_node      = node;
  app_log_info("OBC START: OTA → device_id=%u (node=0x%04X). Auto-distributing...\n",
               target, node);
  start_ota_distribution();   // 이후 PREPARE→배포→bootload→완료까지 상태머신 자동
}

/**************************************************************************//**
 * CMD 0x03 (erase): 스토리지 슬롯 erase
 *   호스트 i2c_erase_storage_t = [cc][storage].
 *   FW 스트리밍 전에 반드시 호출되어야 함 (별도 트랜잭션).
 *****************************************************************************/
static void handle_cmd_erase(const uint8_t *buf, uint16_t len)
{
  if (len < 2) {
    app_log_error("OBC ERASE: too short (len=%d)\n", len);
    return;
  }
  uint8_t slot = buf[1];

  // 슬롯 크기 확보 (FW 스트리밍 경계 검사에 사용)
  BootloaderStorageSlot_t si;
  if (bootloader_getStorageSlotInfo(slot, &si) == BOOTLOADER_OK) {
    if (slot == 0) storage_slot_size = si.length;
  } else {
    app_log_warning("ERASE: getStorageSlotInfo(%u) failed. Bounds check off.\n", slot);
    if (slot == 0) storage_slot_size = 0;
  }

  app_log_info("OBC ERASE: erasing slot %u...\n", slot);
  int32_t ret = bootloader_eraseStorageSlot(slot);   // 느린 동작이나 tick 컨텍스트라 안전
  if (ret != BOOTLOADER_OK) {
    app_log_error("Slot %u erase FAILED: 0x%lX\n", slot, ret);
    ota_state = OTA_ERROR;
    return;
  }
  app_log_info("Slot %u erased.\n", slot);

  if (slot == 0) {
    gbl_write_offset = 0;
    fw_next_seq      = 0;
    ota_state        = OTA_IDLE;   // 스트리밍 수신 대기
  }
}

/**************************************************************************//**
 * CMD 0x02 (FW update): 이미지 스트리밍 패킷 수신
 *
 * 호스트 i2c_FW_buf_t = [cc][packet_num:2 LE][len:1][buf_data:120].
 *   - packet_num == 0      : 첫 패킷 → 오프셋 0부터 수신 시작
 *   - packet_num == 0xFFFF  : 마지막 패킷 → 수신 완료, FW_READY
 *   - 그 외                 : 순차 패킷 (순서 검증)
 *
 * 오프셋은 누적(gbl_write_offset)으로 추적한다.
 *   마지막 패킷의 packet_num이 0xFFFF로 덮어써져 시퀀스 번호를 알 수 없기 때문.
 * 비-마지막 패킷은 packet_num*120 == 누적오프셋 검증으로 드롭 패킷을 잡아낸다.
 *
 * ※ 슬롯 erase는 CMD 0x03(erase)로 사전 수행되어야 함. (호스트 워크플로우)
 *****************************************************************************/
static void handle_cmd_fw_packet(const uint8_t *buf, uint16_t len)
{
  if (len < FW_PKT_HEADER_SIZE) {
    app_log_error("FW pkt too short (len=%d)\n", len);
    return;
  }

  uint16_t packet_num = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
  uint8_t  data_len   = buf[3];
  const uint8_t *data = &buf[FW_PKT_HEADER_SIZE];

  if ((uint16_t)(FW_PKT_HEADER_SIZE + data_len) > len) {
    app_log_error("FW pkt truncated: data_len=%u but only %d rx\n",
                  data_len, len - FW_PKT_HEADER_SIZE);
    return;
  }
  if (data_len == 0 || data_len > FW_MAX_CHUNK) {
    app_log_error("FW pkt invalid data_len=%u\n", data_len);
    return;
  }

  bool is_last = (packet_num == FW_LAST_PACKET_NUM);

  // ─── 수신 세션 시작 ──────────────────────────────────────────────────────
  //   아직 RECEIVING 이 아니면 새 세션으로 간주.
  //   (정상: packet_num==0 / 단일 패킷 이미지: packet_num==0xFFFF 도 여기서 시작)
  //   ※ erase(0x03)로 ota_state=OTA_IDLE 이 된 직후가 정상 진입 지점.
  if (ota_state != OTA_RECEIVING_FW) {
    if (storage_slot_size == 0) {
      BootloaderStorageSlot_t si;
      if (bootloader_getStorageSlotInfo(0, &si) == BOOTLOADER_OK) {
        storage_slot_size = si.length;
      }
    }
    gbl_write_offset = 0;
    fw_next_seq      = 0;
    ota_image_tag    = 0xAA;
    ota_state        = OTA_RECEIVING_FW;
    app_log_info("FW stream START (slot must be pre-erased).\n");
  }

  // ─── 순서 진단 (비치명적) ───────────────────────────────────────────────
  //   청크 크기를 가정하지 않으므로 오프셋 검증은 하지 않는다(검증된 코드와 동일).
  //   드롭 패킷은 최종 verifyImage 에서 CRC 불일치로 걸러져 설치되지 않는다.
  //   순번 점프만 로그로 남겨 진단에 활용.
  if (!is_last && packet_num != fw_next_seq) {
    app_log_warning("FW pkt# jump: got %u, expected %u (drop?)\n",
                    packet_num, fw_next_seq);
  }

  // ─── 슬롯 경계 검사 ─────────────────────────────────────────────────────
  if (storage_slot_size > 0
      && gbl_write_offset + (uint32_t)data_len > storage_slot_size) {
    app_log_error("FW overflow %lu+%u > slot %lu. ABORT.\n",
                  gbl_write_offset, data_len, storage_slot_size);
    ota_state = OTA_ERROR;
    return;
  }

  // ─── 플래시 기록 ────────────────────────────────────────────────────────
  int32_t ret = bootloader_writeStorage(0, gbl_write_offset,
                                         (uint8_t *)data, data_len);
  if (ret != BOOTLOADER_OK) {
    app_log_error("FW writeStorage FAIL at %lu: 0x%lX\n", gbl_write_offset, ret);
    ota_state = OTA_ERROR;
    return;
  }

  // ─── 청크별 readback 검증 (검증된 코드 방식) ────────────────────────────
  //   write가 OK여도 실제 플래시 반영을 즉시 확인 → 손상 조기 검출.
  {
    static uint8_t verify_buf[FW_MAX_CHUNK];
    if (bootloader_readStorage(0, gbl_write_offset, verify_buf, data_len)
          == BOOTLOADER_OK
        && memcmp(verify_buf, data, data_len) != 0) {
      app_log_error("FW readback MISMATCH at %lu. ABORT.\n", gbl_write_offset);
      ota_state = OTA_ERROR;
      return;
    }
  }

  gbl_write_offset += data_len;
  fw_next_seq++;

  // ─── 마지막 패킷 → 완료 ─────────────────────────────────────────────────
  if (is_last) {
    gbl_image_size = gbl_write_offset;
    ota_state      = OTA_FW_READY;
    app_log_info("FW stream COMPLETE. size=%lu bytes. (OTA_FW_READY)\n",
                 gbl_image_size);
    app_log_info("  → AP_OBC: send START(0x04) [target] (0=TX self, 1~%d=RX device_id)\n",
                 MAX_SLAVES);
  } else if ((packet_num % 50) == 0) {
    app_log_info("FW rx: pkt#%u, %lu bytes\n", packet_num, gbl_write_offset);
  }
}

/**************************************************************************//**
 * Slave에 OTA 준비 메시지 전송
 *****************************************************************************/
static void send_slave_prepare_msg(EmberNodeId target)
{
  uint8_t msg[6];
  msg[0] = MSG_TYPE_OTA_PREPARE;
  msg[1] = ota_image_tag;
  msg[2] = (uint8_t)(gbl_image_size & 0xFF);
  msg[3] = (uint8_t)((gbl_image_size >> 8) & 0xFF);
  msg[4] = (uint8_t)((gbl_image_size >> 16) & 0xFF);
  msg[5] = (uint8_t)((gbl_image_size >> 24) & 0xFF);

  EmberStatus st = emberMessageSend(target, CUSTOM_ENDPOINT, 0,
                                    sizeof(msg), msg, tx_options);
  if (st != EMBER_SUCCESS) {
    app_log_error("OTA PREPARE msg to 0x%04X FAILED: 0x%02X\n", target, st);
  }
}

/**************************************************************************//**
 * OTA 자동화 상태머신 (tick에서 호출)
 *****************************************************************************/
static void ota_state_machine_tick(void)
{
  switch (ota_state) {

    case OTA_WAITING_SLAVE_PREPARE: {
      if (slave_prepare_acked) {
        app_log_info("Slave ready. Initiating OTA distribution...\n");
        EmberAfOtaUnicastBootloaderStatus ret =
          emberAfPluginOtaUnicastBootloaderServerInitiateImageDistribution(
            ota_target_node, gbl_image_size, ota_image_tag);
        if (ret == EMBER_OTA_UNICAST_BOOTLOADER_STATUS_SUCCESS) {
          app_log_info("OTA distribution initiated.\n");
          ota_timer_start = sl_sleeptimer_get_tick_count();  // 배포 타임아웃 기준
          ota_state = OTA_DISTRIBUTING;
        } else {
          app_log_error("OTA initiate FAILED: 0x%02X\n", ret);
          ota_state = OTA_ERROR;
        }
      } else {
        uint32_t elapsed = sl_sleeptimer_tick_to_ms(
                             sl_sleeptimer_get_tick_count() - ota_timer_start);
        if (elapsed > OTA_SLAVE_PREPARE_TIMEOUT_MS) {
          if (++prepare_retries > MAX_PREPARE_RETRIES) {
            app_log_error("Slave prepare: RX unreachable (%u retries). Abort to FW_READY.\n",
                          prepare_retries);
            // 이미지는 슬롯에 유효 → OBC가 나중에 START(0x04) 재시도/다른 RX 타겟 가능
            ota_target_device_id = DEVICE_ID_NONE;
            ota_target_node      = EMBER_NULL_NODE_ID;
            ota_state            = OTA_FW_READY;
          } else {
            app_log_error("Slave prepare TIMEOUT (%u/%u). Retrying...\n",
                          prepare_retries, MAX_PREPARE_RETRIES);
            send_slave_prepare_msg(ota_target_node);
            ota_timer_start = sl_sleeptimer_get_tick_count();
          }
        }
      }
      break;
    }

    case OTA_DISTRIBUTING: {
      // 플러그인이 완료 콜백을 끝내 호출하지 않는 경우(타겟 소실 등) 무한정체 방지.
      uint32_t elapsed = sl_sleeptimer_tick_to_ms(
                           sl_sleeptimer_get_tick_count() - ota_timer_start);
      if (elapsed > OTA_DISTRIBUTE_TIMEOUT_MS) {
        app_log_error("OTA distribute timeout (%lums). Aborting.\n", elapsed);
        emberAfPluginOtaUnicastBootloaderServerAbortCurrentProcess();
        ota_state = OTA_ERROR;
      }
      break;
    }

    case OTA_REQUEST_BOOTLOAD: {
      if (!bootload_req_pending) break;
      bootload_req_pending = false;

      app_log_info("OTA: Requesting bootload (delay=%dms)...\n",
                   OTA_BOOTLOAD_DELAY_MS);
      EmberAfOtaUnicastBootloaderStatus ret =
        emberAfPluginUnicastBootloaderServerInitiateRequestTargetBootload(
          OTA_BOOTLOAD_DELAY_MS, ota_image_tag, ota_target_node);

      if (ret == EMBER_OTA_UNICAST_BOOTLOADER_STATUS_SUCCESS) {
        ota_timer_start = sl_sleeptimer_get_tick_count();  // 부트로드 대기 타임아웃 기준
        ota_state = OTA_WAITING_BOOTLOAD;
      } else {
        app_log_error("Bootload request FAILED: 0x%02X\n", ret);
        ota_state = OTA_ERROR;
      }
      break;
    }

    case OTA_WAITING_BOOTLOAD: {
      // bootload 완료 콜백이 끝내 안 오는 경우 FW_READY로 복귀(이미지는 슬롯에 유지).
      uint32_t elapsed = sl_sleeptimer_tick_to_ms(
                           sl_sleeptimer_get_tick_count() - ota_timer_start);
      if (elapsed > OTA_BOOTLOAD_WAIT_TIMEOUT_MS) {
        app_log_error("OTA bootload-wait timeout (%lums). Back to FW_READY.\n", elapsed);
        ota_target_device_id = DEVICE_ID_NONE;
        ota_target_node      = EMBER_NULL_NODE_ID;
        ota_state            = OTA_FW_READY;
      }
      break;
    }

    case OTA_ERROR:
      app_log_error("=== OTA ERROR → IDLE ===\n");
      ota_target_device_id = DEVICE_ID_NONE;
      fw_next_seq          = 0;
      ota_state = OTA_IDLE;
      break;

    case OTA_IDLE:
    case OTA_RECEIVING_FW:
    case OTA_FW_READY:
    case OTA_FW_READY_MANUAL:
    default:
      break;
  }
}

/**************************************************************************//**
 * OTA 배포 시작 (지상/우주 모드 공통)
 *****************************************************************************/
static void start_ota_distribution(void)
{
  app_log_info("OTA → node=0x%04X, id=%d, tag=0x%02X, size=%lu\n",
               ota_target_node, ota_target_device_id,
               ota_image_tag, gbl_image_size);
  // [M-2] 진행률 초기화
  ota_tx_progress_pct = 0;
  slave_prepare_acked = false;
  prepare_retries     = 0;
  send_slave_prepare_msg(ota_target_node);
  ota_timer_start = sl_sleeptimer_get_tick_count();
  ota_state = OTA_WAITING_SLAVE_PREPARE;
}

static void bootload_req_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  bootload_req_pending = true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  [C-3] TX 코디네이터 폴링
//  사이클 완료 조건: poll_cycle_count >= count_pollable_slaves()
//  slave 수에 무관하게 POLL_CYCLE_INTERVAL 간격 보장.
//  node_id 가 EMBER_NULL_NODE_ID 인 slave는 폴링 제외 (rejoin 대기 중).
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t count_pollable_slaves(void)
{
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_SLAVES; i++) {
    if (slave_table[i].registered
        && slave_table[i].node_id != EMBER_NULL_NODE_ID) {
      n++;
    }
  }
  return n;
}

static void poll_next_slave(void)
{
  for (uint8_t i = 0; i < MAX_SLAVES; i++) {
    uint8_t idx = (poll_idx + i) % MAX_SLAVES;
    if (!slave_table[idx].registered) continue;
    if (slave_table[idx].node_id == EMBER_NULL_NODE_ID) continue;  // rejoin 대기

    poll_idx = (idx + 1) % MAX_SLAVES;
    slave_table[idx].online    = false;
    poll_current_dev_id        = slave_table[idx].device_id;

    uint8_t msg[2] = { MSG_TYPE_POLL_REQUEST, slave_table[idx].device_id };
    EmberStatus st = emberMessageSend(slave_table[idx].node_id,
                                      CUSTOM_ENDPOINT, 0,
                                      sizeof(msg), msg, tx_options);
    if (st == EMBER_SUCCESS) {
      app_log_info("[POLL] → id=%d (node=0x%04X)\n",
                   slave_table[idx].device_id, slave_table[idx].node_id);
      poll_waiting = true;
      poll_anchor  = sl_sleeptimer_get_tick_count();
    } else {
      app_log_error("[POLL] Send fail to id=%d: 0x%02X\n",
                    slave_table[idx].device_id, st);
      poll_waiting = false;  // 즉시 다음 slave로
    }
    return;
  }
  // 폴링 가능한 slave 없음 (전부 null node_id 또는 미등록)
  poll_waiting = false;
}

/**************************************************************************//**
 * [C-3] poll_tick — emberAfTickCallback 에서 매 tick 호출
 *
 * 사이클 완료 = poll_cycle_count >= count_pollable_slaves().
 * slave가 몇 개이든(1~4) 항상 POLL_CYCLE_INTERVAL 간격 유지.
 *****************************************************************************/
static void poll_tick(void)
{
  if (poll_waiting) {
    uint32_t elapsed = sl_sleeptimer_tick_to_ms(
                         sl_sleeptimer_get_tick_count() - poll_anchor);
    if (elapsed < POLL_RESPONSE_TIMEOUT_MS) return;   // 아직 대기 중
    app_log_info("[POLL] Timeout for id=%d.\n", poll_current_dev_id);
    poll_waiting = false;
  }

  // 현재 slave 처리 완료 (응답 or 타임아웃)
  poll_cycle_count++;

  uint8_t pollable = count_pollable_slaves();
  if (pollable == 0 || poll_cycle_count >= pollable) {
    app_log_info("[POLL] Cycle done (%u/%u slaves). Next in %ums.\n",
                 poll_cycle_count, pollable, POLL_CYCLE_INTERVAL_MS);
    poll_running = false;
    sl_sleeptimer_start_timer_ms(&poll_cycle_timer, POLL_CYCLE_INTERVAL_MS,
                                 poll_cycle_timer_cb, NULL, 0, 0);
    return;
  }

  poll_next_slave();
}

/**************************************************************************//**
 * poll_cycle_timer_cb — 사이클 간격 타이머 완료 → 다음 사이클 시작
 *****************************************************************************/
static void poll_cycle_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  poll_idx         = 0;
  poll_cycle_count = 0;
  poll_waiting     = false;

  if (count_pollable_slaves() == 0) {
    app_log_info("[POLL] No pollable slaves yet. Retry in %ums.\n",
                 POLL_CYCLE_INTERVAL_MS);
    sl_sleeptimer_start_timer_ms(&poll_cycle_timer, POLL_CYCLE_INTERVAL_MS,
                                 poll_cycle_timer_cb, NULL, 0, 0);
    return;
  }

  poll_running = true;
  poll_next_slave();
}

// -----------------------------------------------------------------------------
//  RTOS Task (기존 코드)
// -----------------------------------------------------------------------------
#if defined(EMBER_AF_PLUGIN_MICRIUM_RTOS) && defined(EMBER_AF_PLUGIN_MICRIUM_RTOS_APP_TASK1)
void emberAfPluginMicriumRtosAppTask1InitCallback(void)
{
  app_log_info("app task init\n");
}
#include <kernel/include/os.h>
#define TICK_INTERVAL_MS 1000
void emberAfPluginMicriumRtosAppTask1MainLoopCallback(void *p_arg)
{
  RTOS_ERR err;
  OS_TICK yield_time_ticks = (OSCfg_TickRate_Hz * TICK_INTERVAL_MS) / 1000;
  while (true) {
    app_log_info("app task tick\n");
    OSTimeDly(yield_time_ticks, OS_OPT_TIME_DLY, &err);
  }
}
#endif
