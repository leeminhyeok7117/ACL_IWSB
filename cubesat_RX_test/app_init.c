/***************************************************************************//**
 * @file
 * @brief app_init.c — Slave (Sensor / OTA Client) — Automated OTA
 *******************************************************************************
 * SPDX-License-Identifier: Zlib
 ******************************************************************************/

// -----------------------------------------------------------------------------
//                                   Includes
// -----------------------------------------------------------------------------
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
#include <string.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
#define PSA_AES_KEY_ID          1
#define SLAVE_NETWORK_CHANNEL   0
#define SLAVE_TX_POWER          0

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void bootloader_storage_init(void);
static void auto_join_network(void);

// -----------------------------------------------------------------------------
//                                Global Variables
// -----------------------------------------------------------------------------
extern EmberKeyData security_key;
extern psa_key_id_t security_key_id;

volatile bool network_joined   = false;
volatile bool join_in_progress = false;

// 컴파일 타임 상수로 확정 — app_process.h의 MY_DEVICE_ID 사용
// app_process.c에서 extern으로 참조
uint8_t my_device_id = MY_DEVICE_ID;

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------
void emberAfInitCallback(void)
{
  EmberStatus em_status = EMBER_ERR_FATAL;

  psa_crypto_init();
  app_log_info("\n=== Slave (Sensor / OTA Client) | device_id=%d ===\n",
               my_device_id);

  // ─── Security Key 설정 ───
  security_key_id = PSA_AES_KEY_ID;
  psa_key_attributes_t key_attr = psa_key_attributes_init();
  psa_status_t psa_status = psa_get_key_attributes(security_key_id, &key_attr);
  if (psa_status == PSA_ERROR_INVALID_HANDLE) {
    app_log_info("No PSA AES key found, creating one.\n");
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
    if (psa_status == PSA_SUCCESS) {
      app_log_info("Security key import OK, id: %lu\n", security_key_id);
    } else {
        app_log_info("Security key import FAIL: %ld\n", psa_status);
    }
  } else {
    app_log_info("PSA AES key exists, reusing.\n");
  }

  em_status = emberSetPsaSecurityKey(security_key_id);
  (void)em_status;

  // ─── Network 자동 접속 ───
  sl_sleeptimer_delay_millisecond(3000);
  emberResetNetworkState();
  network_joined = false;
  app_log_info("Network state cleared. Starting fresh join...\n");
  auto_join_network();

  // ─── Bootloader Storage 초기화 ───
  bootloader_storage_init();

  app_log_info("Slave init complete.\n");
}

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------
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
  if (ret == BOOTLOADER_OK) {
    app_log_info("Storage slot 0: addr=0x%08lX, len=%lu bytes\n",
                 slot_info.address, slot_info.length);
  } else {
    app_log_error("Failed to get slot 0 info: 0x%lX\n", ret);
  }
}

static void auto_join_network(void)
{
  EmberNetworkParameters params;
  memset(&params, 0, sizeof(params));
  params.radioChannel = SLAVE_NETWORK_CHANNEL;
  params.radioTxPower = SLAVE_TX_POWER;
  params.panId        = 0xFFFF;

  EmberStatus status = emberJoinNetwork(EMBER_STAR_END_DEVICE, &params);
  if (status == EMBER_SUCCESS) {
    join_in_progress = true;
    app_log_info("Join initiated on ch=%d...\n", params.radioChannel);
  } else {
    app_log_error("emberJoinNetwork FAILED: 0x%02X\n", status);
  }
}
