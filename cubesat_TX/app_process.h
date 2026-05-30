/***************************************************************************//**
 * @file
 * @brief app_process.h — OTA 자동화 공유 타입/선언
 ******************************************************************************/
#ifndef APP_PROCESS_H
#define APP_PROCESS_H

#include "stack/include/ember.h"
#include <stdint.h>
#include <stdbool.h>

// ─────────────────────────────────────────────────────────────────────────────
//  NVM3 키
// ─────────────────────────────────────────────────────────────────────────────
#define NVM3_KEY_SLAVE_TABLE        0x0002UL   // uint8_t[MAX_SLAVES], device_id 배열

// ─────────────────────────────────────────────────────────────────────────────
//  OTA 자동화 상태머신
//  주의: OTA_VERIFYING / OTA_COMPLETE 는 미구현 상태로 제거됨.
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
  OTA_IDLE = 0,               // 대기 중
  OTA_RECEIVING_FW,           // AP_OBC로부터 GBL 수신 중
  OTA_FW_READY,               // GBL 수신 완료, CMD 0x03 대기 (우주 모드)
  OTA_FW_READY_MANUAL,        // J-Link 적재 완료, BTN0 대기 (지상 모드)
  OTA_WAITING_SLAVE_PREPARE,  // Slave 준비 ACK 대기
  OTA_DISTRIBUTING,           // OTA 배포 진행 중
  OTA_REQUEST_BOOTLOAD,       // 부트로드 요청 단계
  OTA_WAITING_BOOTLOAD,       // 부트로드 완료 대기
  OTA_ERROR                   // 에러 발생
} ota_master_state_t;

// ─────────────────────────────────────────────────────────────────────────────
//  RF 메시지 타입 (TX ↔ RX 커스텀 메시지)
//  RX(app_process.h)와 반드시 동일한 값 사용
// ─────────────────────────────────────────────────────────────────────────────
#define MSG_TYPE_OTA_PREPARE        0xF0U  // TX → RX: OTA 준비 요청
#define MSG_TYPE_OTA_PREPARE_ACK    0xF1U  // RX → TX: OTA 준비 완료 ACK
#define MSG_TYPE_FW_VERSION_REPORT  0xF2U  // RX → TX: 부팅 후 펌웨어 버전 보고
#define MSG_TYPE_ID_ANNOUNCE        0xA0U  // RX → TX: device_id 알림

// ─── TX 코디네이터 폴링 메시지 ────────────────────────────────────────────────
#define MSG_TYPE_POLL_REQUEST       0xB0U  // TX → RX
#define MSG_TYPE_POLL_RESPONSE      0xB1U  // RX → TX (payload[1]=device_id, [2]=fw_ver)

// ─────────────────────────────────────────────────────────────────────────────
//  Slave 테이블
// ─────────────────────────────────────────────────────────────────────────────
#define MAX_SLAVES  4U

typedef struct {
  uint8_t     device_id;   // 사용자 정의 ID (1~4, NVM3 저장값)
  EmberNodeId node_id;     // Connect 네트워크 주소 (Join 시 자동 할당, 재부팅 시 EMBER_NULL_NODE_ID)
  bool        registered;  // 등록 여부
  bool        online;      // 마지막 폴링 사이클에서 응답 여부
  uint8_t     fw_version;  // 마지막 폴링에서 수신한 FW 버전
} SlaveEntry;

// ─────────────────────────────────────────────────────────────────────────────
//  외부 함수 선언 — app_process.c에 구현, app_cli.c에서 호출
// ─────────────────────────────────────────────────────────────────────────────
void send_sweep_start_msg(EmberNodeId target);
bool set_ota_target_by_device_id(uint8_t device_id);
void print_slave_list(void);
void polling_restart(void);

#endif // APP_PROCESS_H
