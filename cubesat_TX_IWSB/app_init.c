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
#include "fw_guard.h"
#include "em_i2c.h"
#include "em_cmu.h"
#include "em_gpio.h"
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

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void     obc_i2c_slave_init(void);
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

  app_log_info("Master init complete.\n");
  app_log_info("OTA via AP_OBC I2C: erase(0x03) → stream(0x02) → START(0x04,target).\n");

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

