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
#include "app_framework_common.h"
#include "psa/crypto.h"
#include "mbedtls/build_info.h"
#include "btl_interface.h"
#include "btl_interface_storage.h"
#include "em_i2c.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include <string.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
#define PSA_AES_KEY_ID            1

#define MASTER_NETWORK_PAN_ID     0xFFFF
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

#define GBL_TAG_END               0xFC0404FC

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void     obc_i2c_slave_init(void);
static void     bootloader_storage_init(void);
static uint32_t detect_gbl_size(BootloaderStorageSlot_t *slot);
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

uint32_t   gbl_image_size    = 0;
uint32_t   gbl_write_offset  = 0;
uint8_t    ota_image_tag     = 0xAA;

EmberNodeId ota_target_node  = EMBER_NULL_NODE_ID;

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void emberAfInitCallback(void)
{
  EmberStatus em_status = EMBER_ERR_FATAL;

  psa_crypto_init();
  app_log_info("\n=== Master (Sink / OTA Server) — Automated OTA ===\n");

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
    // NVM3에서 복원 중 — NETWORK_UP 콜백에서 permit-join 재적용
    app_log_info("Resuming saved network. NodeID=0x%04X (permit-join on NETWORK_UP)\n",
                 emberGetNodeId());
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

  app_log_info("Master init complete.\n");
  app_log_info("Use CLI: slave_list / ota_target <1-4> / BTN0 to start OTA.\n");

#if defined(EMBER_AF_PLUGIN_BLE)
  bleConnectionInfoTableInit();
#endif
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

static void form_network(void)
{
  EmberNetworkParameters params;
  memset(&params, 0, sizeof(params));
  params.radioChannel = (uint8_t)emberGetDefaultChannel();
  params.panId        = MASTER_NETWORK_PAN_ID;
  params.radioTxPower = MASTER_TX_POWER;

  EmberStatus st = emberFormNetwork(&params);
  if (st == EMBER_SUCCESS) {
    app_log_info("Network FORMED on ch=%d, PAN=0x%04X\n",
                 params.radioChannel, params.panId);
    emberPermitJoining(0xFF);
  } else {
    app_log_error("emberFormNetwork FAILED: 0x%02X\n", st);
  }
}

static uint32_t detect_gbl_size(BootloaderStorageSlot_t *slot)
{
  uint32_t offset = 0;
  uint8_t  buf[8];

  while (offset + 8 <= slot->length) {
    if (bootloader_readStorage(0, offset, buf, 8) != BOOTLOADER_OK) {
      app_log_error("GBL scan: read failed at offset %lu\n", offset);
      return 0;
    }

    uint32_t tag = (uint32_t)buf[0]
                 | ((uint32_t)buf[1] << 8)
                 | ((uint32_t)buf[2] << 16)
                 | ((uint32_t)buf[3] << 24);

    uint32_t length = (uint32_t)buf[4]
                    | ((uint32_t)buf[5] << 8)
                    | ((uint32_t)buf[6] << 16)
                    | ((uint32_t)buf[7] << 24);

    if (length > slot->length) {
      app_log_error("GBL scan: invalid length %lu at offset %lu\n", length, offset);
      return 0;
    }

    offset += 8 + length;

    if (tag == GBL_TAG_END) {
      app_log_info("GBL END tag found. Actual size: %lu bytes\n", offset);
      return offset;
    }
  }

  app_log_error("GBL scan: END tag not found\n");
  return 0;
}

static void bootloader_storage_init(void)
{
  int32_t ret = bootloader_init();
  if (ret != BOOTLOADER_OK) {
    app_log_error("bootloader_init() FAILED: 0x%lX\n", ret);
    return;
  }
  app_log_info("Bootloader interface initialized.\n");

  BootloaderStorageSlot_t slot_info;
  ret = bootloader_getStorageSlotInfo(0, &slot_info);
  if (ret != BOOTLOADER_OK) {
    app_log_error("getStorageSlotInfo FAILED: 0x%lX\n", ret);
    return;
  }
  app_log_info("Storage slot 0: addr=0x%08lX, len=%lu bytes\n",
               slot_info.address, slot_info.length);

  ret = bootloader_verifyImage(0, NULL);
  if (ret == BOOTLOADER_OK) {
    // 지상 모드: J-Link로 GBL이 적재돼 있음
    uint32_t actual_size = detect_gbl_size(&slot_info);
    if (actual_size == 0) {
      app_log_error("GBL size detection FAILED. Abort ground mode.\n");
      ota_state = OTA_IDLE;
      return;
    }
    gbl_image_size = actual_size;
    ota_image_tag  = 0xAA;
    ota_state      = OTA_FW_READY_MANUAL;

    app_log_info("=== Ground Mode ===\n");
    app_log_info("GBL detected. size=%lu bytes\n", gbl_image_size);
    app_log_info("Use CLI: slave_list → ota_target <1-4> → BTN0\n");
  } else {
    // 우주 모드: AP_OBC I2C CMD 0x01로 GBL 전달
    app_log_info("=== Space Mode ===\n");
    app_log_info("No valid GBL in slot. Waiting for AP_OBC I2C commands.\n");
    ota_state = OTA_IDLE;
  }
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
                I2C_IEN_ARBLOST);
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
    obc_rx_len = 0;
    return;
  }

  // ─── 주소 프레임 수신 ───────────────────────────────────────────────────
  if (flags & I2C_IF_ADDR) {
    (void)OBC_I2C_PERIPHERAL->RXDATA;   // 주소 바이트 소비
    obc_rx_len = 0;
    I2C_IntClear(OBC_I2C_PERIPHERAL, I2C_IFC_ADDR);
    OBC_I2C_PERIPHERAL->CMD = I2C_CMD_ACK;
  }

  // ─── 수신 가능한 바이트 전부 드레인 ─────────────────────────────────────
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
    // 마지막 바이트가 RXDATA로 시프트되어 들어올 시간 확보
    for (volatile int i = 0; i < 100; i++) {
      if (OBC_I2C_PERIPHERAL->STATUS & I2C_STATUS_RXDATAV) break;
    }
    // 남은 바이트 완전 드레인
    while (OBC_I2C_PERIPHERAL->STATUS & I2C_STATUS_RXDATAV) {
      if (obc_rx_len < sizeof(obc_rx_buffer)) {
        obc_rx_buffer[obc_rx_len++] = OBC_I2C_PERIPHERAL->RXDATA;
      } else {
        (void)OBC_I2C_PERIPHERAL->RXDATA;
      }
    }
    I2C_IntClear(OBC_I2C_PERIPHERAL, I2C_IFC_SSTOP);
    if (obc_rx_len > 0) {
      obc_cmd_ready = true;
    }
  }

  // 처리하지 않은 잔여 플래그 정리
  I2C_IntClear(OBC_I2C_PERIPHERAL, flags & ~(I2C_IFC_ADDR | I2C_IFC_SSTOP));
}
