/***************************************************************************//**
 * @file
 * @brief app_process.h — Slave (Sensor / OTA Client)
 *
 * [Device ID 결정 우선순위]
 *   1순위: NVM3_KEY_DEVICE_ID (UART CLI "set_id <1-4>" 로 한 번만 기입)
 *   2순위: g_eui64_map 테이블 (아래 EUI-64를 실제 값으로 채운 후 OTA 배포)
 *          → 위성 조립 완료 후 UART/J-Link 둘 다 불가한 경우 사용
 *
 * [EUI-64 확인 방법]
 *   - Simplicity Commander: commander device info
 *   - CLI: info 명령 → EUI64 필드
 ******************************************************************************/
#ifndef APP_PROCESS_H
#define APP_PROCESS_H

#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
//  NVM3 키
// ─────────────────────────────────────────────────────────────────────────────
#define NVM3_KEY_DEVICE_ID          0x0001UL

// ─────────────────────────────────────────────────────────────────────────────
//  ★★★ MY_DEVICE_ID_FALLBACK — 첫 번째 OTA 전용 ★★★
//
//  [우선순위 — fallback 권위 방식]
//   1~4 로 빌드 → 그 보드는 "무조건" 이 값. NVM3와 다르면 NVM3를 덮어씀.
//                 → 첫 OTA에서 잘못 넣어도 올바른 값으로 재OTA 하면 교정됨.
//   0xFF 로 빌드 → 이 상수 무시, 각 보드는 NVM3에 저장된 자기 값을 읽음.
//
//  [운용 규칙]
//   첫 OTA   : 보드별로 1/2/3/4 설정 후 4개 GBL 빌드 → 각 보드 ID 확정(NVM3 저장).
//   이후 OTA : 0xFF 단일 GBL 빌드 → 4개 보드 모두 동일 GBL로 업데이트.
//   ※ 모든 보드가 첫 OTA(1~4)를 마친 뒤 0xFF로 바꾸면 끝. 추가 주의사항 없음.
// ─────────────────────────────────────────────────────────────────────────────
#define MY_DEVICE_ID_FALLBACK  1   // ← 첫 OTA 시 1·2·3·4 로 변경 후 보드별 빌드

// ─────────────────────────────────────────────────────────────────────────────
//  EUI-64 → device_id 매핑 테이블  (2순위 폴백, MY_DEVICE_ID_FALLBACK 보다 우선)
//  ★ 보드 EUI-64를 알고 있으면 여기에 기입하면 첫 OTA도 단일 빌드 가능 ★
//  리틀엔디안(LSB first). all-zero = 미기입(매칭 안 됨).
//  EUI-64 확인: TX에서 `info` CLI 또는 Simplicity Commander `device info`
// ─────────────────────────────────────────────────────────────────────────────
typedef struct { uint8_t eui64[8]; uint8_t device_id; } EUI64DeviceIdEntry;

static const EUI64DeviceIdEntry g_eui64_map[] = {
  {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 1},  // RX 보드 1
  {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 2},  // RX 보드 2
  {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 3},  // RX 보드 3
  {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 4},  // RX 보드 4
};
static const uint8_t g_eui64_map_count =
  (uint8_t)(sizeof(g_eui64_map) / sizeof(g_eui64_map[0]));

// ─────────────────────────────────────────────────────────────────────────────
//  RF 메시지 타입 (TX app_process.h와 반드시 동일한 값)
// ─────────────────────────────────────────────────────────────────────────────
#define MSG_TYPE_OTA_PREPARE        0xF0U
#define MSG_TYPE_OTA_PREPARE_ACK    0xF1U
#define MSG_TYPE_FW_VERSION_REPORT  0xF2U
#define MSG_TYPE_ID_ANNOUNCE        0xA0U
#define MSG_TYPE_POLL_REQUEST       0xB0U
#define MSG_TYPE_POLL_RESPONSE      0xB1U

// ─────────────────────────────────────────────────────────────────────────────
//  펌웨어 버전 — OTA 업데이트 시 변경
// ─────────────────────────────────────────────────────────────────────────────
#define FW_VERSION_MAJOR    0x01U

// ─────────────────────────────────────────────────────────────────────────────
//  ID_ANNOUNCE 타이밍
// ─────────────────────────────────────────────────────────────────────────────
#define ID_ANNOUNCE_DELAY_MS        1000U
#define ID_ANNOUNCE_RETRY_MS        10000U   // 10s: TX 미부팅 시 빠른 실패 감지

// ─────────────────────────────────────────────────────────────────────────────
//  app_init.c에서 호출하는 공개 함수
// ─────────────────────────────────────────────────────────────────────────────
/// resume 경로(SUCCESS)에서 tick이 돌도록 EM2 sleep 차단 + wakeup timer 시작.
void join_sleep_block_enable(void);

#endif // APP_PROCESS_H
