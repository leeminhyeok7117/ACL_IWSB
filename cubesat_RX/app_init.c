/***************************************************************************//**
 * @file
 * @brief app_init.c — Slave (Sensor / OTA Client) — Stable Auto-Join OTA
 *
 * [Device ID]
 *   NVM3_KEY_DEVICE_ID(0x0001) 에서 읽음. 미프로비저닝 시 경고 후 조인 보류.
 *   최초 J-Link 플래시 후 UART 터미널 "set_id <1-4>" 로 1회 프로비저닝.
 *   NVM3 영역은 OTA 후에도 유지됨.
 *
 * [Network Init]
 *   EMBER_SUCCESS  → NVM3 복원 진행 (NETWORK_UP 콜백으로 완료 확인)
 *   EMBER_NOT_JOINED → 최초 1회 Join
 *   기타 에러     → Join 폴백 시도
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
#include "nvm3_default.h"
#include "fw_guard.h"
#include <string.h>
#include <stdbool.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
#define PSA_AES_KEY_ID          1
#define SLAVE_TX_POWER          0

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void     bootloader_storage_init(void);
static void     resume_or_join_network(void);
static uint8_t  load_device_id_from_nvm3(void);

void start_initial_join(void);   // app_process.c 에서 정의

// -----------------------------------------------------------------------------
//                                Global Variables
// -----------------------------------------------------------------------------
extern EmberKeyData security_key;
extern psa_key_id_t security_key_id;

volatile bool network_joined   = false;
volatile bool join_in_progress = false;

uint8_t my_device_id = 0xFF;   // 0xFF = 미프로비저닝. NVM3에서 덮어씀.

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------
void emberAfInitCallback(void)
{
  EmberStatus em_status = EMBER_ERR_FATAL;

  psa_crypto_init();

  // ─── [롤백 가드] 최우선: 부트로더 init 후 즉시 probation/롤백 판단 ─────────
  //   pending_commit && boot_attempts>=임계 이면 여기서 golden 복원 후 재부팅(복귀 안 함).
  //   네트워크 조인보다 먼저 수행 — 롤백할 거면 조인은 의미 없음.
  {
    int32_t btl = bootloader_init();
    if (btl != BOOTLOADER_OK) {
      app_log_error("bootloader_init FAILED: 0x%lX (rollback guard limited)\n", btl);
    }
    fw_guard_init();   // 롤백 트리거 시 복귀하지 않음
  }

  // ─── Device ID 로드 ───────────────────────────────────────────────────────
  my_device_id = load_device_id_from_nvm3();
  if (my_device_id == 0xFF) {
    app_log_error("=== DEVICE ID NOT PROVISIONED! ===\n");
    app_log_error("  Run: set_id <1-4>  then reset.\n");
    // 미프로비저닝 상태에서도 부트로더·스택 초기화는 진행하되
    // ID_ANNOUNCE 는 my_device_id 검사에서 막힘.
  }
  app_log_info("\n=== Slave (OTA Client) | device_id=%d ===\n", my_device_id);

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

  // ─── 네트워크 복원/가입 ─────────────────────────────────────────────────
  resume_or_join_network();

  // ─── Bootloader Storage 초기화 ──────────────────────────────────────────
  bootloader_storage_init();

  app_log_info("Slave init complete.\n");
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

/**************************************************************************//**
 * Device ID 결정 (우선순위 — fallback 권위 방식)
 *
 * 1순위: MY_DEVICE_ID_FALLBACK 가 1~4 (보드별 빌드)
 *         → "권위적": 이 값을 강제 사용하고, NVM3 값과 다르면 NVM3를 덮어씀.
 *         → 첫 OTA 실수 시에도 올바른 fallback 으로 재OTA 하면 교정됨.
 *         → 0xFF (단일 바이너리) 이면 이 경로 비활성.
 * 2순위: NVM3_KEY_DEVICE_ID — 단일 바이너리(0xFF) 운용 시 각 보드의 저장된 ID.
 * 3순위: g_eui64_map[] — NVM3 손실 시 EUI-64로 자동 복구(채워둔 경우).
 * 실패:  0xFF — ID_ANNOUNCE 차단(잘못된 ID 강제 안 함).
 *
 * [운용]
 *   첫 OTA  : 보드별 FALLBACK=1/2/3/4 빌드 → 각 보드 ID 확정·NVM3 저장.
 *   이후    : FALLBACK=0xFF 단일 빌드 → 각 보드가 NVM3 값을 읽음.
 *****************************************************************************/
static uint8_t load_device_id_from_nvm3(void)
{
  // 현재 NVM3 값 읽기(여러 단계에서 공용)
  uint32_t stored = 0;
  Ecode_t  rd = nvm3_readData(nvm3_defaultHandle, NVM3_KEY_DEVICE_ID,
                              &stored, sizeof(stored));
  bool nvm3_valid = (rd == ECODE_NVM3_OK && stored >= 1 && stored <= 4);

  // ─── 1순위: 컴파일 타임 FALLBACK (1~4 면 권위적) ─────────────────────────
#if defined(MY_DEVICE_ID_FALLBACK) \
    && (MY_DEVICE_ID_FALLBACK >= 1) && (MY_DEVICE_ID_FALLBACK <= 4)
  {
    uint8_t  id = (uint8_t)MY_DEVICE_ID_FALLBACK;
    // NVM3 와 다를 때만 기록(플래시 마모 방지) — 첫 OTA 실수도 여기서 교정.
    if (!nvm3_valid || (uint8_t)stored != id) {
      uint32_t nv = (uint32_t)id;
      Ecode_t  we = nvm3_writeData(nvm3_defaultHandle, NVM3_KEY_DEVICE_ID,
                                   &nv, sizeof(nv));
      app_log_info("Device ID forced by FALLBACK=%d → NVM3 %s.\n",
                   id, (we == ECODE_NVM3_OK) ? "updated" : "WRITE FAILED");
    } else {
      app_log_info("Device ID = %d (FALLBACK, matches NVM3).\n", id);
    }
    return id;
  }
#endif

  // ─── 2순위: NVM3 (단일 바이너리 0xFF 운용) ──────────────────────────────
  if (nvm3_valid) {
    app_log_info("Device ID = %lu (from NVM3).\n", stored);
    return (uint8_t)stored;
  }

  // ─── 3순위: EUI-64 맵 (NVM3 손실 시 자동 복구) ──────────────────────────
  uint8_t *eui64 = emberGetEui64();
  app_log_info("NVM3 device_id not set. Checking EUI-64 map...\n");
  app_log_info("My EUI-64: ");
  for (int i = 7; i >= 0; i--) app_log_info("%02X", eui64[i]);
  app_log_info("\n");

  for (uint8_t i = 0; i < g_eui64_map_count; i++) {
    bool is_zero = true;
    for (uint8_t j = 0; j < 8; j++) {
      if (g_eui64_map[i].eui64[j] != 0) { is_zero = false; break; }
    }
    if (is_zero) continue;

    if (memcmp(g_eui64_map[i].eui64, eui64, 8) == 0) {
      uint8_t dev_id = g_eui64_map[i].device_id;
      app_log_info("EUI-64 matched → device_id=%d. Saving to NVM3.\n", dev_id);
      uint32_t nvm3_val = (uint32_t)dev_id;
      nvm3_writeData(nvm3_defaultHandle, NVM3_KEY_DEVICE_ID,
                     &nvm3_val, sizeof(nvm3_val));
      return dev_id;
    }
  }

  // ─── 실패 ────────────────────────────────────────────────────────────────
  app_log_error("=== DEVICE ID UNPROVISIONED ===\n");
  app_log_error("Build per-board FALLBACK=1~4, or fill g_eui64_map[].\n");
  return 0xFF;
}

/**************************************************************************//**
 * 부팅 시 네트워크 복원 또는 최초 가입.
 *   EMBER_SUCCESS      → NVM3 복원 진행 중 (NETWORK_UP 콜백으로 완료)
 *   EMBER_NOT_JOINED   → 최초/리셋 후 Join 시도
 *   기타 에러          → Join 폴백
 *****************************************************************************/
static void resume_or_join_network(void)
{
  EmberStatus status = emberNetworkInit();
  app_log_info("emberNetworkInit: 0x%02X (netState=0x%02X)\n",
               status, emberNetworkState());

  if (status == EMBER_NOT_JOINED) {
    app_log_info("No saved network. One-time join...\n");
    network_joined   = false;
    join_in_progress = false;
    start_initial_join();
  } else if (status == EMBER_SUCCESS) {
    app_log_info("Resuming saved network from NVM3...\n");
    network_joined   = false;
    join_in_progress = true;   // 15s 워치독 무장
  } else {
    app_log_error("emberNetworkInit err: 0x%02X. Fallback join.\n", status);
    network_joined   = false;
    join_in_progress = false;
    start_initial_join();
  }
}

static void bootloader_storage_init(void)
{
  // bootloader_init() 는 emberAfInitCallback 초반(롤백 가드)에서 이미 수행됨.
  BootloaderStorageSlot_t slot_info;
  int32_t ret = bootloader_getStorageSlotInfo(0, &slot_info);
  if (ret == BOOTLOADER_OK) {
    app_log_info("Storage slot 0: addr=0x%08lX, len=%lu bytes\n",
                 slot_info.address, slot_info.length);
  } else {
    app_log_error("Failed to get slot 0 info: 0x%lX\n", ret);
  }
}
