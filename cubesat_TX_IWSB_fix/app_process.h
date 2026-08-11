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
#define MEAS_N_BEACONS              4U     // (구) 라운드당 비콘 수 — 연속 송신 전환으로 미사용
#define MEAS_BEACON_SETUP_MS        60U    // cmd 수신→측정 시작 대기(리스너 채널전환 여유)
#define MEAS_BEACON_GAP_MS          15U    // (구) 비콘 간 간격 — 연속 송신 전환으로 미사용
#define MEAS_SLOT_MS                250U   // 측정 슬롯 길이(이후 home 복귀)

// ─── [IQ] 연속 송신(TX stream) 타이밍 ────────────────────────────────────────
//   [왜 바꿨나] 짧은 비콘(패킷)은 15ms 간격으로 수 백 µs 만 존재하는데, IQ 캡처
//   창은 64샘플 ≈ 수십 µs 에 불과해 둘이 겹칠 확률이 사실상 0 이었다. 실측에서
//   모든 레코드가 "수신기 켜질 때의 순간 반응 8샘플 + 잡음 56샘플" 로만 채워진
//   이유가 이것. → 송신원이 슬롯 동안 "끊김 없이" 쏘면 어느 시점에 캡처해도
//   반드시 신호가 담긴다. TX_STREAM_PN9(의사난수 변조)는 실제 통신과 같은
//   대역폭을 점유하므로 경로 특성 측정에 적합하다(무변조 CW 보다 유리).
#define MEAS_STREAM_START_MS        20U    // 채널 전환 후 이 시점부터 송신원이 스트리밍 시작
#define MEAS_STREAM_STOP_MS         230U   // 슬롯(250ms) 종료 직전 반드시 정지
#define MEAS_CAPTURE_AT_MS          80U    // 리스너가 캡처를 수행하는 시점(스트림 한가운데)
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

// ─── [IWSB 미션] OBC 명령 하나로 전 과정 자동 수행 ───────────────────────────
//   0x05 → 버퍼가 비었는지 확인 후 스윕 100회 시작. 한 스윕마다 배치를 준비하고
//   OBC 가 가져갈 때까지 대기(유실 방지). 상태는 리드백 프레임 헤더로 전달.
#define IQ_MISSION_SWEEPS   100U    // 미션 1회 = 스윕 100번 (프레임 헤더로도 전달)
void     iq_meas_trigger(uint16_t dur_s);   // 미션 시작(인자는 하위호환용, 미사용)
// 0x05 공통 진입점 — OBC(SPI/I2C)와 CLI 가 동일 경로를 타도록 분리(OTA 가드 포함)
void     obc_cmd_iq_start_mirror(void);
// [지상 테스트 전용] 배치 자동 회수 on/off (비행 기본값 OFF).
void     iq_auto_drain_set(bool on);
bool     iq_auto_drain_get(void);
void     iq_mission_abort(void);            // 미션 중단
uint8_t  iq_mission_is_active(void);        // 1=수행 중
uint16_t iq_mission_sweeps_done(void);      // 완료한 스윕 수
uint8_t  iq_mission_batch_ready(void);      // 1=회수 대기 중인 배치 있음
void     iq_batch_taken(void);              // 배치 회수 완료 통지(다음 스윕 허가)
void     iq_ring_reset(void);               // 링 즉시 비움

// ─── [IQ 측정] Master 집계 + OBC 리드백 (app_process.c 구현, app_init.c/CLI 호출) ─
uint8_t  iq_readback_count(void);                             // 큐에 남은 IQ 레코드 수
// 쌓인 IQ 레코드 전부를 한 프레임으로 직렬화(app_init.c I2C 리드백에서 호출).
//   프레임: [n_records] + 레코드들(각 IQ_REC_SIZE 고정). 호출 후 링은 비워짐.
uint16_t iq_readback_drain_all(uint8_t *buf, uint16_t buf_size);
// I2C 리드백 스테이징(app_init.c 구현): iq_readback_drain_all() 로 i2c_tx_buf 채움.
void     obc_stage_iq_readback(void);
// ACK/timeout 후 이전 프레임을 폐기하고 SPI MISO를 0x5A idle로 복귀.
void     obc_release_iq_readback(void);
// 공용 스테이징 버퍼 접근(CLI 덤프가 재사용 — RAM 중복 방지)
const uint8_t *obc_readback_buffer(uint16_t *len);
// 스테이징된 리드백 프레임이 버스(SPI/I2C)로 전부 빠져나갔는가?
//   OBC 는 0x05 만 보내고 회수 명령을 따로 보내지 않으므로, 이 관찰로 회수를 판정한다.
bool obc_readback_consumed(void);

// ─── [지상 테스트] SPI 슬레이브 진단 ────────────────────────────────────────
//   SPI 슬레이브는 마스터가 클럭을 넣기 전엔 아무 일도 안 하므로, "무엇이 실제로
//   들어왔는가"를 세어야 배선/클럭/프레이밍 중 어디가 문제인지 알 수 있다.
typedef struct {
  uint32_t bytes;       // 수신 총 바이트 (0 이면 클럭이 아예 안 들어오는 것)
  uint32_t cs_edges;    // CS 상승 엣지 수 (0 이면 CS 배선/극성 문제)
  uint32_t frames;      // 내용이 있는 트랜잭션 수
  uint16_t last_len;    // 마지막 프레임 길이
  uint8_t  last_cmd;    // 마지막 프레임 선두 바이트
  uint16_t first_rx;    // 최초 수신 바이트 (0xFFFF = 아직 없음)
  uint32_t overrun;     // 수신 오버런 횟수 (0 이 아니면 SPI 클럭이 너무 빠름)
  uint16_t staged;      // 리드백 스테이징 길이
  uint16_t tx_idx;      // 다음 송신 인덱스
  uint8_t  cs_level;    // 현재 CS 핀 레벨 (유휴 시 1 이어야 정상)
  uint8_t  clk_level;   // 현재 CLK 핀 레벨 (유휴 시 0)
  uint8_t  mosi_level;  // 현재 MOSI 핀 레벨 (유휴 시 0)
} obc_spi_stats_t;

void obc_spi_get_stats(obc_spi_stats_t *s);
void obc_spi_reset_stats(void);
// 물리 SPI 없이 명령 처리 경로만 검증(ISR 이 하는 일을 흉내낸다).
void obc_spi_inject(const uint8_t *frame, uint16_t len);

#endif // APP_PROCESS_H
