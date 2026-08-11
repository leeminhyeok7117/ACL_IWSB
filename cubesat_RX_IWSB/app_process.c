/***************************************************************************//**
 * @file
 * @brief app_process.c — Slave (Sensor / OTA Client) — Coordinator Polling
 *
 * [자동 복구 설계]
 *   1. 재Join 지수 백오프 (5s → ... → 60s 상한)
 *   2. resume 멈춤 15s 워치독 → 자동 rejoin
 *   3. 연속 ACK 실패 10회 → soft rejoin
 *   4. NETWORK_DOWN → 즉시 rejoin 스케줄
 *   5. ID_ANNOUNCE: NETWORK_UP 1초 후 최초 전송, 이후 30초마다 재전송
 *      (TX 리셋 후 slave 등록 자동 복구)
 *   6. OTA PREPARE 슬롯 erase: 메시지 콜백에서 defer → tick에서 처리
 *      (수백ms 블로킹을 콜백 밖으로 이동)
 *******************************************************************************
 * SPDX-License-Identifier: Zlib
 ******************************************************************************/

#include PLATFORM_HEADER
#include "stack/include/ember.h"
#include "em_chip.h"
#include "app_log.h"
#include "sl_app_common.h"
#include "app_framework_common.h"
#include "app_process.h"
#include <stdlib.h>
#include <string.h>
#include "sl_sleeptimer.h"
#include "btl_interface.h"
#include "btl_interface_storage.h"
#include "ota-unicast-bootloader-client.h"
#include "fw_guard.h"
#include "em_usart.h"
#include "sl_power_manager.h"
#include "iq_capture.h"   // [IQ] RAIL IQ 캡처 코어 (측정 슬롯에서만 사용)

// [디버깅] VCOM(USART0) TX가 다 빠질 때까지 대기 → 로그 즉시 표시(sleep로 갇힘 방지)
//   ★ 비행 안전: 무한 대기 금지(TX 프로젝트와 동일 조치).
//     VCOM 비활성/USART0 클럭 off 빌드에서 TXC 가 영원히 안 서면 여기서 멈춘다.
static inline void log_flush(void)
{
  for (uint32_t guard = 0; guard < 100000U; guard++) {
    if (USART0->STATUS & USART_STATUS_TXC) return;
  }
}

// ─── [sleep 제어] 미가입 동안 EM2 sleep 차단 ────────────────────────────────
//   EM1 requirement만으로는 Connect 스택이 EM2로 빠지는 경우가 있어 tick이
//   멈춘다. 추가로 1초 주기 sleeptimer를 돌려 RTCC 인터럽트로 CPU를 깨운다.
//   RTCC는 EM2에서도 동작하므로, 가입 전까지 최악의 경우에도 1초마다 tick이
//   돌아 rejoin 재시도 타이머가 진행된다.
static bool s_no_sleep_req = false;
static sl_sleeptimer_timer_handle_t s_wakeup_timer;

static void wakeup_timer_cb(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;   // 빈 콜백 — RTCC 인터럽트로 CPU를 EM2에서 깨우는 것이 목적
}

static inline void block_sleep_while_unjoined(bool block)
{
  if (block && !s_no_sleep_req) {
    sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
    s_no_sleep_req = true;
    // EM1 req 가 무시되는 경우를 대비해 1초 주기 RTCC 타이머로 CPU 강제 웨이크업.
    sl_sleeptimer_start_periodic_timer_ms(&s_wakeup_timer, 1000,
                                          wakeup_timer_cb, NULL, 0, 0);
  } else if (!block && s_no_sleep_req) {
    sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1);
    s_no_sleep_req = false;
    sl_sleeptimer_stop_timer(&s_wakeup_timer);   // 가입 완료 → 저전력 복귀
  }
}

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
#define CUSTOM_ENDPOINT             0x02

#define JOIN_RETRY_MIN_MS           2000U   // 첫 재시도 간격(빠른 재연결 우선)
#define JOIN_RETRY_MAX_MS           20000U  // 백오프 상한(위성: 전력보다 연결성 우선)
#define FIRST_JOIN_DELAY_MS         300U    // 부팅 후 첫 join까지 지연(PHY 보정 완료 대기)
#define RESUME_WATCHDOG_MS          15000U
#define MAX_CONSEC_SEND_FAILS       3U    // 3회 연속 실패 → soft rejoin
#define JOIN_GIVEUP_RESET_MS        300000U // 5분 연속 미가입 → 칩 리셋(깨끗한 재시작)

// ─── [롤백 가드] 헬스 확인 타이밍 ────────────────────────────────────────────
#define HEALTH_STABILITY_MS         15000U   // NETWORK_UP 연속 유지 시 healthy 확정
#define BOOT_HEALTH_TIMEOUT_MS      180000U  // 이 시간 내 healthy 못 되면 probation시 self-reset

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
// [IQ 중계] 전방 선언 — 구현은 아래 정의부 참조.
static void iq_send_frag_of(const iq_record_t *rec, uint8_t frag_idx,
                            uint8_t msg_type, EmberNodeId dest);
static void iq_relay_tx_tick(void);
static void iq_relay_forward(const uint8_t *p, uint8_t len);
static void handle_poll_request(EmberIncomingMessage *message);
static void send_poll_response(EmberNodeId requester);
static void handle_ota_prepare_msg(EmberIncomingMessage *message);
static void send_prepare_ack(EmberNodeId master_node);
static void send_id_announce(void);
static void do_join(void);
static void schedule_rejoin(void);
static void bootload_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data);
static void verify_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data);
static void id_announce_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data);

void start_initial_join(void);   // app_init.c 에서 호출

// -----------------------------------------------------------------------------
//                                Global Variables
// -----------------------------------------------------------------------------
EmberMessageOptions tx_options = EMBER_OPTIONS_ACK_REQUESTED | EMBER_OPTIONS_SECURITY_ENABLED;

extern volatile bool network_joined;
extern volatile bool join_in_progress;
extern uint8_t my_device_id;

uint16_t sensor_report_period_ms = 1000;
EmberEventControl *report_control = NULL;

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
static bool        ota_prepare_done   = false;
static uint8_t     ota_expected_tag   = 0;
static uint32_t    ota_expected_size  = 0;
static bool        image_verified     = false;
static EmberNodeId master_node_id     = 0x0000;

static uint32_t    join_retry_anchor  = 0;
static uint32_t    rejoin_backoff_ms  = JOIN_RETRY_MIN_MS;
static uint8_t     consec_send_fails  = 0;
static uint32_t    unjoined_since_tick = 0;   // 미가입 시작 시점(0=가입됨). giveup-reset 판정용

// ─── [OTA 보호] 다운로드 중 자가 교란 억제 ───────────────────────────────────
//   OTA 수신 중엔 채널이 세그먼트로 혼잡 → ID_ANNOUNCE 의 ACK가 자주 실패한다.
//   이를 soft-rejoin(3회 실패→네트워크 leave)으로 오해하면 OTA가 끊긴다(0x09).
//   다운로드 동안: soft-rejoin 금지 + ID_ANNOUNCE 전송 억제 + golden 캡처 보류.
static volatile bool ota_download_active = false;

// ─── [롤백 가드] 헬스 확인 상태 ──────────────────────────────────────────────
static bool        health_confirmed   = false;
static bool        boot_tick_set       = false;
static uint32_t    boot_tick           = 0;   // 부팅 기준 tick
static uint32_t    net_up_tick         = 0;   // 마지막 NETWORK_UP tick (0=다운)

// ─── [자가복구] 잘못된 코디네이터 상태 leave defer ──────────────────────────
// NETWORK_UP 콜백(emberNetworkInit 재진입 컨텍스트)에서 직접 emberResetNetworkState()
// 하면 스택 상태가 꼬이고, 이어지는 resume 로직이 복구를 덮어쓴다.
// 플래그만 세우고 emberAfTickCallback에서 안전하게 처리.
static volatile bool stale_coord_recover_pending = false;

// ─── [H-1] 슬롯 erase defer ────────────────────────────────────────────────
// OTA PREPARE 메시지 콜백에서 직접 erase 하면 수백ms 블로킹 발생.
// 플래그를 세우고 emberAfTickCallback에서 처리.
static bool        ota_erase_pending      = false;
static EmberNodeId pending_master_node    = 0x0000;
static uint8_t     pending_ota_tag        = 0;
static uint32_t    pending_ota_size       = 0;

static sl_sleeptimer_timer_handle_t bootload_timer;
static sl_sleeptimer_timer_handle_t verify_timer;
static sl_sleeptimer_timer_handle_t id_announce_timer;

// ─── [RSSI 측정] 상태머신 (tick 기반, 콜백 컨텍스트 안전) ─────────────────────
//   MEAS_CMD 수신 시 콜백에서는 파라미터만 적재(pending) → tick 에서 채널전환/
//   비콘송신/복귀/리포트 처리. 측정 윈도 동안 EM1 로 sleep 차단(루프 고속 회전).
typedef enum {
  MEAS_RX_IDLE = 0,     // 평시 (home 채널 ch0)
  MEAS_RX_ON_CHANNEL,   // 측정 채널 전환됨 (비콘 송신 또는 청취)
  MEAS_RX_REPORTING     // home 복귀 후 리포트 전송 대기(스태거)
} meas_rx_state_t;

static volatile bool    meas_cmd_pending  = false;   // 콜백→tick 인계
static uint8_t          meas_p_seq        = 0;       // 인계 파라미터(콜백이 채움)
static uint8_t          meas_p_tx_id      = 0;
static uint8_t          meas_p_channel    = 0;
static uint8_t          meas_p_nbeacons   = 0;

static meas_rx_state_t  meas_rx_state     = MEAS_RX_IDLE;
static uint8_t          meas_cur_seq      = 0;       // 처리 중 라운드 파라미터
static uint8_t          meas_cur_tx_id    = 0;
static uint8_t          meas_cur_channel  = 0;
static uint8_t          meas_cur_nbeacons = 0;
static uint16_t         meas_home_channel = 0;
static uint32_t         meas_win_start    = 0;       // 측정 윈도 시작 tick
static uint32_t         meas_last_beacon  = 0;
static uint8_t          meas_beacons_sent = 0;
static int32_t          meas_rssi_sum     = 0;       // 청취 누적(평균용)
static uint16_t         meas_rssi_cnt     = 0;
static int8_t           meas_last_lqi     = 0;
static uint32_t         meas_report_due   = 0;
static bool             meas_no_sleep_req = false;

// ─── [IQ 측정] 캡처 결과 + 프래그먼트 리포트 상태 ───────────────────────────
//   청취자일 때 측정 슬롯 안에서 iq_capture_burst() 로 원시 IQ 를 취득한 뒤,
//   home 복귀 후 TX(Master)로 조각내어 전송한다(Connect 페이로드 제한 회피).
static iq_sample_t      meas_iq_buf[IQ_SAMPLES_PER_LINK];
static uint16_t         meas_iq_n         = 0;      // 이번 라운드 캡처한 복소샘플 수
static bool             meas_iq_done      = false;  // 이번 라운드 캡처 완료 여부
static uint8_t          meas_iq_frag      = 0;      // 다음 전송할 프래그먼트 인덱스

// ─── [IQ 중계] Master 로 직접 못 보낸 레코드를 형제 노드를 거쳐 전달 ─────────
//  [왜 필요한가]
//    각 노드는 자기가 측정한 IQ 를 Master 에게 직접 유니캐스트로 보낸다. 그런데
//    Master 와의 링크만 나빠지면(예: 날개 전개로 이 노드만 가려짐) 그 노드가
//    측정한 값 — 형제끼리 측정한 것까지 포함해 — 이 전부 사라진다.
//  [해결]
//    송신이 실패하면 그 레코드를 큐에 넣고, home 채널에서 브로드캐스트로
//    형제들에게 뿌린다(MSG_TYPE_IQ_RELAY). 이를 들은 형제는 같은 프레임을
//    Master 에게 유니캐스트로 그대로 넘긴다. rx_id 는 원래 측정자 값을 유지하므로
//    Master 는 누가 측정한 값인지 정확히 안다.
//    부수 효과: 브로드캐스트는 Master 도 직접 듣는다. 링크가 비대칭(데이터는
//    가는데 ACK 만 유실)이면 형제를 거치지 않고 여기서 바로 복구된다.
//  [왜 브로드캐스트인가]
//    이 노드는 스타 자식이라 형제의 주소를 모르고, 형제로의 유니캐스트는
//    코디네이터(Master)를 거치므로 Master 링크가 끊긴 상황에서 무의미하다.
//    브로드캐스트는 MAC 레벨이라 형제에게 직접 닿는다(MEAS_BEACON 과 동일 원리).
#define IQ_RELAY_QLEN   8U          // 링 크기 8 → 실보관 7건 (7 × 261B ≈ 1.8KB)
static iq_record_t      iq_relay_q[IQ_RELAY_QLEN];
static uint8_t          iq_relay_head = 0;
static uint8_t          iq_relay_tail = 0;
static uint8_t          iq_relay_frag = 0;      // 현재 브로드캐스트 중인 조각
static uint32_t         iq_relay_tick = 0;      // 조각 간격 타이머
static volatile bool    iq_send_failed = false; // 콜백→tick 인계: 리포트 전송이 실패했나

static uint8_t iq_relay_count(void)
{
  return (uint8_t)((iq_relay_head - iq_relay_tail) & (uint8_t)(IQ_RELAY_QLEN - 1U));
}

static void meas_rx_tick(void);
static void meas_rx_send_iq_frag(uint8_t frag_idx);

// 마지막 조각(n4 frag3)이 수집창(MEAS_COLLECT_MS) 안에 들어가는지 컴파일 단계에서 확인.
//   벗어나면 Master 가 이미 다음 라운드로 넘어간 뒤 도착해 통째로 버려진다.
_Static_assert(MEAS_REPORT_BASE_MS
                 + (4U * IQ_N_FRAGS - 1U) * MEAS_REPORT_STEP_MS
                 < MEAS_COLLECT_MS,
               "IQ report schedule overflows the collect window");

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void emberAfIncomingMessageCallback(EmberIncomingMessage *message)
{
  if (message->length < 1) return;

  uint8_t msg_type = message->payload[0];

  if (message->endpoint == CUSTOM_ENDPOINT) {
    if (msg_type == MSG_TYPE_POLL_REQUEST) {
      handle_poll_request(message);
      return;
    }
    if (msg_type == MSG_TYPE_OTA_PREPARE) {
      handle_ota_prepare_msg(message);
      return;
    }
    // ─── [IQ 중계] 형제가 Master 로 못 보낸 조각 → 내가 대신 전달 ─────────────
    if (msg_type == MSG_TYPE_IQ_RELAY) {
      iq_relay_forward(message->payload, (uint8_t)message->length);
      return;
    }
    // ─── [RSSI 측정] TX 의 라운드 명령 → tick 에 인계(채널전환은 콜백에서 안 함) ──
    if (msg_type == MSG_TYPE_MEAS_CMD) {
      if (message->length < 5) return;
      if (ota_download_active) return;            // OTA 중엔 측정 금지
      meas_p_seq      = message->payload[1];
      meas_p_tx_id    = message->payload[2];
      meas_p_channel  = message->payload[3];
      meas_p_nbeacons = message->payload[4];
      meas_cmd_pending = true;
      return;
    }
    // ─── [RSSI 측정] 측정 채널에서 비콘 수신 → RSSI 누적 ───────────────────────
    if (msg_type == MSG_TYPE_MEAS_BEACON) {
      if (message->length < 4) return;
      // 현재 라운드의 송신원/채널과 일치할 때만 집계.
      if (meas_rx_state == MEAS_RX_ON_CHANNEL
          && message->payload[2] == meas_cur_tx_id
          && (message->payload[3] & 0x7FU) == (meas_cur_channel & 0x7FU)) {
        meas_rssi_sum += message->rssi;
        meas_last_lqi  = (int8_t)message->lqi;
        meas_rssi_cnt++;
      }
      return;
    }
  }

  if (message->endpoint == 13) return;

  app_log_info("RX from 0x%04X: ep=%d, type=0x%02X, len=%d\n",
               message->source, message->endpoint, msg_type, message->length);
}

void emberAfMessageSentCallback(EmberStatus status, EmberOutgoingMessage *message)
{
  // [IQ 중계] Master 로 보낸 IQ 리포트가 실패했는지 먼저 본다.
  //   아래의 "측정 중엔 실패를 무시" 가드보다 앞에 두어야 한다(리포트는 측정
  //   상태에서 나가므로, 뒤에 두면 실패를 영영 못 본다).
  if (status != EMBER_SUCCESS && message != NULL && message->length > 0
      && message->payload != NULL
      && message->payload[0] == MSG_TYPE_IQ_REPORT) {
    iq_send_failed = true;
  }
  // [IQ 중계] 중계 방송(브로드캐스트)의 실패는 부모 도달성과 무관하다 —
  //   soft-rejoin 판정에 넣지 않는다(위 옵션 주석과 함께 이중 방어).
  if (message != NULL && message->length > 0 && message->payload != NULL
      && message->payload[0] == MSG_TYPE_IQ_RELAY) {
    return;
  }

  if (status == EMBER_SUCCESS) {
    consec_send_fails = 0;
    return;
  }
  // [OTA 보호] 다운로드 중엔 송신 실패로 soft-rejoin 하지 않는다.
  //   (OTA 혼잡으로 ID_ANNOUNCE ACK가 실패하는 것뿐 → 네트워크를 나가면 OTA 중단)
  if (ota_download_active) {
    consec_send_fails = 0;
    return;
  }
  // [IQ 보호] 측정 라운드 중엔 채널을 잠깐씩 이탈하므로 그 사이 걸린 송신은
  //   실패해도 정상이다(OTA 가드와 동일 논리). 여기서 세지 않으면 측정
  //   캠페인이 반복될 때마다 카운터가 쌓여 soft-rejoin 폭주로 이어진다.
  if (meas_rx_state != MEAS_RX_IDLE) {
    consec_send_fails = 0;
    return;
  }
  app_log_info("TX fail: 0x%02X (%u/%u)\n",
               status, consec_send_fails + 1, MAX_CONSEC_SEND_FAILS);

  if (network_joined && (++consec_send_fails >= MAX_CONSEC_SEND_FAILS)) {
    app_log_error("Parent unreachable. Soft rejoin.\n");
    consec_send_fails = 0;
    network_joined    = false;
    rejoin_backoff_ms = JOIN_RETRY_MIN_MS;   // backoff 초기화: TX 재부팅 후 빠른 재연결
    schedule_rejoin();
  }
}

void emberAfStackStatusCallback(EmberStatus status)
{
  switch (status) {
    case EMBER_NETWORK_UP:
      app_log_info("Network UP. NodeID=0x%04X, device_id=%d\n",
                   emberGetNodeId(), my_device_id);

      // ─── [자가복구] 잘못된 코디네이터 상태 감지 ──────────────────────────
      //   RX는 End Device라 NodeID는 0x0001~ 여야 정상. NodeID==0x0000(코디네이터)
      //   이면 과거 TX 펌웨어가 form한 네트워크 상태가 NVM3에 남아 resume된 것.
      //   UART 접근 불가한 위성에서 leave 명령을 줄 수 없으므로 자력으로 비우고
      //   rejoin 한다. (이 경로가 없으면 영구 deadlock = brick)
      //   ★ 실제 leave 는 콜백(emberNetworkInit 재진입) 밖 tick에서 처리 — 여기서
      //      직접 emberResetNetworkState() 하면 스택 꼬임 + resume 로직이 덮어씀.
      if (emberGetNodeId() == EMBER_COORDINATOR_ADDRESS) {
        app_log_error("[RECOVER] Stale coordinator state (NodeID=0x0000). "
                      "Will reset network in tick.\n");
        network_joined   = false;
        join_in_progress = false;
        net_up_tick      = 0;
        sl_sleeptimer_stop_timer(&id_announce_timer);
        stale_coord_recover_pending = true;   // tick에서 leave+rejoin
        break;
      }

      network_joined    = true;
      join_in_progress  = false;
      rejoin_backoff_ms = JOIN_RETRY_MIN_MS;
      consec_send_fails = 0;
      block_sleep_while_unjoined(false);   // 가입됨 → sleep 허용(저전력 복귀)
      net_up_tick       = sl_sleeptimer_get_tick_count();  // healthy 안정성 측정 시작
      // ─── [H-2] ID_ANNOUNCE: 1초 후 최초 전송, 이후 30초마다 재전송 ─────
      // TX가 리셋된 후에도 slave 등록이 자동 복구됨.
      sl_sleeptimer_stop_timer(&id_announce_timer);
      sl_sleeptimer_start_timer_ms(&id_announce_timer,
                                   ID_ANNOUNCE_DELAY_MS,
                                   id_announce_timer_cb, NULL, 0, 0);
      log_flush();   // [디버깅] join 로그 즉시 표시
      break;

    case EMBER_NETWORK_DOWN:
      app_log_info("Network DOWN.\n");
      network_joined    = false;
      join_in_progress  = false;
      net_up_tick       = 0;
      rejoin_backoff_ms = JOIN_RETRY_MIN_MS;   // backoff 초기화: TX 재부팅 후 빠른 재연결
      block_sleep_while_unjoined(true);
      sl_sleeptimer_stop_timer(&id_announce_timer);
      schedule_rejoin();
      break;

    default:
      app_log_info("Stack status: 0x%02X\n", status);
      if (join_in_progress) {
        join_in_progress = false;
        schedule_rejoin();
      }
      break;
  }
}

void emberAfTickCallback(void)
{
  fw_guard_feed_watchdog();   // [롤백 가드] tick 살아있음을 워치독에 보고

  // ─── [자가복구] 잘못된 코디네이터 상태 → 네트워크 비우고 재부팅 ───────────
  //   NETWORK_UP에서 NodeID==0x0000 감지 시: 수동 "leave→reset"과 동일하게
  //   네트워크 상태만 NVM3에서 비운 뒤 곧바로 재부팅한다.
  //   (reset 직후 곧장 join 하면 스택 미준비로 0x8E 실패 → 재부팅이 가장 확실)
  //   다음 부팅에서 emberNetworkInit이 NOT_JOINED 반환 → 깨끗하게 신규 join.
  if (stale_coord_recover_pending) {
    stale_coord_recover_pending = false;
    app_log_error("[RECOVER] Clearing stale network state and rebooting...\n");
    emberResetNetworkState();   // NVM3 네트워크 상태 소거(동기)
    NVIC_SystemReset();         // 복귀 안 함 → 다음 부팅에서 신규 join
  }

  // ─── [롤백 가드] 헬스 확인 / 부팅 타임아웃 self-reset ─────────────────────
  if (!boot_tick_set) {
    boot_tick     = sl_sleeptimer_get_tick_count();
    boot_tick_set = true;
  }
  if (!health_confirmed) {
    uint32_t now = sl_sleeptimer_get_tick_count();
    if (network_joined && net_up_tick != 0 && !ota_download_active
        && sl_sleeptimer_tick_to_ms(now - net_up_tick) >= HEALTH_STABILITY_MS) {
      // 네트워크가 충분히 안정적으로 유지됨 → 이 펌웨어는 정상.
      fw_guard_confirm_healthy();   // golden 캡처(느림, tick 컨텍스트라 허용)
      health_confirmed = true;
    } else if (fw_guard_is_on_probation()
               && sl_sleeptimer_tick_to_ms(now - boot_tick) >= BOOT_HEALTH_TIMEOUT_MS) {
      // probation 중인데 제한시간 내 healthy 못 됨 → 리셋(다음 부팅 카운터 누적→롤백)
      app_log_error("[GUARD] Boot health timeout — self-reset (probation count).\n");
      NVIC_SystemReset();
    }
  }

  // ─── [비행 안전] IQ 캡처 후 수신 복구 실패 상태면 계속 되살린다 ──────────
  //   방치하면 스택은 수신 중이라 믿는데 라디오는 꺼져 있어 노드가 조용히
  //   네트워크에서 이탈한다. 아래 미가입 자체리셋보다 훨씬 싸고 빠른 복구 경로다.
  if (iq_radio_is_deaf()) {
    if (iq_radio_recover()) {
      app_log_info("[IQ] radio RX recovered.\n");
    }
  }

  // ─── join/resume 워치독 ──────────────────────────────────────────────────
  if (!network_joined) {
    block_sleep_while_unjoined(true);   // 미가입 동안 sleep 차단 → rejoin tick 보장

    uint32_t now_tick = sl_sleeptimer_get_tick_count();

    // ─── [복구] 너무 오래 미가입 → 칩 리셋(깨끗한 재시작) ──────────────────
    //   join이 어떤 이유로든(스택 wedge, TX 부재 등) 장시간 안 되면 전체 리셋이
    //   가장 확실한 복구. tick은 살아있어 hang-워치독은 안 걸리므로 이 경로가 필요.
    //   정상 운용(pending_commit=0)에서는 리셋해도 롤백 카운터 증가 없음(안전).
    if (unjoined_since_tick == 0) {
      unjoined_since_tick = now_tick;
    } else if (sl_sleeptimer_tick_to_ms(now_tick - unjoined_since_tick)
                 > JOIN_GIVEUP_RESET_MS) {
      app_log_error("[RECOVER] Unjoined > %lums. Self-reset for clean restart.\n",
                    (unsigned long)JOIN_GIVEUP_RESET_MS);
      log_flush();
      NVIC_SystemReset();
    }

    uint32_t elapsed = sl_sleeptimer_tick_to_ms(now_tick - join_retry_anchor);

    if (join_in_progress) {
      if (elapsed > RESUME_WATCHDOG_MS) {
        app_log_error("Join/resume stalled (%lums). Reset+retry.\n", elapsed);
        join_in_progress = false;
        // JOINING 상태에 갇혔을 수 있으므로 강제로 NO_NETWORK로 되돌린 뒤 재시도.
        // (tick 컨텍스트라 reset 후 다음 tick에서 PHY 보정 끝나고 do_join 정상 시작)
        if (emberNetworkState() != EMBER_NO_NETWORK) {
          emberResetNetworkState();
        }
        rejoin_backoff_ms = JOIN_RETRY_MIN_MS;
        schedule_rejoin();
      }
      return;
    }

    if (elapsed >= rejoin_backoff_ms) {
      app_log_info("Rejoin (backoff=%lums)...\n", rejoin_backoff_ms);
      do_join();
      rejoin_backoff_ms = (rejoin_backoff_ms * 2U > JOIN_RETRY_MAX_MS)
                            ? JOIN_RETRY_MAX_MS : rejoin_backoff_ms * 2U;
    }
    return;
  }

  // 가입 상태 → 미가입 타이머 리셋
  unjoined_since_tick = 0;

  // ─── [RSSI 측정] 라운드 상태머신 (가입 + 비-OTA 일 때만) ─────────────────────
  meas_rx_tick();

  // ─── [IQ 중계] 미전달 레코드를 형제들에게 뿌린다(측정 쉬는 동안만) ──────────
  if (!ota_download_active) {
    iq_relay_tx_tick();
  }

  // ─── [H-1] OTA 슬롯 erase defer ─────────────────────────────────────────
  // 메시지 콜백에서 직접 erase 하면 수백ms 블로킹 → 여기서 처리
  if (ota_erase_pending) {
    ota_erase_pending = false;
    app_log_info("OTA: Erasing slot 0...\n");
    int32_t ret = bootloader_eraseStorageSlot(0);
    if (ret != BOOTLOADER_OK) {
      app_log_error("Slot erase FAILED: 0x%lX. Skipping OTA prepare.\n", ret);
      return;
    }
    master_node_id   = pending_master_node;
    ota_expected_tag  = pending_ota_tag;
    ota_expected_size = pending_ota_size;
    ota_prepare_done  = true;
    app_log_info("Slot 0 erased. Ready for OTA (tag=0x%02X, size=%lu).\n",
                 ota_expected_tag, ota_expected_size);
    send_prepare_ack(master_node_id);
  }
}

// ─── OTA Unicast Bootloader Client 콜백 ─────────────────────────────────────

bool emberAfPluginOtaUnicastBootloaderClientNewIncomingImageCallback(
  EmberNodeId serverId, uint8_t imageTag, uint32_t imageSize, uint32_t *startIndex)
{
  app_log_info("OTA: New image from 0x%04X, tag=0x%02X, size=%lu\n",
               serverId, imageTag, imageSize);
  ota_download_active = true;   // [OTA 보호] 다운로드 시작 → 자가 교란 억제
  ota_expected_tag  = imageTag;
  ota_expected_size = imageSize;
  *startIndex = 0;
  return true;
}

void emberAfPluginOtaUnicastBootloaderClientIncomingImageSegmentCallback(
  EmberNodeId serverId, uint32_t startIndex, uint32_t endIndex,
  uint8_t imageTag, uint8_t *imageSegment)
{
  (void)serverId; (void)imageTag;
  ota_download_active = true;   // [OTA 보호] 세그먼트 수신 중 → 자가 교란 억제(보강)
  uint32_t len = endIndex - startIndex + 1;
  int32_t ret  = bootloader_writeStorage(0, startIndex, imageSegment, len);
  if (ret != BOOTLOADER_OK) {
    app_log_error("OTA: writeStorage FAIL at %lu: 0x%lX\n", startIndex, ret);
  } else {
    // ─── [L-1] 진행률 100%까지 정확히 출력 ─────────────────────────────
    if (ota_expected_size > 0) {
      static uint32_t last_pct = 0;
      uint32_t pct = ((endIndex + 1) * 100) / ota_expected_size;
      if (pct / 10 > last_pct / 10) {
        app_log_info("OTA RX: %lu%% (%lu/%lu bytes)\n",
                     pct, endIndex + 1, ota_expected_size);
        last_pct = pct;
      }
      if (pct >= 100) last_pct = 0;  // 다음 세션을 위해 초기화
    }
  }
}

void emberAfPluginOtaUnicastBootloaderClientImageDownloadCompleteCallback(
  EmberAfOtaUnicastBootloaderStatus status, uint8_t imageTag, uint32_t imageSize)
{
  ota_download_active = false;   // [OTA 보호] 다운로드 종료(성공/실패) → 정상 동작 복귀
  if (status == EMBER_OTA_UNICAST_BOOTLOADER_STATUS_SUCCESS) {
    app_log_info("OTA: Download COMPLETE. tag=0x%02X, size=%lu\n",
                 imageTag, imageSize);
    image_verified = false;
    sl_sleeptimer_start_timer_ms(&verify_timer, 100, verify_timer_cb, NULL, 0, 0);
  } else {
    app_log_error("OTA: Download FAILED, status=0x%02X\n", status);
    image_verified = false;
  }
}

bool emberAfPluginOtaUnicastBootloaderClientIncomingRequestBootloadCallback(
  EmberNodeId serverId, uint8_t imageTag, uint32_t bootloadDelayMs)
{
  (void)imageTag;
  app_log_info("OTA: Bootload request from 0x%04X, delay=%lums.\n",
               serverId, bootloadDelayMs);
  if (!image_verified) {
    app_log_error("OTA: Image not verified. Refusing.\n");
    return false;
  }
  app_log_info("OTA: Rebooting in %lums...\n", bootloadDelayMs);
  sl_sleeptimer_start_timer_ms(&bootload_timer, bootloadDelayMs,
                               bootload_timer_cb, NULL, 0, 0);
  image_verified = false;
  return true;
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

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

// ─── [측정] 측정 윈도 동안 tick 고속 유지(슬롯/비콘 타이밍 보장) ──────────────
//   ★ EM1 요구만으로는 부족하다. EM1 은 깊은 잠(EM2)만 막고, CPU 는 다음
//     인터럽트까지 얕은 잠을 잔다. 측정 중 상태 전이가 제때 일어나려면 주기적
//     웨이크업이 필요하다(이 파일 위쪽 block_sleep_while_unjoined 의 1초 타이머와
//     같은 기법, 주기만 짧게). TX 와 슬롯 타이밍이 어긋나면 비콘을 놓친다.
#define MEAS_TICK_MS   10U

static sl_sleeptimer_timer_handle_t meas_wake_timer;

static void meas_wake_cb(sl_sleeptimer_timer_handle_t *h, void *d)
{
  (void)h; (void)d;   // 빈 콜백 — CPU 를 깨우는 것 자체가 목적
}

static void meas_rx_set_sleep_block(bool block)
{
  if (block && !meas_no_sleep_req) {
    sl_power_manager_add_em_requirement(SL_POWER_MANAGER_EM1);
    sl_sleeptimer_start_periodic_timer_ms(&meas_wake_timer, MEAS_TICK_MS,
                                          meas_wake_cb, NULL, 0, 0);
    meas_no_sleep_req = true;
  } else if (!block && meas_no_sleep_req) {
    sl_sleeptimer_stop_timer(&meas_wake_timer);
    sl_power_manager_remove_em_requirement(SL_POWER_MANAGER_EM1);
    meas_no_sleep_req = false;
  }
}

// ─── [IQ] 연속 송신(TX stream) — 송신원 전용 ────────────────────────────────
//   ★ 안전 최우선: 스트림이 켜진 채로 남으면 그 채널을 계속 점유(재밍)하고
//     자신은 네트워크에서 이탈한다. stop 은 슬롯 종료·OTA 선점 등 모든 경로에서
//     무조건 호출하고, idempotent(이미 꺼져 있으면 무해)하게 만든다.
static bool meas_rx_streaming = false;

// ─── [안전장치] 송신 스트림 감시 타이머 (TX 프로젝트와 동일 조치) ────────────
//   스트림 정지는 tick 이 담당하는데, tick 이 오래 굶으면 스트림이 슬롯을 넘겨
//   계속 송신한다. 그러면 Connect MAC 이 아무것도 보내지 못해 송신 큐가 가득 차고
//   이후 모든 전송이 0x39(MAC_TRANSMIT_QUEUE_FULL)로 영구 실패한다.
//   tick 에 의존하지 않는 sleeptimer 콜백으로 강제 정지시킨다.
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

static void meas_rx_stream_start(uint8_t channel)
{
  if (meas_rx_streaming) return;
  // RAIL 레벨 PN9 스트림(스택 상태 검사 없음 — iq_capture.h 주석 참조).
  if (iq_stream_start(channel)) {
    meas_rx_streaming = true;
    if (sl_sleeptimer_start_timer_ms(&meas_stream_guard, MEAS_STREAM_GUARD_MS,
                                     meas_stream_guard_cb, NULL, 0, 0)
        == SL_STATUS_OK) {
      meas_stream_guard_armed = true;
    }
  } else {
    app_log_error("[MEAS] TX stream start failed (ch=%u)\n", (unsigned)channel);
  }
}

static void meas_rx_stream_stop(void)
{
  if (meas_stream_guard_armed) {
    sl_sleeptimer_stop_timer(&meas_stream_guard);
    meas_stream_guard_armed = false;
  }
  if (!meas_rx_streaming) return;
  // 중지 후 같은 측정 채널에서 수신 재개(슬롯 종료 시 호출측이 home 으로 복귀).
  iq_stream_stop(meas_cur_channel & 0x7FU);
  meas_rx_streaming = false;
}

// ─── [구] 빈 비콘 1개 브로드캐스트 — 연속 송신 전환으로 미사용 ────────────────
__attribute__((unused))
static void meas_rx_send_beacon(uint8_t beacon_idx)
{
  uint8_t msg[5] = {
    MSG_TYPE_MEAS_BEACON, meas_cur_seq, meas_cur_tx_id, meas_cur_channel, beacon_idx
  };
  // 브로드캐스트(단일홉) → 이웃 RX/TX가 직접 듣고 RSSI 측정. ACK 불가 → OPTIONS_NONE.
  emberMessageSend(EMBER_BROADCAST_ADDRESS, CUSTOM_ENDPOINT, 0,
                   sizeof(msg), msg, EMBER_OPTIONS_NONE);
}

// ─── [RSSI 측정] 청취 결과를 TX(coordinator)로 리포트 ────────────────────────
//   [IQ 전환] 청취자는 이제 IQ 모드로 캡처하므로 이 RSSI 리포트는 사용하지 않음.
//   (참고용으로 남겨둠 — RSSI 병행 측정 재도입 시 재사용)
__attribute__((unused))
static void meas_rx_send_report(void)
{
  int8_t avg = (meas_rssi_cnt > 0)
                 ? (int8_t)(meas_rssi_sum / (int32_t)meas_rssi_cnt) : 0;
  uint8_t cnt8 = (meas_rssi_cnt > 255) ? 255 : (uint8_t)meas_rssi_cnt;
  uint8_t msg[8] = {
    MSG_TYPE_MEAS_REPORT, meas_cur_seq, meas_cur_tx_id, meas_cur_channel,
    my_device_id, (uint8_t)avg, (uint8_t)meas_last_lqi, cnt8
  };
  emberMessageSend(EMBER_COORDINATOR_ADDRESS, CUSTOM_ENDPOINT, 0,
                   sizeof(msg), msg, tx_options);
}

// ─── [RSSI 측정] tick 상태머신 ───────────────────────────────────────────────
static void meas_rx_tick(void)
{
  // ─── [안전장치] 스트림 감시 타이머가 발동했다면 측정을 안전 종료 ──────────
  //   송신은 콜백이 이미 멈췄다. 여기서 채널 원복 + 수신 재개까지 마무리한다.
  if (meas_stream_guard_fired) {
    meas_stream_guard_fired = false;
    meas_stream_guard_armed = false;
    meas_rx_streaming       = false;   // 콜백이 이미 껐다
    app_log_error("[MEAS] stream guard fired (tick starved) — returning home.\n");
    emberSetRadioChannelExtended(meas_home_channel, false);
    meas_rx_set_sleep_block(false);
    meas_rx_state = MEAS_RX_IDLE;
  }

  // OTA 중이면 측정 중단하고 home 으로 안전 복귀.
  if (ota_download_active) {
    if (meas_rx_state != MEAS_RX_IDLE) {
      meas_rx_stream_stop();   // ★ 최우선: 스트림이 남으면 채널 점유 + OTA 방해
      emberSetRadioChannelExtended(meas_home_channel, false);
      meas_rx_set_sleep_block(false);
      meas_rx_state = MEAS_RX_IDLE;
    }
    meas_cmd_pending = false;
    return;
  }

  uint32_t now = sl_sleeptimer_get_tick_count();

  // ─── 새 라운드 명령 처리(IDLE 일 때만 수락) ───────────────────────────────
  if (meas_cmd_pending && meas_rx_state == MEAS_RX_IDLE) {
    meas_cmd_pending  = false;
    meas_cur_seq      = meas_p_seq;
    meas_cur_tx_id    = meas_p_tx_id;
    meas_cur_channel  = meas_p_channel;
    meas_cur_nbeacons = meas_p_nbeacons;

    meas_home_channel = emberGetRadioChannel();   // 복귀용 home 채널 캡처(보통 0)
    meas_rssi_sum = 0; meas_rssi_cnt = 0; meas_last_lqi = 0;
    meas_beacons_sent = 0;
    meas_iq_n = 0; meas_iq_done = false; meas_iq_frag = 0;   // [IQ] 라운드 초기화

    meas_rx_set_sleep_block(true);                // 윈도 동안 sleep 차단
    if (emberSetRadioChannelExtended(meas_cur_channel & 0x7FU, false) != EMBER_SUCCESS) {
      // 채널 전환 실패 → 즉시 포기(home 유지).
      meas_rx_set_sleep_block(false);
      return;
    }
    meas_win_start   = now;
    meas_last_beacon = now;
    meas_rx_state    = MEAS_RX_ON_CHANNEL;
    return;
  }

  // ─── 측정 채널 활동 ───────────────────────────────────────────────────────
  if (meas_rx_state == MEAS_RX_ON_CHANNEL) {
    uint32_t el = sl_sleeptimer_tick_to_ms(now - meas_win_start);

    // ─── 내가 송신원이면: 슬롯 동안 연속 송신(TX stream) ─────────────────────
    //   짧은 비콘 대신 끊김 없는 스트림을 쏘아, 리스너가 어느 시점에 캡처해도
    //   반드시 신호가 담기게 한다(비콘 방식은 캡처 창과 겹칠 확률이 사실상 0).
    if (meas_cur_tx_id == my_device_id) {
      if (!meas_rx_streaming && el >= MEAS_STREAM_START_MS
          && el < MEAS_STREAM_STOP_MS) {
        meas_rx_stream_start(meas_cur_channel & 0x7FU);
      } else if (meas_rx_streaming && el >= MEAS_STREAM_STOP_MS) {
        meas_rx_stream_stop();                 // 슬롯 종료 전 반드시 정지
      }
    }

    // ─── [IQ] 내가 청취자면: 스트림이 한창일 때 IQ 버스트 1회 캡처 ────────────
    //   iq_capture_burst() 는 RAIL 을 잠깐 IQ 모드로 빌렸다 패킷 모드로 원복한다
    //   (블로킹, 상한 IQ_CAPTURE_TIMEOUT_MS). 송신원이 연속 송신 중이므로 이
    //   시점의 원시 IQ 에는 반드시 상대 신호가 담긴다.
    if (meas_cur_tx_id != my_device_id && !meas_iq_done
        && el >= MEAS_CAPTURE_AT_MS && iq_capture_ready()) {
      // 캡처 후에도 같은 측정 채널에서 수신을 이어간다(슬롯 종료 시 home 복귀).
      meas_iq_n = iq_capture_burst(meas_cur_channel & 0x7FU,
                                   meas_cur_channel & 0x7FU,
                                   meas_iq_buf,
                                   IQ_SAMPLES_PER_LINK, IQ_DECIM,
                                   IQ_CAPTURE_TIMEOUT_MS);
      meas_iq_done = true;
    }

    // 슬롯 종료 → home 복귀.
    if (el >= MEAS_SLOT_MS) {
      meas_rx_stream_stop();                   // 보험: 무조건 정지
      emberSetRadioChannelExtended(meas_home_channel, false);
      // [IQ] 청취자였고 샘플을 얻었으면 프래그먼트 리포트 예약(device_id 스태거).
      if (meas_cur_tx_id != my_device_id && meas_iq_n > 0) {
        meas_report_due = now;   // 기준 시각; 아래에서 스태거 적용
        meas_iq_frag    = 0;
        meas_rx_state   = MEAS_RX_REPORTING;
      } else {
        meas_rx_set_sleep_block(false);
        meas_rx_state = MEAS_RX_IDLE;
      }
    }
    return;
  }

  // ─── home 복귀 후 IQ 프래그먼트 전송 ──────────────────────────────────────
  //   노드별 스태거(device_id * REPORT_GAP)로 시작 시점을 벌리고,
  //   프래그먼트끼리도 IQ_FRAG_GAP_MS 간격을 둬 TX 큐 고갈/충돌을 막는다.
  if (meas_rx_state == MEAS_RX_REPORTING) {
    uint32_t since = sl_sleeptimer_tick_to_ms(now - meas_report_due);
    // 16개 슬롯(노드 4 × 조각 4)을 겹치지 않게 배치 — 헤더의 상수 주석 참조.
    //   device_id 는 1~4. 미프로비저닝(0xFF)이면 조인하지 않아 여기 오지 않지만,
    //   방어적으로 묶어 둔다(슬롯 인덱스가 폭주해 수집창을 벗어나지 않게).
    uint8_t  dev  = (my_device_id >= 1U && my_device_id <= 4U) ? my_device_id : 1U;
    uint32_t slot = (uint32_t)(dev - 1U) * IQ_N_FRAGS + meas_iq_frag;
    uint32_t due  = MEAS_REPORT_BASE_MS + slot * MEAS_REPORT_STEP_MS;
    if (since >= due) {
      meas_rx_send_iq_frag(meas_iq_frag);
      meas_iq_frag++;
      if (meas_iq_frag >= IQ_N_FRAGS) {
        // [IQ 중계] 조각 중 하나라도 Master 로 못 갔으면 형제를 통해 재시도한다.
        //   Master 는 조각이 전부 모여야 레코드를 확정하므로, 부분 실패도
        //   결국 레코드 전체 유실이다 → 레코드 통째로 큐에 넣는다.
        if (iq_send_failed && meas_iq_n > 0) {
          iq_record_t *r = &iq_relay_q[iq_relay_head];
          r->tx_id   = meas_cur_tx_id;
          r->rx_id   = my_device_id;
          r->channel = meas_cur_channel;
          r->seq     = meas_cur_seq;
          r->n_samp  = (meas_iq_n > IQ_SAMPLES_PER_LINK)
                         ? (uint8_t)IQ_SAMPLES_PER_LINK : (uint8_t)meas_iq_n;
          for (uint8_t k = 0; k < r->n_samp; k++) r->iq[k] = meas_iq_buf[k];
          iq_relay_head = (uint8_t)((iq_relay_head + 1U) & (uint8_t)(IQ_RELAY_QLEN - 1U));
          if (iq_relay_head == iq_relay_tail) {   // 가득 참 → 가장 오래된 것 폐기
            iq_relay_tail = (uint8_t)((iq_relay_tail + 1U) & (uint8_t)(IQ_RELAY_QLEN - 1U));
          }
          app_log_info("[IQ] report to master failed → queued for relay (%u)\n",
                       (unsigned)iq_relay_count());
        }
        iq_send_failed = false;
        meas_rx_set_sleep_block(false);
        meas_rx_state = MEAS_RX_IDLE;
      }
    }
    return;
  }
}

// ─── [IQ 측정] 캡처한 IQ 를 조각내어 TX(Master)로 전송 ───────────────────────
//   Connect 앱 페이로드(~111B) 제한 때문에 IQ_FRAG_SAMPLES 개씩 나눠 보낸다.
//   payload: [0]=type [1]seq [2]tx_id [3]ch [4]rx_id [5]frag_idx [6]n_frags
//            [7]n_samp  [8..]= iq bytes (n_samp*4, LE)
static void meas_rx_send_iq_frag(uint8_t frag_idx)
{
  uint16_t start = (uint16_t)frag_idx * IQ_FRAG_SAMPLES;
  if (start >= meas_iq_n) {
    // 이 조각에 실을 샘플이 없으면 빈(n_samp=0) 조각을 보내 프레임 수를 맞춘다.
    start = meas_iq_n;
  }
  uint16_t nsamp = meas_iq_n - start;
  if (nsamp > IQ_FRAG_SAMPLES) nsamp = IQ_FRAG_SAMPLES;

  uint8_t msg[IQ_REPORT_HDR + IQ_FRAG_SAMPLES * 4U];
  msg[0] = MSG_TYPE_IQ_REPORT;
  msg[1] = meas_cur_seq;
  msg[2] = meas_cur_tx_id;
  msg[3] = meas_cur_channel;
  msg[4] = my_device_id;
  msg[5] = frag_idx;
  msg[6] = (uint8_t)IQ_N_FRAGS;
  msg[7] = (uint8_t)nsamp;
  for (uint16_t k = 0; k < nsamp; k++) {
    const iq_sample_t *s = &meas_iq_buf[start + k];
    uint8_t *p = &msg[IQ_REPORT_HDR + k * 4U];
    p[0] = (uint8_t)((uint16_t)s->i & 0xFF);
    p[1] = (uint8_t)(((uint16_t)s->i >> 8) & 0xFF);
    p[2] = (uint8_t)((uint16_t)s->q & 0xFF);
    p[3] = (uint8_t)(((uint16_t)s->q >> 8) & 0xFF);
  }
  // ★ 반환값을 반드시 본다. MAC 큐가 차 있으면 여기서 즉시 실패하고 큐에 들어가지
  //   않는데, 그 경우 emberAfMessageSentCallback 은 호출되지 않는다 → 조각이
  //   흔적 없이 사라지고 iq_send_failed 도 안 켜져 중계까지 발동하지 못한다.
  if (emberMessageSend(EMBER_COORDINATOR_ADDRESS, CUSTOM_ENDPOINT, 0,
                       (uint8_t)(IQ_REPORT_HDR + nsamp * 4U), msg, tx_options)
      != EMBER_SUCCESS) {
    iq_send_failed = true;
  }
}

// [IQ 중계] 레코드 하나의 조각을 지정 타입/목적지로 전송한다.
//   meas_rx_send_iq_frag() 와 페이로드 형식이 완전히 동일하고, 원본이 되는
//   레코드와 목적지만 다르다(중계 브로드캐스트 / Master 로의 전달 양쪽에 쓴다).
static void iq_send_frag_of(const iq_record_t *rec, uint8_t frag_idx,
                            uint8_t msg_type, EmberNodeId dest)
{
  uint16_t start = (uint16_t)frag_idx * IQ_FRAG_SAMPLES;
  // ★ 실을 샘플이 없어도 "빈 조각(n_samp=0)"을 보내야 한다 — return 하면 안 된다.
  //   Master 는 frag 비트마스크가 (1<<IQ_N_FRAGS)-1 로 다 차야 레코드를 확정한다.
  //   캡처가 짧아 n_samp<=60 이면 마지막 조각(샘플 60..63)이 통째로 빠지고,
  //   마스크가 영원히 0b0111 에 머물러 중계한 레코드가 그대로 버려진다.
  //   (직접 전송 경로 meas_rx_send_iq_frag() 는 이미 이렇게 처리한다 — 동일하게 맞춘다.)
  if (start >= rec->n_samp) start = rec->n_samp;
  uint16_t nsamp = (uint16_t)rec->n_samp - start;
  if (nsamp > IQ_FRAG_SAMPLES) nsamp = IQ_FRAG_SAMPLES;

  uint8_t msg[IQ_REPORT_HDR + IQ_FRAG_SAMPLES * 4U];
  msg[0] = msg_type;
  msg[1] = rec->seq;
  msg[2] = rec->tx_id;
  msg[3] = rec->channel;
  msg[4] = rec->rx_id;          // ★ 원래 측정자 유지 — 중계해도 출처가 안 바뀐다
  msg[5] = frag_idx;
  msg[6] = (uint8_t)IQ_N_FRAGS;
  msg[7] = (uint8_t)nsamp;
  for (uint16_t k = 0; k < nsamp; k++) {
    const iq_sample_t *sp = &rec->iq[start + k];
    uint8_t *p = &msg[IQ_REPORT_HDR + k * 4U];
    p[0] = (uint8_t)((uint16_t)sp->i & 0xFF);
    p[1] = (uint8_t)(((uint16_t)sp->i >> 8) & 0xFF);
    p[2] = (uint8_t)((uint16_t)sp->q & 0xFF);
    p[3] = (uint8_t)(((uint16_t)sp->q >> 8) & 0xFF);
  }
  // ★ 브로드캐스트에는 ACK 라는 것이 없다. tx_options(=ACK_REQUESTED)를 그대로
  //   쓰면 전송이 실패로 처리되어 두 가지가 동시에 터진다:
  //     (1) 중계 프레임이 한 바이트도 나가지 않는다 → 중계 기능 자체가 죽는다.
  //     (2) emberAfMessageSentCallback 에서 consec_send_fails 가 쌓이고,
  //         중계 방송은 MEAS_RX_IDLE 에서만 나가므로 "측정 중 실패 무시" 가드에도
  //         걸리지 않는다 → 3회(=60ms) 만에 soft-rejoin, 노드가 네트워크를 이탈한다.
  //   이 코드베이스의 다른 브로드캐스트(MEAS_CMD / MEAS_BEACON)와 동일하게 맞춘다.
  EmberMessageOptions opt = (dest == EMBER_BROADCAST_ADDRESS)
                              ? EMBER_OPTIONS_NONE : tx_options;
  emberMessageSend(dest, CUSTOM_ENDPOINT, 0,
                   (uint8_t)(IQ_REPORT_HDR + nsamp * 4U), msg, opt);
}

// [IQ 중계] 큐에 쌓인 미전달 레코드를 형제들에게 브로드캐스트한다.
//   측정이 쉬고 있을 때(home 채널, IDLE)만 보내 측정 슬롯을 방해하지 않는다.
static void iq_relay_tx_tick(void)
{
  if (iq_relay_count() == 0U) return;
  if (meas_rx_state != MEAS_RX_IDLE) return;   // 측정 중엔 채널이 다르다
  if (!network_joined) return;

  uint32_t now = sl_sleeptimer_get_tick_count();
  if (sl_sleeptimer_tick_to_ms(now - iq_relay_tick) < IQ_FRAG_GAP_MS) return;
  iq_relay_tick = now;

  const iq_record_t *r = &iq_relay_q[iq_relay_tail];
  iq_send_frag_of(r, iq_relay_frag, MSG_TYPE_IQ_RELAY, EMBER_BROADCAST_ADDRESS);

  if (++iq_relay_frag >= IQ_N_FRAGS) {         // 한 레코드 다 뿌림 → 다음 것
    iq_relay_frag = 0;
    iq_relay_tail = (uint8_t)((iq_relay_tail + 1U) & (uint8_t)(IQ_RELAY_QLEN - 1U));
  }
}

// [IQ 중계] 형제가 뿌린 조각을 받아 Master 로 그대로 넘긴다.
//   ★ 타입만 IQ_REPORT 로 바꿔 보내므로, 받은 쪽이 다시 중계하지 않는다(루프 불가).
//   ★ 조립하지 않고 조각 단위로 즉시 전달 — 상태를 안 가져 단순하고 견고하다.
static void iq_relay_forward(const uint8_t *p, uint8_t len)
{
  if (len < IQ_REPORT_HDR) return;
  if (!network_joined) return;                 // 나도 Master 를 못 보면 소용없다
  if (ota_download_active) return;             // OTA 중엔 대역폭을 건드리지 않는다
  uint8_t msg[IQ_REPORT_HDR + IQ_FRAG_SAMPLES * 4U];
  if (len > sizeof(msg)) len = (uint8_t)sizeof(msg);
  for (uint8_t i = 0; i < len; i++) msg[i] = p[i];
  // ★ 타입을 IQ_REPORT 로 바꾸지 않고 IQ_RELAY 그대로 Master 에게 유니캐스트한다.
  //   (1) 이 전송이 실패해도 emberAfMessageSentCallback 의 IQ_REPORT 검사에
  //       걸리지 않는다 → 남의 중계 실패를 "내 리포트 실패"로 오인해 내 정상
  //       레코드를 쓸데없이 재방송하는 일이 없다.
  //   (2) Master 가 중계본을 직접본과 다른 조립 슬롯에 넣을 수 있다 → 중계 조각의
  //       frag0 이 진행 중인 직접 전송 조립을 밀어내 레코드를 잃는 일이 없다.
  //   (3) 유니캐스트라 형제는 이 프레임을 받지 않는다 → 재중계 루프 불가.
  emberMessageSend(EMBER_COORDINATOR_ADDRESS, CUSTOM_ENDPOINT, 0, len, msg, tx_options);
}

static void handle_poll_request(EmberIncomingMessage *message)
{
  if (message->length < 2) return;
  uint8_t target_id = message->payload[1];
  if (target_id != my_device_id) return;
  send_poll_response(message->source);
}

static void send_poll_response(EmberNodeId requester)
{
  // [텔레메트리] guard 상태를 1바이트로 실어 보냄(TX 로그에서 golden 확인용):
  //   bit0 = golden_valid, bit1 = on_probation, bit2 = active_golden(0=A,1=B)
  uint8_t guard = 0;
  if (fw_guard_golden_valid())    guard |= 0x01;
  if (fw_guard_is_on_probation()) guard |= 0x02;
  if (fw_guard_active_golden())   guard |= 0x04;

  uint8_t msg[4] = {
    MSG_TYPE_POLL_RESPONSE,
    my_device_id,
    FW_VERSION_MAJOR,
    guard
  };
  EmberStatus status = emberMessageSend(requester, CUSTOM_ENDPOINT, 0,
                                        sizeof(msg), msg, tx_options);
  if (status == EMBER_SUCCESS) {
    app_log_info("[POLL] Response → 0x%04X (id=%d, fw=0x%02X)\n",
                 requester, my_device_id, FW_VERSION_MAJOR);
  } else {
    app_log_error("[POLL] Response err: 0x%02X\n", status);
  }
}

static void send_id_announce(void)
{
  if (my_device_id == 0xFF) {
    app_log_error("ID_ANNOUNCE skipped: device_id not provisioned.\n");
    return;
  }
  // [EUI64] device_id 뒤에 자기 EUI-64(8B, LSB first)를 실어 보냄.
  //   유선 접근 불가 시 TX 로그로 EUI-64를 확보 → g_eui64_map[] 채우기용.
  uint8_t *eui64 = emberGetEui64();
  uint8_t msg[10];
  msg[0] = MSG_TYPE_ID_ANNOUNCE;
  msg[1] = my_device_id;
  memcpy(&msg[2], eui64, 8);
  EmberStatus status = emberMessageSend(EMBER_COORDINATOR_ADDRESS,
                                        CUSTOM_ENDPOINT, 0,
                                        sizeof(msg), msg, tx_options);
  if (status == EMBER_SUCCESS) {
    app_log_info("ID_ANNOUNCE sent: device_id=%d\n", my_device_id);
  } else {
    app_log_error("ID_ANNOUNCE err: 0x%02X\n", status);
  }
}

/**************************************************************************//**
 * [H-2] ID_ANNOUNCE 타이머 콜백
 *   NETWORK_UP 후 1초에 최초 전송, 이후 30초마다 재전송(체이닝 타이머).
 *   TX가 리셋되더라도 slave가 자동으로 재등록됨.
 *****************************************************************************/
static void id_announce_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  if (network_joined) {
    // [OTA 보호] 다운로드 중엔 전송 억제(혼잡으로 ACK 실패 → soft-rejoin 유발 방지).
    //   타이머는 계속 돌려 OTA 종료 후 자동으로 announce 재개.
    // [IQ 보호] 측정 라운드 중에도 동일 이유로 억제(채널 이탈 중 전송 시도 방지).
    //   타이머는 계속 돌려 측정 종료 후 자동으로 announce 재개.
    if (!ota_download_active && meas_rx_state == MEAS_RX_IDLE) {
      send_id_announce();
    }
    sl_sleeptimer_start_timer_ms(&id_announce_timer,
                                 ID_ANNOUNCE_RETRY_MS,
                                 id_announce_timer_cb, NULL, 0, 0);
  }
}

static void do_join(void)
{
  // 안전 가드: 이미 join 시도/완료 중이면 중복 호출 방지(스택 상태 꼬임 차단).
  EmberNetworkStatus ns = emberNetworkState();

  // ─── [버그픽스] stale JOINED 복구 ────────────────────────────────────────
  //   소프트 리조인(부모 unreachable 3회)은 network_joined=false 만 내리고
  //   스택은 죽은 옛 부모에 EMBER_JOINED_NETWORK 로 그대로 묶여 있다.
  //   이 상태면 아래 NO_NETWORK 게이트에 매번 막혀 emberJoinNetwork()를 영영
  //   호출 못 하고(=재가입 불가), join_retry_anchor도 안 움직여 tick마다
  //   "Rejoin..." 로그만 폭주한다. → 강제로 네트워크를 내려 NO_NETWORK 로 만든다.
  //   (do_join 은 tick 컨텍스트에서만 호출되므로 emberResetNetworkState() 안전)
  if (ns == EMBER_JOINED_NETWORK) {
    app_log_info("Stale JOINED while unjoined → reset network state, then rejoin.\n");
    emberResetNetworkState();
    rejoin_backoff_ms = JOIN_RETRY_MIN_MS;
    join_retry_anchor = sl_sleeptimer_get_tick_count();  // 다음 틱 스핀/로그폭주 방지
    return;   // reset 완료(~다음 틱) 후 NO_NETWORK 되면 실제 join 진행
  }
  if (ns != EMBER_NO_NETWORK) {
    // JOINING 등 전이 상태 — 이번 틱은 건너뛰고 다음에 재시도.
    return;
  }

  EmberNetworkParameters params;
  memset(&params, 0, sizeof(params));
  params.radioChannel = 0;        // ★ 구펌웨어 SLAVE_NETWORK_CHANNEL=0, TX form 채널과 동일
  params.radioTxPower = 0;
  params.panId        = 0xFFFF;   // ★ 위성 RX 구펌웨어와 동일 PAN (변경 금지)

  join_retry_anchor = sl_sleeptimer_get_tick_count();

  EmberStatus status = emberJoinNetwork(EMBER_STAR_END_DEVICE, &params);
  if (status == EMBER_SUCCESS) {
    join_in_progress = true;
    app_log_info("Join initiated on ch=%d.\n", params.radioChannel);
  } else {
    join_in_progress = false;
    // 0x8E(PHY_CALIBRATING) 등 일시 실패 → tick이 backoff 후 자동 재시도.
    app_log_error("emberJoinNetwork FAILED: 0x%02X (tick will retry)\n", status);
  }
}

void join_sleep_block_enable(void)
{
  block_sleep_while_unjoined(true);
}

void start_initial_join(void)
{
  // ★★ init 컨텍스트에서 직접 do_join()을 호출하지 않는다.
  //   emberAfInitCallback()은 첫 emberTick() 이전이라 라디오 PHY 보정이
  //   끝나지 않았다. 이 시점에 emberJoinNetwork()를 부르면 항상
  //   EMBER_PHY_CALIBRATING(0x8E)로 실패한다(과거 로그의 0x8E 원인).
  //   → 첫 join을 tick의 rejoin 로직에 위임한다. tick은 sl_system_process_action
  //      → emberTick()(PHY 보정 완료) 이후 실행되므로 join이 정상 시작된다.
  //
  // ★★ EM2 sleep 차단(첫 sleep 이전에 선제 차단).
  //   미가입 End Device가 EM2로 자면 라디오가 꺼져 join 응답을 못 받는다.
  //   tick에서 막으면 이미 늦다(tick 자체가 sleep으로 안 돎) → 여기서 선제 차단.
  block_sleep_while_unjoined(true);
  rejoin_backoff_ms   = JOIN_RETRY_MIN_MS;
  unjoined_since_tick = 0;   // tick 첫 진입에서 현재 시각으로 설정됨
  // 첫 join을 ~FIRST_JOIN_DELAY_MS 후 tick에서 시도하도록 anchor를 과거로 당김.
  join_retry_anchor = sl_sleeptimer_get_tick_count()
        - sl_sleeptimer_ms_to_tick(JOIN_RETRY_MIN_MS - FIRST_JOIN_DELAY_MS);
}

static void schedule_rejoin(void)
{
  join_retry_anchor = sl_sleeptimer_get_tick_count();
}

static void verify_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  int32_t ret = bootloader_verifyImage(0, NULL);
  if (ret == BOOTLOADER_OK) {
    bootloader_setImageToBootload(0);
    image_verified = true;
    app_log_info("OTA: Image verified OK.\n");
  } else {
    image_verified = false;
    app_log_error("OTA: Image verify FAILED: 0x%lX\n", ret);
  }
}

static void bootload_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  // [롤백 가드] 설치 직전 probation 무장 → 새 펌웨어가 다음 부팅에서 검증받음.
  //   (verifyImage는 이미 통과한 상태 = verify-before-install 방어)
  fw_guard_arm_pending();
  app_log_info("OTA: Rebooting to install new firmware...\n");
  bootloader_rebootAndInstall();
}

/**************************************************************************//**
 * [H-1] OTA PREPARE 메시지 처리 — erase는 defer (tick에서 실행)
 *
 * 원래 코드에서 bootloader_eraseStorageSlot(0)를 이 콜백에서 직접 호출하면
 * 수백ms 블로킹 → 스택 타이머 만료 / 메시지 누락 위험.
 * 플래그와 파라미터만 저장하고 실제 erase는 emberAfTickCallback에서 처리.
 *****************************************************************************/
static void handle_ota_prepare_msg(EmberIncomingMessage *message)
{
  if (message->length < 6) {
    app_log_error("OTA prepare msg too short: %d\n", message->length);
    return;
  }

  uint8_t  tag  = message->payload[1];
  uint32_t size = (uint32_t)message->payload[2]
                | ((uint32_t)message->payload[3] << 8)
                | ((uint32_t)message->payload[4] << 16)
                | ((uint32_t)message->payload[5] << 24);

  app_log_info("OTA PREPARE received: tag=0x%02X, size=%lu. Deferring erase.\n",
               tag, size);

  pending_master_node = message->source;
  pending_ota_tag     = tag;
  pending_ota_size    = size;
  ota_prepare_done    = false;
  ota_erase_pending   = true;  // tick에서 처리
}

static void send_prepare_ack(EmberNodeId master_node)
{
  uint8_t msg[1] = { MSG_TYPE_OTA_PREPARE_ACK };
  EmberStatus status = emberMessageSend(master_node, CUSTOM_ENDPOINT, 0,
                                        sizeof(msg), msg, tx_options);
  if (status == EMBER_SUCCESS) {
    app_log_info("Prepare ACK sent to 0x%04X.\n", master_node);
  } else {
    app_log_error("Prepare ACK err: 0x%02X\n", status);
  }
}

#if defined(EMBER_AF_PLUGIN_MICRIUM_RTOS) && defined(EMBER_AF_PLUGIN_MICRIUM_RTOS_APP_TASK1)
void emberAfPluginMicriumRtosAppTask1InitCallback(void) { app_log_info("app task init\n"); }
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
