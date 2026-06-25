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
#define NVM3_KEY_RSSI_LOG_CURSOR    0x0010UL   // uint32_t: RSSI 로그 쓰기 커서(절대주소) 영속화

// ─────────────────────────────────────────────────────────────────────────────
//  RSSI 측정 로그 (외부 SPI 플래시 — 날개 전개 감지용)
//   - 32Mbit(4MB) 외부 SPI 플래시. golden B 끝(0x200000) 위 빈 영역 사용.
//   - 기존 영역(부트로더/SLOT0/golden A·B) 절대 침범 금지.
//   - 앱이 직접 SPI를 만지지 않고 bootloader_*RawStorage 로 접근(fw_guard 와 동일).
// ─────────────────────────────────────────────────────────────────────────────
#define RSSI_LOG_BASE_ADDR          0x00200000UL   // 2 MB (golden B 끝 직후)
#define RSSI_LOG_REGION_SIZE        0x00200000UL   // 2 MB (→ 0x400000 = 칩 끝)

// 한 레코드 = 8바이트(고정). 4096바이트 섹터에 512개가 정확히 들어감(경계 깔끔).
typedef struct __attribute__((packed)) {
  uint8_t  tx_id;     // 비콘을 송신한 노드 device_id (TX 자신 = 0xFF 로 표기)
  uint8_t  rx_id;     // 그 비콘의 RSSI를 측정한 노드 device_id
  uint8_t  channel;   // 측정 채널. bit7=밴드(0=915MHz,1=2.4GHz), bit0..6=채널번호
  int8_t   rssi;      // 측정 RSSI (dBm)
  uint8_t  lqi;       // link quality indicator
  uint8_t  seq;       // 측정 라운드(sweep) 번호
  uint16_t tstamp_s;  // 부팅 후 경과초(또는 라운드 타임스탬프)
} rssi_record_t;      // sizeof == 8

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
//  RSSI 측정 프로토콜 (주파수 호핑 기반 별 토폴로지) — RX app_process.h와 동일 값 필수
//   1) MEAS_CMD 브로드캐스트 → 모든 노드가 ch 로 채널 전환
//   2) 송신원 T 가 빈 비콘 N개 브로드캐스트 → 나머지 노드가 message->rssi 측정
//   3) home 채널(0) 복귀 → MEAS_REPORT 로 TX 보고 → 외부 플래시 저장
//   ※ FH 컴포넌트는 포함됐으나 비활성(고정 ch0) → 앱이 직접 채널 스윕(안전).
// ─────────────────────────────────────────────────────────────────────────────
#define MSG_TYPE_MEAS_CMD           0xC0U  // TX→bcast : [1]seq [2]tx_id [3]ch [4]n_beacons
#define MSG_TYPE_MEAS_BEACON        0xC1U  // T →bcast : [1]seq [2]tx_id [3]ch [4]beacon_idx
#define MSG_TYPE_MEAS_REPORT        0xC2U  // listener→TX: [1]seq [2]tx_id [3]ch [4]rx_id [5]rssi [6]lqi [7]count

#define MEAS_TX_DEVICE_ID           0x00U  // 프로토콜/레코드에서 TX 자신 (RX 는 1~4)
#define MEAS_CH_FIRST               0U     // 스윕 시작 채널 (915.0 MHz)
#define MEAS_CH_LAST                20U    // 스윕 끝 채널 (915MHz 채널플랜 0~20 = 915.0~923.0 MHz)
#define MEAS_N_BEACONS              4U     // 라운드당 비콘 수
#define MEAS_BEACON_SETUP_MS        60U    // cmd 수신→비콘 시작 대기(리스너 채널전환 여유)
#define MEAS_BEACON_GAP_MS          15U    // 비콘 간 간격
#define MEAS_SLOT_MS                250U   // 측정 슬롯 길이(이후 home 복귀)
#define MEAS_REPORT_GAP_MS          40U    // 리포트 충돌 방지 스태거(device_id 당)
#define MEAS_COLLECT_MS             500U   // (TX) 라운드별 리포트 수집 윈도
#define MEAS_BAND_915               0x00U  // channel 바이트 bit7=0 → 915MHz
#define MEAS_AUTO_GAP_S             30U    // (TX) 자동 모드: 캠페인 종료 후 다음 시작까지 간격(초)

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

// ─── RSSI 측정 캠페인 (주파수 호핑 스윕) ──────────────────────────────────────
void     meas_campaign_start(void);   // 전체 스윕 1회 시작(CLI/조건에서 호출). 진행중이면 무시.
bool     meas_campaign_active(void);  // 캠페인 진행 여부
void     meas_auto_set(bool en);      // 자동 주기 스윕 on/off (기본 on)
bool     meas_auto_get(void);         // 자동 모드 상태

// ─── RSSI 로그 (외부 플래시) ─────────────────────────────────────────────────
void     rssi_log_init(void);                       // 부팅 시 1회: 지오메트리 검증 + 커서 복원
bool     rssi_log_append(const rssi_record_t *rec); // 레코드 1개 추가(섹터 lazy-erase + 커서 영속화)
uint32_t rssi_log_count(void);                      // 저장된 레코드 수
bool     rssi_log_read(uint32_t index, rssi_record_t *out); // index번째 레코드 읽기
void     rssi_log_clear(void);                      // 커서를 처음으로(논리적 비움)
bool     rssi_log_ready(void);                      // 로깅 가능 여부(칩크기 검증 통과 시 true)

// [TEST-ONLY ota_start] I2C 없이 CLI로 OTA 시작(지상 테스트용). 실위성 전 제거.
bool ota_start_test(uint8_t device_id);

#endif // APP_PROCESS_H
