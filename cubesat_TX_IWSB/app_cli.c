/***************************************************************************//**
 * @file
 * @brief app_cli.c
 *
 * [변경 내역]
 *   - cli_ota_target(): "ota_target <device_id>" — OTA 타겟 device_id 선택
 *   - cli_slave_list(): "slave_list"              — 등록된 슬레이브 목록 출력
 *******************************************************************************
 * SPDX-License-Identifier: Zlib
 ******************************************************************************/
// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
#include <string.h>
#include PLATFORM_HEADER
#include "em_chip.h"
#include "em_cmu.h"
#include "stack/include/ember.h"
#include "sl_cli.h"
#include "app_log.h"
#include "sl_app_common.h"
#include "app_init.h"
#include "stack-info.h"
#include "mbedtls/build_info.h"
#include "app_process.h"
#include "iq_capture.h"   // [IQ] 지상 CLI 테스트용(I2C 없이 트리거/덤프)

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
#define ENABLED  "enabled"
#define DISABLED "disabled"
#define DATA_ENDPOINT           1
#define TX_TEST_ENDPOINT        2

// -----------------------------------------------------------------------------
//                                Global Variables
// -----------------------------------------------------------------------------
EmberKeyData security_key = { .contents = SL_SENSOR_SINK_SECURITY_KEY };
psa_key_id_t security_key_id = 0;

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
static int16_t tx_power = SL_SENSOR_SINK_TX_POWER;

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

/******************************************************************************
 * CLI - form command
 *****************************************************************************/
void cli_form(sl_cli_command_arg_t *arguments)
{
  EmberStatus status;
  EmberNetworkParameters parameters;
  uint16_t channel = sl_cli_get_argument_uint8(arguments, 0);
  uint16_t default_channel = emberGetDefaultChannel();

  if (channel < default_channel) {
    app_log_info("Channel %d is invalid, the first valid channel is %d!\n",
                 channel, default_channel);
    return;
  }

  MEMSET(&parameters, 0, sizeof(EmberNetworkParameters));
  parameters.radioTxPower = tx_power;
  parameters.radioChannel = sl_cli_get_argument_uint8(arguments, 0);

  if (sl_cli_get_argument_count(arguments) > 1) {
    parameters.panId = sl_cli_get_argument_uint16(arguments, 1);
  } else {
    parameters.panId = SL_SENSOR_SINK_PAN_ID;
  }

  status = emberFormNetwork(&parameters);
  app_log_info("form 0x%02X\n", status);
}

/******************************************************************************
 * CLI - permit join
 *****************************************************************************/
void cli_pjoin(sl_cli_command_arg_t *arguments)
{
  EmberStatus status;
  uint8_t duration = sl_cli_get_argument_uint8(arguments, 0);
  size_t length = 0;
  uint8_t *contents = NULL;

  if (sl_cli_get_argument_count(arguments) > 1) {
    contents = sl_cli_get_argument_hex(arguments, 1, &length);
    status = emberSetSelectiveJoinPayload(length, contents);
  } else {
    emberClearSelectiveJoinPayload();
  }

  status = emberPermitJoining(duration);
  if (status != EMBER_SUCCESS) {
    app_log_info("Permit join status: 0x%02X", status);
  }
}

/******************************************************************************
 * CLI - set TX power
 *****************************************************************************/
void cli_set_tx_power(sl_cli_command_arg_t *arguments)
{
  bool save_power = false;
  tx_power = sl_cli_get_argument_int16(arguments, 0);

  if (sl_cli_get_argument_count(arguments) > 1) {
    save_power = sl_cli_get_argument_int8(arguments, 1);
  }

  if (emberSetRadioPower(tx_power, save_power) == EMBER_SUCCESS) {
    app_log_info("TX power set: %d\n", (int16_t)emberGetRadioPower());
  } else {
    app_log_error("TX power set failed\n");
  }
}

/******************************************************************************
 * CLI - set TX options
 *****************************************************************************/
void cli_set_tx_options(sl_cli_command_arg_t *arguments)
{
  tx_options = sl_cli_get_argument_uint8(arguments, 0);
  app_log_info("TX options set: MAC acks %s, security %s, priority %s\n",
               ((tx_options & EMBER_OPTIONS_ACK_REQUESTED)  ? ENABLED : DISABLED),
               ((tx_options & EMBER_OPTIONS_SECURITY_ENABLED) ? ENABLED : DISABLED),
               ((tx_options & EMBER_OPTIONS_HIGH_PRIORITY)  ? ENABLED : DISABLED));
}

/******************************************************************************
 * CLI - remove child
 *****************************************************************************/
void cli_remove_child(sl_cli_command_arg_t *arguments)
{
  EmberStatus status;
  EmberMacAddress address;
  size_t hex_length = 0;
  uint8_t *child_id;

  address.mode = sl_cli_get_argument_uint8(arguments, 0);

  if (address.mode == EMBER_MAC_ADDRESS_MODE_SHORT) {
    address.addr.shortAddress = sl_cli_get_argument_uint8(arguments, 1);
  } else {
    child_id = sl_cli_get_argument_hex(arguments, 1, &hex_length);
    memcpy(&address.addr.longAddress, child_id, hex_length);
  }

  status = emberRemoveChild(&address);
  app_log_info("Child removal 0x%02X\n", status);
}

/******************************************************************************
 * CLI - info command
 *****************************************************************************/
void cli_info(sl_cli_command_arg_t *arguments)
{
  (void)arguments;

  uint8_t *eui64 = emberGetEui64();

  char *is_ack      = ((tx_options & EMBER_OPTIONS_ACK_REQUESTED)    ? ENABLED : DISABLED);
  char *is_security = ((tx_options & EMBER_OPTIONS_SECURITY_ENABLED) ? ENABLED : DISABLED);
  char *is_high_prio = ((tx_options & EMBER_OPTIONS_HIGH_PRIORITY)   ? ENABLED : DISABLED);

  app_log_info("Info:\n");
  app_log_info("         MCU Id: 0x%016llX\n", SYSTEM_GetUnique());
  app_log_info("  Network state: 0x%02X\n", emberNetworkState());
  app_log_info("      Node type: 0x%02X\n", emberGetNodeType());
  app_log_info("          eui64: >%x%x%x%x%x%x%x%x\n",
               eui64[7], eui64[6], eui64[5], eui64[4],
               eui64[3], eui64[2], eui64[1], eui64[0]);
  app_log_info("        Node id: 0x%04X\n", emberGetNodeId());
  app_log_info("   Node long id: 0x");
  for (uint8_t i = 0; i < EUI64_SIZE; i++) {
    app_log_info("%02X", emberGetEui64()[i]);
  }
  app_log_info("\n");
  app_log_info("         Pan id: 0x%04X\n", emberGetPanId());
  app_log_info("        Channel: %d\n", (uint16_t)emberGetRadioChannel());
  app_log_info("          Power: %d\n", (int16_t)emberGetRadioPower());
  app_log_info("     TX options: MAC acks %s, security %s, priority %s\n",
               is_ack, is_security, is_high_prio);
}

/******************************************************************************
 * CLI - leave command
 *****************************************************************************/
void cli_leave(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  emberResetNetworkState();
}

/******************************************************************************
 * CLI - data command
 *****************************************************************************/
void cli_data(sl_cli_command_arg_t *arguments)
{
  EmberStatus status;
  EmberNodeId destination = sl_cli_get_argument_uint16(arguments, 0);
  uint8_t *hex_value = 0;
  size_t hex_length = 0;
  hex_value = sl_cli_get_argument_hex(arguments, 1, &hex_length);

  status = emberMessageSend(destination,
                            DATA_ENDPOINT,
                            0,
                            hex_length,
                            hex_value,
                            tx_options);

  app_log_info("TX: Data to 0x%04X:{", destination);
  for (uint8_t i = 0; i < hex_length; i++) {
    app_log_info("%02X ", hex_value[i]);
  }
  app_log_info("}: status=0x%02X\n", status);
}

/******************************************************************************
 * CLI - set_channel command
 *****************************************************************************/
void cli_set_channel(sl_cli_command_arg_t *arguments)
{
  uint8_t channel = sl_cli_get_argument_uint8(arguments, 0);
  EmberStatus status = emberSetRadioChannel(channel);
  if (status == EMBER_SUCCESS) {
    app_log_info("Radio channel set, status=0x%02X\n", status);
  } else {
    app_log_error("Setting radio channel failed, status=0x%02X\n", status);
  }
}

/******************************************************************************
 * CLI - set_tx_option command
 *****************************************************************************/
void cli_set_tx_option(sl_cli_command_arg_t *arguments)
{
  tx_options = sl_cli_get_argument_uint8(arguments, 0);
  char *is_ack      = ((tx_options & EMBER_OPTIONS_ACK_REQUESTED)    ? ENABLED : DISABLED);
  char *is_security = ((tx_options & EMBER_OPTIONS_SECURITY_ENABLED) ? ENABLED : DISABLED);
  char *is_high_prio = ((tx_options & EMBER_OPTIONS_HIGH_PRIORITY)   ? ENABLED : DISABLED);
  app_log_info("TX options set: MAC acks %s, security %s, priority %s\n",
               is_ack, is_security, is_high_prio);
}

/******************************************************************************
 * CLI - reset command
 *****************************************************************************/
void cli_reset(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  NVIC_SystemReset();
}

/******************************************************************************
 * CLI - toggle_radio command
 *****************************************************************************/
void cli_toggle_radio(sl_cli_command_arg_t *arguments)
{
  bool radio_on = (sl_cli_get_argument_uint8(arguments, 0) > 0);
  EmberStatus status = emberSetRadioPowerMode(radio_on);
  if (status == EMBER_SUCCESS) {
    app_log_info("Radio is turned %s\n", (radio_on) ? "ON" : "OFF");
  } else {
    app_log_error("Radio toggle is failed, status=0x%02X\n", status);
  }
}

/******************************************************************************
 * CLI - start_energy_scan command
 *****************************************************************************/
void cli_start_energy_scan(sl_cli_command_arg_t *arguments)
{
  EmberStatus status;
  uint8_t channel    = sl_cli_get_argument_uint8(arguments, 0);
  uint8_t sample_num = sl_cli_get_argument_uint8(arguments, 1);
  status = emberStartEnergyScan(channel, sample_num);
  if (status == EMBER_SUCCESS) {
    app_log_info("Start energy scanning: channel %d, samples %d\n",
                 channel, sample_num);
  } else {
    app_log_error("Start energy scanning failed, status=0x%02X\n", status);
  }
}

/******************************************************************************
 * CLI - set_security_key command
 *****************************************************************************/
void cli_set_security_key(sl_cli_command_arg_t *arguments)
{
#ifdef SL_CATALOG_CONNECT_AES_SECURITY_PRESENT
  uint8_t *key_hex_value = 0;
  size_t key_hex_length = 0;
  key_hex_value = sl_cli_get_argument_hex(arguments, 0, &key_hex_length);
  if (key_hex_length != EMBER_ENCRYPTION_KEY_SIZE) {
    app_log_info("Security key length must be: %d bytes\n", EMBER_ENCRYPTION_KEY_SIZE);
    return;
  }
  set_security_key(key_hex_value, key_hex_length);
#else
  (void)arguments;
  app_log_info("Security plugin: CONNECT AES SECURITY is missing\n");
  app_log_info("Security key set failed 0x%02X\n", EMBER_ERR_FATAL);
#endif
}

void cli_unset_security_key(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
#ifdef SL_CATALOG_CONNECT_AES_SECURITY_PRESENT
  emberRemovePsaSecurityKey();
  app_log_info("Security key unset successful\n");
#endif
}

/******************************************************************************
 * CLI - sweep_start command
 *****************************************************************************/
void cli_sweep_start(sl_cli_command_arg_t *args)
{
  EmberNodeId target = sl_cli_get_argument_uint16(args, 0);
  send_sweep_start_msg(target);
}

/******************************************************************************
 * CLI - counter command
 *****************************************************************************/
void cli_counter(sl_cli_command_arg_t *arguments)
{
  uint8_t counter_type = sl_cli_get_argument_uint8(arguments, 0);
  uint32_t counter;
  EmberStatus status = emberGetCounter(counter_type, &counter);
  if (status == EMBER_SUCCESS) {
    app_log_info("Counter type=0x%02X: %ld\n", counter_type, counter);
  } else {
    app_log_error("Get counter failed, status=0x%02X\n", status);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  [신규] OTA 타겟 선택 CLI 명령
// ─────────────────────────────────────────────────────────────────────────────

/******************************************************************************
 * CLI - ota_target command
 *
 * 사용법: ota_target <device_id>
 *   device_id: 1~4 (NVM3에 저장된 RX의 사용자 정의 ID)
 *
 * 동작:
 *   slave_table에 해당 device_id가 등록되어 있으면 OTA 타겟으로 선택.
 *   BTN0을 누르면 해당 RX에만 OTA 진행.
 *
 * 예시:
 *   > slave_list          ← 먼저 등록된 슬레이브 확인
 *   > ota_target 2        ← device_id 2번을 타겟으로 선택
 *   > (BTN0 press)        ← OTA 시작
 *****************************************************************************/
void cli_ota_target(sl_cli_command_arg_t *arguments)
{
  uint8_t device_id = sl_cli_get_argument_uint8(arguments, 0);

  if (device_id < 1 || device_id > MAX_SLAVES) {
    app_log_error("ota_target: invalid device_id=%d (valid range: 1~%d)\n",
                  device_id, MAX_SLAVES);
    return;
  }

  // app_process.c의 set_ota_target_by_device_id() 호출
  set_ota_target_by_device_id(device_id);
}

/******************************************************************************
 * [TEST-ONLY ota_start] CLI - ota_start command (I2C 없이 OTA 시작, 추후 제거)
 *
 * 사용법: ota_start <1-4>
 * 사전조건:
 *   1) Simplicity Commander로 SLOT0(0x84000)에 GBL 적재
 *   2) 해당 device_id RX가 등록/온라인 (slave_list 로 확인)
 * 동작: SLOT0 GBL 크기 자동 스캔 → 타겟 RX로 배포 시작(상태머신 자동 진행).
 *****************************************************************************/
void cli_ota_start(sl_cli_command_arg_t *arguments)
{
  uint8_t device_id = sl_cli_get_argument_uint8(arguments, 0);
  ota_start_test(device_id);
}

/******************************************************************************
 * CLI - slave_list command
 *
 * 사용법: slave_list
 *
 * 동작:
 *   TX의 slave_table에 등록된 RX 목록 출력.
 *   현재 선택된 OTA 타겟(*) 표시 및 GBL 준비 상태도 함께 출력.
 *
 * 출력 예시:
 *   ╔══════════════════════════════════════════╗
 *   ║          Registered Slave List           ║
 *   ╠══════════════════════════════════════════╣
 *   ║   [0] device_id=1   node_id=0x0001      ║
 *   ║ * [1] device_id=2   node_id=0x0002      ║  ← 현재 선택된 타겟
 *   ╠══════════════════════════════════════════╣
 *   ║ OTA target : device_id=2                 ║
 *   ║ GBL state  : Ready (Ground mode)         ║
 *   ╚══════════════════════════════════════════╝
 *****************************************************************************/
void cli_slave_list(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  print_slave_list();
}

/******************************************************************************
 * CLI - poll_status command
 *   사용법: poll_status
 *   폴링 재시작 강제 실행 (디버깅/수동 확인용)
 *****************************************************************************/
void cli_poll_status(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  polling_restart();
  app_log_info("[POLL] Manual poll cycle started.\n");
}

/******************************************************************************
 * CLI - meas command : RSSI 측정 캠페인 1회 시작(주파수 호핑 스윕)
 *****************************************************************************/
void cli_meas_start(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  if (meas_campaign_active()) {
    app_log_info("[MEAS] already running.\n");
    return;
  }
  meas_campaign_start();
}

/******************************************************************************
 * CLI - meas_stat command : 저장된 RSSI 레코드 수 출력
 *****************************************************************************/
void cli_meas_stat(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  app_log_info("[MEAS] ready=%d active=%d records=%lu\n",
               (int)rssi_log_ready(), (int)meas_campaign_active(),
               (unsigned long)rssi_log_count());
}

/******************************************************************************
 * CLI - meas_clear command : RSSI 로그 비움(커서 리셋)
 *****************************************************************************/
void cli_meas_clear(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  rssi_log_clear();
  app_log_info("[MEAS] log cleared.\n");
}

/******************************************************************************
 * CLI - meas_auto command : 자동 주기 스윕 on/off
 *   사용법: meas_auto <0|1>   (인자 없으면 현재 상태만 출력)
 *****************************************************************************/
void cli_meas_auto(sl_cli_command_arg_t *arguments)
{
  uint8_t cnt = sl_cli_get_argument_count(arguments);
  if (cnt >= 1) {
    uint8_t en = sl_cli_get_argument_uint8(arguments, 0);
    meas_auto_set(en != 0);
  }
  app_log_info("[MEAS] auto = %d (gap=%us)\n",
               (int)meas_auto_get(), (unsigned)MEAS_AUTO_GAP_S);
}

/******************************************************************************
 * [IQ 지상테스트] CLI - iq_start : IQ 측정 창을 dur_s 초 동안 염 (OBC 0x05 대체)
 *   사용법: iq_start <dur_s>   (인자 없으면 기본 5초)
 *****************************************************************************/
void cli_iq_start(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  // ★ OBC 가 SPI/I2C 로 0x05 를 보냈을 때와 "완전히 동일한" 경로를 탄다.
  //   (eval 보드에서 OBC 없이 전 과정을 검증하기 위한 미러링)
  //   OBC 핸들러(handle_cmd_iq_start)와 같은 OTA 가드 → 같은 진입점 순서.
  obc_cmd_iq_start_mirror();
}

/******************************************************************************
 * [IWSB 지상테스트] CLI - iq_abort : 진행 중인 미션 중단
 *****************************************************************************/
void cli_iq_abort(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  iq_mission_abort();
}

/******************************************************************************
 * [IQ 지상테스트] CLI - iq_status : 큐에 쌓인 완성 레코드 수 출력
 *****************************************************************************/
void cli_iq_status(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  // OBC 가 리드백 프레임 헤더로 보게 될 값과 동일한 정보를 그대로 표시한다.
  app_log_info("[IWSB] mission=%u  sweeps=%u/%u  batch_ready=%u  queued=%u\n",
               (unsigned)iq_mission_is_active(),
               (unsigned)iq_mission_sweeps_done(),
               (unsigned)IQ_MISSION_SWEEPS,
               (unsigned)iq_mission_batch_ready(),
               (unsigned)iq_readback_count());
}

/******************************************************************************
 * [IWSB 지상테스트] CLI - iq_auto : 배치 자동 회수 on/off
 *
 *   실제 OBC 는 0x06 을 폴링해서 배치를 알아서 가져간다. CLI 로 시험할 때는
 *   사람이 iq_dump 를 대신 쳐야 하는데, 100 스윕이면 100번을 쳐야 하고
 *   30초 안에 못 치면 그 배치는 버려진다.
 *   이 모드를 켜면 tick 이 배치를 자동으로 회수하고 스윕당 한 줄만 요약 출력한다
 *   → 사람 개입 없이 100 스윕 미션 전체를 돌려볼 수 있다.
 *   ※ 원시 IQ 값이 필요하면 끄고 iq_dump 를 쓰거나, 스윕 수를 줄여서 볼 것.
 *****************************************************************************/
void cli_iq_auto(sl_cli_command_arg_t *arguments)
{
  if (sl_cli_get_argument_count(arguments) == 0) {
    app_log_info("[IWSB] auto-drain = %s\n", iq_auto_drain_get() ? "ON" : "OFF");
    return;
  }
  uint8_t on = sl_cli_get_argument_uint8(arguments, 0);
  iq_auto_drain_set(on != 0U);
  app_log_info("[IWSB] auto-drain = %s\n", on ? "ON (summary only)" : "OFF");
}

/******************************************************************************
 * [지상테스트] CLI - spi_stat : SPI 슬레이브 진단
 *   SPI 슬레이브는 마스터가 클럭을 넣기 전엔 아무 일도 안 하므로, 이 카운터로
 *   "배선 / 클럭 / 프레이밍 / 파싱" 중 어디까지 도달했는지를 좁혀 나간다.
 *****************************************************************************/
void cli_spi_stat(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  obc_spi_stats_t s;
  obc_spi_get_stats(&s);

  app_log_info("[SPI] pins: CS=%u CLK=%u MOSI=%u   (유휴 정상값: CS=1 CLK=0 MOSI=0)\n",
               s.cs_level, s.clk_level, s.mosi_level);
  app_log_info("[SPI] rx_bytes=%lu  cs_edges=%lu  frames=%lu\n",
               (unsigned long)s.bytes, (unsigned long)s.cs_edges,
               (unsigned long)s.frames);
  app_log_info("[SPI] last_cmd=0x%02X last_len=%u  first_rx=0x%04X\n",
               s.last_cmd, s.last_len, s.first_rx);
  app_log_info("[SPI] overrun=%lu  %s\n", (unsigned long)s.overrun,
               (s.overrun == 0U) ? "(정상)" : "★ 클럭이 너무 빠름 — 속도를 낮출 것");
  app_log_info("[SPI] staged=%u bytes, tx_idx=%u\n", s.staged, s.tx_idx);

  // 스테이징된 프레임 선두 — 마스터가 읽었을 때 받아야 할 바이트열.
  uint16_t n = 0;
  const uint8_t *buf = obc_readback_buffer(&n);
  if (buf != NULL && n >= 8U) {
    app_log_info("[SPI] MISO 선두 8B: %02X %02X %02X %02X %02X %02X %02X %02X"
                 "  (첫 바이트가 A5 여야 정상)\n",
                 buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
  }

  // 진단 해석 가이드 — 로그만 보고 판단할 수 있게 함께 출력.
  if (s.bytes == 0U && s.cs_edges == 0U) {
    app_log_info("[SPI] → 아무 활동 없음. 마스터가 클럭/CS 를 안 넣거나 배선 미연결.\n");
  } else if (s.bytes == 0U) {
    app_log_info("[SPI] → CS 는 오는데 클럭이 없음. SCLK 배선/LOC 확인.\n");
  } else if (s.cs_edges == 0U) {
    app_log_info("[SPI] → 바이트는 오는데 CS 엣지가 없음. CS 배선/극성 확인.\n");
  } else {
    app_log_info("[SPI] → 배선/프레이밍 정상. last_cmd 가 보낸 명령과 같은지 확인.\n");
  }
}

/******************************************************************************
 * [지상테스트] CLI - spi_clear : SPI 진단 카운터 초기화
 *****************************************************************************/
void cli_spi_clear(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  obc_spi_reset_stats();
  app_log_info("[SPI] stats cleared.\n");
}

/******************************************************************************
 * [지상테스트] CLI - spi_inject : 물리 SPI 없이 명령 경로만 검증
 *   사용: spi_inject 05      → OBC 가 0x05 를 보낸 것과 동일하게 처리
 *         spi_inject 06      → 리드백 스테이징까지 수행(이후 spi_stat 로 확인)
 *   ※ 배선/클럭/프레이밍은 검증하지 못한다 — 그건 실제 마스터가 있어야 한다.
 *****************************************************************************/
void cli_spi_inject(sl_cli_command_arg_t *arguments)
{
  if (sl_cli_get_argument_count(arguments) == 0) {
    app_log_error("usage: spi_inject <cmd_hex> [arg_hex]\n");
    return;
  }
  uint8_t frame[2];
  uint16_t len = 1;
  frame[0] = sl_cli_get_argument_uint8(arguments, 0);
  if (sl_cli_get_argument_count(arguments) >= 2) {
    frame[1] = sl_cli_get_argument_uint8(arguments, 1);
    len = 2;
  }
  app_log_info("[SPI] inject frame: %02X%s (len=%u)\n",
               frame[0], (len == 2) ? " .." : "", len);
  obc_spi_inject(frame, len);
}

/******************************************************************************
 * [IQ 지상테스트] CLI - iq_dump : 쌓인 IQ 전부를 UART 로 CSV 형태로 출력
 *   (OBC 0x06 I2C 리드백과 동일 데이터를 I2C 없이 확인하기 위한 디버그 경로)
 *   출력: tx_id,rx_id,channel,seq,sample_idx,I,Q  한 줄씩
 *****************************************************************************/
void cli_iq_dump(sl_cli_command_arg_t *arguments)
{
  (void)arguments;
  // ★ 스윕이 도는 중에는 절대 덤프하지 않는다.
  //   이 함수는 레코드당 64줄을 UART 로 뱉어 최대 십수 초를 블로킹한다. 그동안
  //   tick 이 멈추면 측정 상태머신도 멈추고, 송신 스트림이 슬롯을 넘겨 계속
  //   켜져 있게 된다 → Connect MAC 송신 큐가 가득 차서 이후 모든 전송이
  //   0x39(MAC_TRANSMIT_QUEUE_FULL)로 영구 실패한다(실제 발생한 장애).
  //   감시 타이머가 이제 최악은 막아 주지만, 애초에 유발하지 않는 것이 맞다.
  if (meas_campaign_active()) {
    app_log_error("[IQ] sweep in progress — dump blocked (would stall the radio).\n");
    app_log_info("[IQ] 스윕이 끝난 뒤에 다시 시도하거나, 'iq_auto 1' 로 자동 회수를 쓰세요.\n");
    return;
  }
  // ★ 전용 버퍼를 따로 두지 않는다(33KB 중복 방지). OBC 리드백과 같은 공용
  //   스테이징 버퍼를 그대로 재사용한다 — 내용/형식이 완전히 동일하므로
  //   CLI 덤프가 곧 "OBC 가 받게 될 바이트"를 그대로 보여주는 셈이다.
  // ★ 링에 아직 안 옮긴 레코드가 있을 때만 새로 만든다.
  //   스윕이 끝나면 tick 이 이미 obc_stage_iq_readback() 으로 링을 프레임 버퍼로
  //   전부 옮겨 둔다(링은 비어 있음). 그 상태에서 여기서 또 스테이징하면
  //   빈 링으로 프레임을 다시 만들어 n_records=0 으로 덮어써 버린다 —
  //   보려던 데이터를 보는 행위가 지워 버리는 셈이다.
  if (iq_readback_count() > 0U) {
    obc_stage_iq_readback();
  }
  uint16_t n = 0;
  const uint8_t *buf = obc_readback_buffer(&n);
  if (buf == NULL || n == 0U) {
    app_log_info("[IQ] nothing staged\n");
    return;
  }
  // 프레임 헤더 해석 (iq_capture.h 정의) — OBC 가 보게 될 상태 플래그와 동일.
  if (buf[0] != IQ_FRAME_MAGIC) {
    app_log_error("[IQ] bad magic 0x%02X\n", buf[0]);
    return;
  }
  uint8_t  nrec = buf[5];
  uint16_t off  = IQ_FRAME_HDR;
  app_log_info("[IWSB] mission=%u sweeps=%u/%u batch_ready=%u n_records=%u\n",
               buf[1], buf[2], buf[3], buf[4], (unsigned)nrec);
  // [중계 검증] 어느 노드의 데이터가 몇 건 들어왔는지 먼저 요약한다.
  //   격리 시험에서 "끊어 놓은 노드의 rx_id 가 오는가"를 6000 줄을 넘기지 않고
  //   바로 확인하기 위한 것.
  {
    uint16_t per_rx[5] = { 0, 0, 0, 0, 0 };
    uint16_t o = IQ_FRAME_HDR;
    for (uint8_t r = 0; r < nrec; r++) {
      uint8_t id = buf[o + 1];
      if (id < 5U) per_rx[id]++;
      o += IQ_REC_SIZE;
    }
    app_log_info("[IWSB] rx_id별 레코드: TX(0)=%u  n1=%u  n2=%u  n3=%u  n4=%u\n",
                 per_rx[0], per_rx[1], per_rx[2], per_rx[3], per_rx[4]);
  }

  app_log_info("tx_id,rx_id,channel,seq,sample_idx,I,Q\n");
  for (uint8_t r = 0; r < nrec; r++) {
    uint8_t tx_id = buf[off + 0], rx_id = buf[off + 1];
    uint8_t ch    = buf[off + 2], seq   = buf[off + 3];
    uint8_t nsamp = buf[off + 4];
    for (uint8_t k = 0; k < nsamp; k++) {
      const uint8_t *p = &buf[off + IQ_REC_HDR + (uint16_t)k * 4U];
      int16_t I = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
      int16_t Q = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
      app_log_info("%u,%u,%u,%u,%u,%d,%d\n",
                   tx_id, rx_id, ch, seq, k, I, Q);
    }
    off += IQ_REC_SIZE;
  }
  iq_batch_taken();   // UART 로 실제 출력했으므로 회수된 것으로 본다
}

// ─────────────────────────────────────────────────────────────────────────────
//  Security key 유틸리티 (기존 그대로)
// ─────────────────────────────────────────────────────────────────────────────
bool set_security_key(uint8_t *key, size_t key_length)
{
  bool success = false;
  EmberStatus em_status = EMBER_ERR_FATAL;
  psa_key_attributes_t key_attr;

  key_attr = psa_key_attributes_init();
  psa_status_t psa_status = psa_get_key_attributes(security_key_id, &key_attr);
  if (psa_status != PSA_ERROR_INVALID_HANDLE) {
    psa_destroy_key(security_key_id);
  }

  key_attr = psa_key_attributes_init();
  psa_set_key_type(&key_attr, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&key_attr, 128);
  psa_set_key_usage_flags(&key_attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&key_attr, PSA_ALG_ECB_NO_PADDING);
  psa_set_key_id(&key_attr, security_key_id);

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

  psa_status = psa_import_key(&key_attr, key, key_length, &security_key_id);
  if (psa_status == PSA_SUCCESS) {
    app_log_info("Security key import successful, key id: %lu\n", security_key_id);
  } else {
    app_log_info("Security Key import failed: %ld\n", psa_status);
  }

  em_status = emberSetPsaSecurityKey(security_key_id);
  if (em_status == EMBER_SUCCESS) {
    app_log_info("Security key set successful\n");
    success = true;
  } else {
    app_log_info("Security key set failed 0x%02X\n", em_status);
  }

  return success;
}
