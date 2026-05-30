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
#include <stdlib.h>
#include <string.h>
#include "sl_sleeptimer.h"
#include "sl_simple_button_instances.h"
#include "btl_interface.h"
#include "btl_interface_storage.h"
#include "ota-unicast-bootloader-server.h"
#include "nvm3_default.h"

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
// ─── AP_OBC I2C 명령 코드 (호스트 sdrc_I2C.c 와 동일) ────────────────────────
#define OBC_CMD_DATA          0x01   // 데이터/상태 (1바이트)
#define OBC_CMD_FW_UPDATE     0x02   // FW 이미지 스트리밍 패킷
#define OBC_CMD_ERASE         0x03   // 스토리지 슬롯 erase
#define OBC_CMD_START         0x04   // OTA 트리거 [cc][target] target:0=TX자체,1~4=RX
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

#define DEVICE_ID_NONE  0xFFU

#define POLL_RESPONSE_TIMEOUT_MS  1500U
#define POLL_CYCLE_INTERVAL_MS    5000U

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
//                          Public Function Definitions
// -----------------------------------------------------------------------------

/**************************************************************************//**
 * BTN0 — OTA 시작
 *****************************************************************************/
void sl_button_on_change(const sl_button_t *handle)
{
  if (handle != &sl_button_btn0
      || sl_button_get_state(handle) != SL_SIMPLE_BUTTON_PRESSED) {
    return;
  }

  if (ota_state != OTA_FW_READY_MANUAL && ota_state != OTA_FW_READY) {
    app_log_info("BTN0: No GBL or OTA in progress (state=%d)\n", ota_state);
    return;
  }

  if (ota_target_device_id == DEVICE_ID_NONE) {
    app_log_info("BTN0: OTA target not set. Use: ota_target <1-%d>\n", MAX_SLAVES);
    return;
  }

  EmberNodeId target_node = get_node_id_by_device_id(ota_target_device_id);
  if (target_node == EMBER_NULL_NODE_ID) {
    app_log_info("BTN0: device_id=%d not registered (node_id unknown).\n",
                 ota_target_device_id);
    app_log_info("  Waiting for slave to rejoin and announce. Try again.\n");
    return;
  }

  ota_target_node = target_node;
  app_log_info("BTN0: OTA → device_id=%d, node=0x%04X\n",
               ota_target_device_id, ota_target_node);
  start_ota_distribution();
}

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
    app_log_info("[POLL] RX id=%d (node=0x%04X) online. fw=0x%02X\n",
                 dev_id, message->source, fw_ver);
    poll_waiting = false;   // 응답 수신 → 대기 해제
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

  app_log_info(" Temp: %s%d.%03dC Hum: %d.%03d%%\n",
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

/**************************************************************************//**
 * Tick callback
 *****************************************************************************/
void emberAfTickCallback(void)
{
  // ─── [C-2] 첫 tick에서 NVM3 slave_table 로드 ─────────────────────────────
  if (!slave_table_loaded) {
    slave_table_loaded = true;
    load_slave_table_nvm3();
  }

  if (emberStackIsUp()) {
    sl_led_turn_on(&sl_led_led0);
  } else {
    sl_led_turn_off(&sl_led_led0);
  }

  // ─── [M-3] volatile 버퍼 → 로컬 복사 후 OBC 명령 처리 ──────────────────
  if (obc_cmd_ready) {
    obc_cmd_ready = false;
    process_obc_command();
  }

  // ─── 폴링: OTA 진행 중이 아닐 때만 실행 ──────────────────────────────────
  bool ota_busy = (ota_state != OTA_IDLE
                   && ota_state != OTA_FW_READY_MANUAL
                   && ota_state != OTA_FW_READY);
  if (poll_running && !ota_busy) {
    poll_tick();
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
    app_log_info("[POLL] No pollable slaves (node_id unknown). Retry in %lums.\n",
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
 * [M-3] obc_rx_buffer(volatile) → 로컬 배열로 복사 후 파싱
 *****************************************************************************/
static void process_obc_command(void)
{
  uint8_t  local_buf[OBC_LOCAL_BUF_SIZE];
  uint16_t local_len = obc_rx_len;
  if (local_len > sizeof(local_buf)) local_len = sizeof(local_buf);
  memcpy(local_buf, (const uint8_t *)obc_rx_buffer, local_len);

  if (local_len < 1) {
    return;   // 빈 트랜잭션 무시
  }

  uint8_t cmd = local_buf[0];   // 명령별 길이 검증은 각 핸들러에서 수행

  switch (cmd) {
    case OBC_CMD_DUMMY:
      // 호스트가 스트리밍 직전 보내는 10바이트 dummy write → 무시
      break;
    case OBC_CMD_DATA:      handle_cmd_data(local_buf, local_len);      break;
    case OBC_CMD_FW_UPDATE: handle_cmd_fw_packet(local_buf, local_len); break;
    case OBC_CMD_ERASE:     handle_cmd_erase(local_buf, local_len);     break;
    case OBC_CMD_START:     handle_cmd_start(local_buf, local_len);     break;
    default:
      app_log_error("OBC: Unknown CMD 0x%02X (len=%d)\n", cmd, local_len);
      break;
  }
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
    app_log_info("Self-update verified. Rebooting to install...\n");
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

    case OTA_DISTRIBUTING:
      break;

    case OTA_REQUEST_BOOTLOAD: {
      if (!bootload_req_pending) break;
      bootload_req_pending = false;

      app_log_info("OTA: Requesting bootload (delay=%dms)...\n",
                   OTA_BOOTLOAD_DELAY_MS);
      EmberAfOtaUnicastBootloaderStatus ret =
        emberAfPluginUnicastBootloaderServerInitiateRequestTargetBootload(
          OTA_BOOTLOAD_DELAY_MS, ota_image_tag, ota_target_node);

      if (ret == EMBER_OTA_UNICAST_BOOTLOADER_STATUS_SUCCESS) {
        ota_state = OTA_WAITING_BOOTLOAD;
      } else {
        app_log_error("Bootload request FAILED: 0x%02X\n", ret);
        ota_state = OTA_ERROR;
      }
      break;
    }

    case OTA_WAITING_BOOTLOAD:
      break;

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
    app_log_info("[POLL] Cycle done (%u/%u slaves). Next in %lums.\n",
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
    app_log_info("[POLL] No pollable slaves yet. Retry in %lums.\n",
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
