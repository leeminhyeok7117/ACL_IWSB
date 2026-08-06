/***************************************************************************//**
 * @file    iq_capture.h
 * @brief   IWSB — RAIL IQ 캡처 코어 (Connect 스택과 시분할 공유)
 *
 *  [목적]
 *    별 토폴로지 노드들이 측정 캠페인 동안 서로 더미 비콘을 주고받을 때,
 *    청취 노드가 그 순간의 원시 IQ 샘플을 취득한다(날개 전개각 추정용).
 *    IQ 자체 해석/각도 계산은 지상국에서 하므로 노드는 "취득 + 전달"만 담당.
 *
 *  [핵심 설계 — 왜 이렇게 하나]
 *    · Connect 스택이 RAIL 을 소유(패킷 모드). RAIL 의 IQ 소스
 *      (RX_IQDATA_FILTLSB, FIFO 모드)는 RX_PACKET_DATA 와 "동시 사용 불가".
 *      → 측정 슬롯(네트워킹 일시중단 구간) 안에서만 잠깐 라디오를 빌려
 *        IQ 모드로 전환→캡처→패킷 모드로 복원 하는 시분할 방식.
 *    · 스택의 RAIL 핸들은 공개 API 로 노출되지 않는다. 대신 RAIL 이
 *      부팅 시 호출하는 weak 콜백 RAILCb_SetupRxFifo(railHandle) 를
 *      strong 으로 override 하여 핸들을 가로챈다(iq_capture.c).
 *    · ★ OTA/Connect 로직은 건드리지 않는다. 이 모듈은 측정 슬롯에서만
 *        호출되고, 반환 시 RAIL 을 패킷 모드로 원복한다.
 *
 *  [공식 문서 근거 — Gecko SDK 4.5.0]
 *    · rail_types.h  RX_IQDATA_FILTLSB / RAIL_DataMethod_t::FIFO_MODE
 *    · rail.h        RAIL_ConfigData(), RAIL_ReadRxFifo(),
 *                    RAIL_GetRxFifoBytesAvailable()  — IQ 캡처 예제 블록
 *    · rail.h 주의:  IQ 소스는 오버샘플레이트로 나오며 300 kSps 이상은
 *                    CPU 가 못 따라감 → PHY 데이터레이트가 낮아야 안전.
 *                    (xG12=Series1 은 xG22+ 와 달리 32bit 정렬 리셋 불필요)
 *******************************************************************************
 * SPDX-License-Identifier: Zlib
 ******************************************************************************/
#ifndef IQ_CAPTURE_H
#define IQ_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>

// ─────────────────────────────────────────────────────────────────────────────
//  IQ 캡처 파라미터 (하드웨어로 튜닝 예정 — 지상 검증 시 조정)
// ─────────────────────────────────────────────────────────────────────────────
// ★ 512 = librail 의 weak 기본 RAILCb_SetupRxFifo 가 쓰는 정확히 그 크기(rail.h
//   문서: "a default implementation... 512-byte receive FIFO"). 우리가 이 콜백을
//   override 하는 목적은 오직 railHandle 을 가로채는 것뿐이므로, 실제 동작을
//   원본과 최대한 동일하게 유지하기 위해 크기도 그대로 맞춘다(불필요한 편차 제거).
#define IQ_RXFIFO_SIZE        512U    // RAIL RX FIFO — librail 기본값과 동일
#define IQ_SAMPLES_PER_LINK   64U     // 링크당 OBC 로 넘길 디시메이트 IQ 샘플 수
#define IQ_DECIM              8U      // 원시 샘플 N개당 1개 유지(윈도 전체로 분산)
#define IQ_CAPTURE_TIMEOUT_MS 120U    // 한 링크 캡처 블로킹 상한(측정 슬롯 < 250ms)
// AGC(자동 이득 조절) 정착 전 구간을 몇 샘플 버릴지.
//   수신 시작 직후에는 이득이 과도해 강한 신호가 ±32767 로 포화한다.
//   실측(연속송신 전환 후): idx 0~25 포화율 50%, idx 32 이후 포화 0%.
//   → 48 샘플 버리면 여유 있게 정착 구간만 기록. (48+64=112 ≤ FIFO 128샘플)
#define IQ_SETTLE_DISCARD     48U

// 한 복소 샘플 = 16bit signed I, 16bit signed Q (RX_IQDATA_FILTLSB 하위 16bit).
typedef struct __attribute__((packed)) {
  int16_t i;
  int16_t q;
} iq_sample_t;   // sizeof == 4

// ─────────────────────────────────────────────────────────────────────────────
//  RF 메시지 타입 (0xC3~ : 기존 측정군 0xC0~0xC2 와 구분) — TX/RX 동일 값 필수
//   MSG_TYPE_IQ_REPORT : listener → TX(Master) 로 IQ 조각 전송(프래그먼트)
//   payload:
//     [0]=type [1]seq [2]tx_id(송신원) [3]ch [4]rx_id(청취자)
//     [5]frag_idx [6]n_frags [7]n_samp(이 조각의 복소샘플 수)
//     [8..]= iq bytes  (n_samp * 4, little-endian  I_lo I_hi Q_lo Q_hi …)
// ─────────────────────────────────────────────────────────────────────────────
#define MSG_TYPE_IQ_REPORT    0xC3U
#define IQ_REPORT_HDR         8U      // 위 헤더 바이트 수
// 한 프래그먼트에 담는 복소샘플 수(Connect 앱 페이로드 ~111B 안에서 안전).
#define IQ_FRAG_SAMPLES       20U     // 20*4 + 8 = 88 bytes/frag
// 링크당 프래그먼트 수(올림).
#define IQ_N_FRAGS  ((IQ_SAMPLES_PER_LINK + IQ_FRAG_SAMPLES - 1U) / IQ_FRAG_SAMPLES)
// 프래그먼트 간 전송 간격(ms) — 연속 송신으로 인한 Connect TX 큐 고갈/충돌 방지.
#define IQ_FRAG_GAP_MS       20U

// ─────────────────────────────────────────────────────────────────────────────
//  [Master] 완성된 링크 IQ 레코드 (TX 집계 저장소 + OBC I2C 리드백 공용 타입)
//   한 레코드 = 특정 (송신원 tx_id → 청취자 rx_id, 채널 ch) 링크의 IQ 세트.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
  uint8_t     tx_id;     // 송신원 device_id (0=TX/Master, 1..N=RX)
  uint8_t     rx_id;     // 청취자 device_id (0=TX/Master, 1..N=RX)
  uint8_t     channel;   // 측정 채널(bit7=밴드, bit0..6=채널)
  uint8_t     seq;       // 측정 라운드 시퀀스
  uint8_t     n_samp;    // 유효 복소샘플 수
  iq_sample_t iq[IQ_SAMPLES_PER_LINK];
} iq_record_t;           // sizeof == 5 + IQ_SAMPLES_PER_LINK*4

// Master 집계 링버퍼 길이(2의 거듭제곱). 실사용 = LEN-1 개.
//   축소 스윕(20링크 × 5채널 = 100 레코드)이 한 번에 담기도록 128칸.
//   RAM: 128 × 261B ≈ 33KB (링) + 리드백 스테이징 ≈ 33KB. FG12 힙 여유 ~236KB.
#define IQ_RING_LEN       128U
// 직렬화된 레코드 1건 크기: [tx_id][rx_id][ch][seq][n_samp] + iq(고정 IQ_SAMPLES_PER_LINK*4)
#define IQ_REC_HDR        5U
#define IQ_REC_SIZE       (IQ_REC_HDR + IQ_SAMPLES_PER_LINK * 4U)

// ─────────────────────────────────────────────────────────────────────────────
//  OBC 리드백 프레임 (I2C / SPI 공통)
//
//   [0] MAGIC(0xA5)      : 동기 확인용. 다른 값이면 타이밍이 어긋난 것.
//   [1] mission_active   : 1=미션 수행 중, 0=대기/완료
//   [2] sweeps_done      : 지금까지 완료한 스윕 수 (0~100)
//   [3] sweeps_total     : 미션 전체 스윕 수 (100)
//   [4] batch_ready      : 1=이 프레임에 새 배치가 실려 있음
//   [5] n_records        : 실려 있는 레코드 수
//   [6] reserved / [7] reserved
//   [8..] 레코드들 (각 IQ_REC_SIZE 고정)
//
//   ★ EFR32 는 I2C/SPI 모두 슬레이브라 스스로 전송을 시작할 수 없다. 그래서
//     "사이클 완료 플래그"는 이 헤더로 표현하고, OBC 가 읽어 가면 된다.
//     앞 8바이트만 읽으면 상태 확인(가벼운 폴링), 전체를 읽으면 데이터 회수.
// ─────────────────────────────────────────────────────────────────────────────
#define IQ_FRAME_MAGIC    0xA5U
#define IQ_FRAME_HDR      8U
#define IQ_READBACK_MAX   (IQ_FRAME_HDR + (IQ_RING_LEN - 1U) * IQ_REC_SIZE)

// ─────────────────────────────────────────────────────────────────────────────
//  API
// ─────────────────────────────────────────────────────────────────────────────

/// RAIL 핸들 확보 여부(부팅 후 RAILCb_SetupRxFifo 가 불렸으면 true).
bool iq_capture_ready(void);

// [비행 안전] 캡처 후 패킷 수신 복구에 실패해 라디오가 귀머거리 상태인가?
bool iq_radio_is_deaf(void);
// 귀머거리 상태면 수신 복구를 재시도한다. tick 에서 주기적으로 호출할 것.
// 반환: true = 정상(또는 복구 성공), false = 아직 복구 실패.
bool iq_radio_recover(void);

/**
 * 지정 채널에서 IQ 버스트 캡처. RAIL 을 IQ FIFO 모드로 전환→읽기→패킷 모드 복원.
 * ★ 반드시 측정 슬롯(Connect 네트워킹 일시중단) 안에서만 호출할 것.
 * ★ 블로킹. timeout_ms 로 상한 보장(호출 컨텍스트: emberAfTickCallback).
 *
 * ★★ resume_channel 이 핵심: 우리는 Connect 스택 몰래 RAIL_Idle() 로 라디오를
 *   세우고 IQ 모드로 바꾼다. 스택은 여전히 "내가 수신 중"이라고 믿고 있으므로,
 *   반환 전에 반드시 우리가 직접 RAIL_StartRx(resume_channel) 로 수신을 되살려야
 *   한다. 이걸 스택(emberSetRadioChannel)에 맡기면, 스택이 채널 변경이 없다고
 *   판단해 StartRx 를 재발행하지 않아 라디오가 "먹통"으로 남는다.
 *
 * @param channel        캡처할 물리 채널 번호
 * @param resume_channel 캡처 후 수신을 재개할 채널(보통 캡처와 동일한 측정 채널.
 *                       슬롯 종료 시 호출측이 home 으로 다시 옮긴다)
 * @param out            결과 버퍼(≥ max_samples 개)
 * @param max_samples    최대 저장 복소샘플 수
 * @param decim          디시메이션(원시 decim개당 1개 유지, 0→1로 보정)
 * @param timeout_ms     캡처 블로킹 상한(ms)
 * @return 실제 저장한 복소샘플 수(0 = 실패/무신호)
 */
uint16_t iq_capture_burst(uint8_t channel, uint8_t resume_channel,
                          iq_sample_t *out,
                          uint16_t max_samples, uint16_t decim,
                          uint32_t timeout_ms);

/**
 * 지정 채널에서 연속 송신(PN9 스트림) 시작 — 송신원 전용.
 * ★ Connect 의 emberStartTxStream() 은 "네트워크 미가입" 상태에서만 허용되어
 *   (내부에서 emNetworkState/emScanState 를 검사) 가입 중인 우리 노드에서는
 *   항상 0x70(EMBER_INVALID_CALL) 이 난다. 그래서 RAIL 레벨로 직접 수행한다.
 * ★ 반드시 측정 슬롯 안에서만 호출하고, iq_stream_stop() 을 모든 경로에서 호출할 것
 *   (켜진 채 남으면 해당 채널을 계속 점유한다).
 * @return true = 시작됨
 */
bool iq_stream_start(uint8_t channel);

/**
 * 연속 송신 중지 + 패킷 수신 재개.
 * @param resume_channel 수신을 재개할 채널(보통 측정 채널)
 */
void iq_stream_stop(uint8_t resume_channel);
// 송신 스트림만 즉시 정지(복구 없음). 인터럽트 문맥에서 호출 가능한 최소 조치 —
// tick 이 굶어도 라디오가 계속 송신하는 상황을 막는 안전장치용.
void iq_stream_abort(void);

#endif // IQ_CAPTURE_H
