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
