/***************************************************************************//**
 * @file
 * @brief app_process.c — Slave (Sensor / OTA Client) — Automated OTA
 *
 * [변경 내역]
 *   - TDMA 슬롯 기반 전송 타이밍 적용
 *     device_id 기반으로 슬롯 오프셋 계산 → RF 충돌 제거
 *     사이클: 10초, 슬롯: 2.5초 간격
 *     device_id=1: 0ms / device_id=2: 2500ms / device_id=3: 5000ms / device_id=4: 7500ms
 *******************************************************************************
 * SPDX-License-Identifier: Zlib
 ******************************************************************************/

// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
#define CUSTOM_ENDPOINT             0x02
#define JOIN_RETRY_INTERVAL_MS      5000

// ─── TDMA 설정 ───────────────────────────────────────────────────────────────
#define TDMA_CYCLE_MS               10000U                          // 전체 사이클 10초
#define TDMA_SLOT_MS   (TDMA_CYCLE_MS / 4U)

// device_id=1 → 0ms, 2 → 2500ms, 3 → 5000ms, 4 → 7500ms
#define TDMA_OFFSET_MS              ((uint32_t)(my_device_id - 1) * TDMA_SLOT_MS)

// FW 버전 리포트는 ID 알림 500ms 후 전송
#define FW_REPORT_EXTRA_MS          500U

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void handle_ota_prepare_msg(EmberIncomingMessage *message);
static void send_prepare_ack(EmberNodeId master_node);
static void send_fw_version_report(void);
static void send_id_announce(void);
static void try_rejoin(void);
static void bootload_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data);
static void verify_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data);
static void version_report_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data);
static void id_announce_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data);
static void id_heartbeat_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data);

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
static bool        ota_prepare_done  = false;
static uint8_t     ota_expected_tag  = 0;
static uint32_t    ota_expected_size = 0;
static uint32_t    join_retry_timer  = 0;
static bool        image_verified    = false;
static EmberNodeId master_node_id    = 0x0000;

static sl_sleeptimer_timer_handle_t bootload_timer;
static sl_sleeptimer_timer_handle_t verify_timer;
static sl_sleeptimer_timer_handle_t version_report_timer;
static sl_sleeptimer_timer_handle_t id_announce_timer;
static sl_sleeptimer_timer_handle_t id_heartbeat_timer;

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void emberAfIncomingMessageCallback(EmberIncomingMessage *message)
{
  if (message->endpoint == CUSTOM_ENDPOINT
      && message->length >= 1
      && message->payload[0] == MSG_TYPE_OTA_PREPARE) {
    handle_ota_prepare_msg(message);
    return;
  }

  if (message->endpoint == 13) return;

  app_log_info("RX from 0x%04X: ep=%d, len=%d\n",
               message->source, message->endpoint, message->length);
}

void emberAfMessageSentCallback(EmberStatus status, EmberOutgoingMessage *message)
{
  (void)message;
  if (status != EMBER_SUCCESS) {
    app_log_info("TX: 0x%02X\n", status);
  }
}

/**************************************************************************//**
 * Stack status callback
 *
 * TDMA 슬롯 타이밍:
 *   ID 알림    : TDMA_OFFSET_MS
 *   FW 리포트  : TDMA_OFFSET_MS + 500ms
 *   Heartbeat  : TDMA_CYCLE_MS + TDMA_OFFSET_MS (첫 사이클 후 반복)
 *
 *   device_id=1: 0ms    / 500ms   / 10000ms
 *   device_id=2: 2500ms / 3000ms  / 12500ms
 *   device_id=3: 5000ms / 5500ms  / 15000ms
 *   device_id=4: 7500ms / 8000ms  / 17500ms
 *****************************************************************************/
void emberAfStackStatusCallback(EmberStatus status)
{
  switch (status) {
    case EMBER_NETWORK_UP:
      app_log_info("Network UP. NodeID=0x%04X, device_id=%d, slot_offset=%lums\n",
                   emberGetNodeId(), my_device_id, TDMA_OFFSET_MS);
      network_joined   = true;
      join_in_progress = false;

      // ID 알림 — TDMA 슬롯 오프셋 적용
      sl_sleeptimer_start_timer_ms(&id_announce_timer,
                                   TDMA_OFFSET_MS,
                                   id_announce_timer_cb, NULL, 0, 0);

      // FW 버전 리포트 — ID 알림 500ms 후
      sl_sleeptimer_start_timer_ms(&version_report_timer,
                                   TDMA_OFFSET_MS + FW_REPORT_EXTRA_MS,
                                   version_report_timer_cb, NULL, 0, 0);

      // Heartbeat — 첫 사이클은 CYCLE + OFFSET 후 시작
      sl_sleeptimer_start_timer_ms(&id_heartbeat_timer,
                                   TDMA_CYCLE_MS + TDMA_OFFSET_MS,
                                   id_heartbeat_timer_cb, NULL, 0, 0);
      break;

    case EMBER_NETWORK_DOWN:
      app_log_info("Network DOWN.\n");
      network_joined   = false;
      join_in_progress = false;
      join_retry_timer = sl_sleeptimer_get_tick_count();
      break;

    default:
      app_log_info("Stack status: 0x%02X\n", status);
      if (join_in_progress) {
        join_in_progress = false;
        join_retry_timer = sl_sleeptimer_get_tick_count();
      }
      break;
  }
}

void emberAfTickCallback(void)
{
  if (!network_joined && !join_in_progress) {
    uint32_t elapsed = sl_sleeptimer_tick_to_ms(
                           sl_sleeptimer_get_tick_count() - join_retry_timer);
    if (elapsed > JOIN_RETRY_INTERVAL_MS) {
      app_log_info("Retrying join...\n");
      try_rejoin();
      join_retry_timer = sl_sleeptimer_get_tick_count();
    }
  }
}

bool emberAfPluginOtaUnicastBootloaderClientNewIncomingImageCallback(
  EmberNodeId serverId, uint8_t imageTag, uint32_t imageSize, uint32_t *startIndex)
{
  app_log_info("OTA: New image from 0x%04X, tag=0x%02X, size=%lu\n",
               serverId, imageTag, imageSize);
  ota_expected_tag  = imageTag;
  ota_expected_size = imageSize;
  *startIndex = 0;
  return true;
}

void emberAfPluginOtaUnicastBootloaderClientIncomingImageSegmentCallback(
  EmberNodeId serverId, uint32_t startIndex, uint32_t endIndex,
  uint8_t imageTag, uint8_t *imageSegment)
{
  (void)serverId;
  (void)imageTag;
  uint32_t len = endIndex - startIndex + 1;
  int32_t ret  = bootloader_writeStorage(0, startIndex, imageSegment, len);
  if (ret != BOOTLOADER_OK) {
    app_log_error("OTA: writeStorage FAIL at %lu: 0x%lX\n", startIndex, ret);
  }
}

void emberAfPluginOtaUnicastBootloaderClientImageDownloadCompleteCallback(
  EmberAfOtaUnicastBootloaderStatus status, uint8_t imageTag, uint32_t imageSize)
{
  if (status == EMBER_OTA_UNICAST_BOOTLOADER_STATUS_SUCCESS) {
    app_log_info("OTA: Download COMPLETE. tag=0x%02X, size=%lu\n",
                 imageTag, imageSize);
    image_verified = false;
    sl_sleeptimer_start_timer_ms(&verify_timer, 100,
                                 verify_timer_cb, NULL, 0, 0);
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

static void send_id_announce(void)
{
  uint8_t msg[2] = { MSG_TYPE_ID_ANNOUNCE, my_device_id };
  EmberStatus status = emberMessageSend(EMBER_COORDINATOR_ADDRESS,
                                        CUSTOM_ENDPOINT, 0,
                                        sizeof(msg), msg, tx_options);
  if (status == EMBER_SUCCESS) {
    app_log_info("ID announce sent: device_id=%d\n", my_device_id);
  } else {
    app_log_error("ID announce FAILED: 0x%02X\n", status);
  }
}

static void id_announce_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  send_id_announce();
}

/**************************************************************************//**
 * Heartbeat 콜백 — TDMA_CYCLE_MS 주기로 반복
 * 오프셋은 최초 1회만 적용되고 이후는 정확히 사이클 주기로 반복
 *****************************************************************************/
static void id_heartbeat_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  if (network_joined) {
    send_id_announce();
  }
  // 정확히 CYCLE 주기로 재시작 (오프셋 없이)
  sl_sleeptimer_start_timer_ms(&id_heartbeat_timer,
                               TDMA_CYCLE_MS,
                               id_heartbeat_timer_cb, NULL, 0, 0);
}

static void send_fw_version_report(void)
{
  static uint8_t retry_count = 0;

  if (retry_count >= 5) {
    app_log_error("Cannot reach TX. RX auto-resetting...\n");
    retry_count = 0;
    NVIC_SystemReset();
    return;
  }

  uint8_t msg[2] = { MSG_TYPE_FW_VERSION_REPORT, FW_VERSION_MAJOR };
  EmberStatus status = emberMessageSend(EMBER_COORDINATOR_ADDRESS,
                                        CUSTOM_ENDPOINT, 0,
                                        sizeof(msg), msg,
                                        EMBER_OPTIONS_ACK_REQUESTED);
  if (status == EMBER_SUCCESS) {
    app_log_info("FW version report sent: version=0x%02X\n", FW_VERSION_MAJOR);
    retry_count = 0;
  } else {
    app_log_error("FW version report FAILED: 0x%02X (%d/5)\n",
                  status, retry_count + 1);
    retry_count++;
    sl_sleeptimer_start_timer_ms(&version_report_timer, 3000,
                                 version_report_timer_cb, NULL, 0, 0);
  }
}

static void version_report_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  send_fw_version_report();
}

static void verify_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  int32_t ret = bootloader_verifyImage(0, NULL);
  if (ret == BOOTLOADER_OK) {
    bootloader_setImageToBootload(0);
    image_verified = true;
    app_log_info("OTA: Image verified OK. Ready for bootload.\n");
  } else {
    image_verified = false;
    app_log_error("OTA: Image verify FAILED: 0x%lX\n", ret);
  }
}

static void bootload_timer_cb(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle; (void)data;
  app_log_info("OTA: Rebooting to install new firmware...\n");
  bootloader_rebootAndInstall();
}

static void handle_ota_prepare_msg(EmberIncomingMessage *message)
{
  if (message->length < 6) {
    app_log_error("OTA prepare msg too short: %d\n", message->length);
    return;
  }

  master_node_id = message->source;
  uint8_t  tag  = message->payload[1];
  uint32_t size = (uint32_t)message->payload[2]
                | ((uint32_t)message->payload[3] << 8)
                | ((uint32_t)message->payload[4] << 16)
                | ((uint32_t)message->payload[5] << 24);

  app_log_info("OTA PREPARE from 0x%04X: tag=0x%02X, size=%lu\n",
               master_node_id, tag, size);

  ota_expected_tag  = tag;
  ota_expected_size = size;

  app_log_info("Erasing slot 0...\n");
  int32_t ret = bootloader_eraseStorageSlot(0);
  if (ret != BOOTLOADER_OK) {
    app_log_error("Slot erase FAILED: 0x%lX\n", ret);
    return;
  }
  app_log_info("Slot 0 erased. Ready for OTA.\n");
  ota_prepare_done = true;
  send_prepare_ack(master_node_id);
}

static void send_prepare_ack(EmberNodeId master_node)
{
  uint8_t msg[1] = { MSG_TYPE_OTA_PREPARE_ACK };
  EmberStatus status = emberMessageSend(master_node, CUSTOM_ENDPOINT, 0,
                                        sizeof(msg), msg, tx_options);
  if (status == EMBER_SUCCESS) {
    app_log_info("Prepare ACK sent to 0x%04X.\n", master_node);
  } else {
    app_log_error("Prepare ACK FAILED: 0x%02X\n", status);
  }
}

static void try_rejoin(void)
{
  EmberNetworkParameters params;
  memset(&params, 0, sizeof(params));
  params.radioChannel = (uint8_t)emberGetDefaultChannel();
  params.radioTxPower = 0;
  params.panId        = 0xFFFF;

  EmberStatus status = emberJoinNetwork(EMBER_STAR_END_DEVICE, &params);
  if (status == EMBER_SUCCESS) {
    join_in_progress = true;
    app_log_info("Rejoin initiated.\n");
  } else {
    app_log_error("Rejoin FAILED: 0x%02X\n", status);
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
