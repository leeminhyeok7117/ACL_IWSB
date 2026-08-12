/***************************************************************************//**
 * @file    iq_capture.c
 * @brief   IWSB — RAIL IQ 캡처 코어 구현 (Connect 스택과 시분할 공유)
 *
 *  자세한 설계 배경은 iq_capture.h 주석 참조.
 *  이 파일은 Connect/OTA 코드를 전혀 건드리지 않으며, 오직 측정 슬롯에서만
 *  RAIL 을 잠시 IQ 모드로 빌렸다가 패킷 모드로 원복한다.
 *******************************************************************************
 * SPDX-License-Identifier: Zlib
 ******************************************************************************/
#include "iq_capture.h"
#include "rail.h"
#include "rail_types.h"
#include "sl_sleeptimer.h"

// -----------------------------------------------------------------------------
//  RAIL 핸들 가로채기 — RAILCb_SetupRxFifo override (librail weak 기본 대체)
//
//  RAIL 은 부팅 시 이 콜백을 호출하여 앱이 RX FIFO 를 설정하도록 한다.
//  Connect(SoC)는 librail 의 weak 기본 구현을 쓰지만, 여기서 strong 으로
//  재정의하면:
//    1) 스택이 넘겨준 railHandle 을 붙잡아 두고(IQ 재설정에 사용),
//    2) 우리 정적 버퍼로 RX FIFO 를 설정(패킷 수신에도 그대로 사용됨).
//  ※ 링크 시 이 strong 정의가 librail 의 weak 정의를 대체한다(중복 아님).
// -----------------------------------------------------------------------------
static RAIL_Handle_t s_rail = NULL;
static int16_t        s_last_stop_st = -1;   // [진단] 마지막 StopTxStream 결과
static uint8_t       s_rxfifo[IQ_RXFIFO_SIZE] __attribute__((aligned(4)));

RAIL_Status_t RAILCb_SetupRxFifo(RAIL_Handle_t railHandle)
{
  s_rail = railHandle;
  uint16_t sz = IQ_RXFIFO_SIZE;
  RAIL_Status_t st = RAIL_SetRxFifo(railHandle, s_rxfifo, &sz);
  // [공식 문서 예제와 동일한 방어 코드] RAIL 은 하드웨어가 지원하는 크기 중
  // "요청 이하로 가장 가까운 값"을 골라 sz 에 되돌려줄 수 있다(rail.h:
  // "The returned FIFO size will be the closest allowed size..."). 요청과
  // 다르면 우리 가정(IQ_RXFIFO_SIZE)이 틀린 것이므로 조용히 넘어가지 않고
  // 실패 처리한다(RAIL_Init 이 이 경우 실패하도록 — 침묵 오동작 방지).
  if (sz != IQ_RXFIFO_SIZE) {
    return RAIL_STATUS_INVALID_PARAMETER;
  }
  // 멀티프로토콜 등으로 아직 설정 불가한 경우는 성공으로 처리(스택 기본 동작 유지).
  if (st == RAIL_STATUS_INVALID_STATE) {
    return RAIL_STATUS_NO_ERROR;
  }
  return st;
}

bool iq_capture_ready(void) { return s_rail != NULL; }

// -----------------------------------------------------------------------------
//  데이터 컨피그: 패킷 모드(Connect 평시) / IQ FIFO 모드(캡처 순간)
// -----------------------------------------------------------------------------
static const RAIL_DataConfig_t k_cfg_packet = {
  .txSource = TX_PACKET_DATA,
  .rxSource = RX_PACKET_DATA,
  .txMethod = PACKET_MODE,
  .rxMethod = PACKET_MODE,
};

static const RAIL_DataConfig_t k_cfg_iq = {
  .txSource = TX_PACKET_DATA,
  .rxSource = RX_IQDATA_FILTLSB,   // 하위 16bit I/Q → iq_sample_t 와 매칭
  .txMethod = FIFO_MODE,
  .rxMethod = FIFO_MODE,
};

// 라디오를 패킷 모드로 원복하고 "직접" 수신을 재개(캡처 종료/실패 공통 경로).
//
// ★ 왜 여기서 RAIL_StartRx 를 우리가 직접 불러야 하나:
//   Connect 스택은 자기가 라디오를 계속 수신 상태로 두고 있다고 믿는다. 우리가
//   그 몰래 RAIL_Idle() 로 세웠다가 패킷 모드로만 되돌려 놓으면, 스택은 아무것도
//   모르므로 수신 재개 명령을 다시 내리지 않는다 → 라디오가 조용히 "먹통"이 되어
//   그 이후 Master 패킷을 못 듣고, 결국 스택이 부모 연결 끊김을 선언한다.
//   (이전 구현은 이 재개를 emberSetRadioChannel 에 맡겼는데, 같은 채널로 다시
//    설정하면 스택이 변경 없음으로 보고 StartRx 를 재발행하지 않아 실패했다.)
//
// ★ 비행 안전: 두 번의 시도가 모두 실패하면 노드는 영구히 귀머거리가 된다.
//   그 상태를 s_radio_deaf 로 남겨 두고, 앱 tick 이 iq_radio_recover() 로
//   계속 재시도하게 한다(일시적 RAIL 상태 경합이면 다음 시도에서 살아난다).
static volatile bool    s_radio_deaf     = false;
static uint8_t          s_deaf_channel   = 0;

static bool restore_try(uint8_t resume_channel)
{
  RAIL_Idle(s_rail, RAIL_IDLE_ABORT, true);
  RAIL_ConfigData(s_rail, &k_cfg_packet);
  RAIL_ResetFifo(s_rail, false, true);
  if (RAIL_StartRx(s_rail, resume_channel, NULL) == RAIL_STATUS_NO_ERROR) {
    return true;
  }
  // 강한 재시도: FIFO 내용은 잃지만 여기서는 이미 회수가 끝난 뒤라 무관하다.
  RAIL_Idle(s_rail, RAIL_IDLE_FORCE_SHUTDOWN, true);
  RAIL_ConfigData(s_rail, &k_cfg_packet);
  RAIL_ResetFifo(s_rail, false, true);
  return (RAIL_StartRx(s_rail, resume_channel, NULL) == RAIL_STATUS_NO_ERROR);
}

static void restore_packet_mode(uint8_t resume_channel)
{
  // ★ 여기서 실패하면 라디오가 조용히 먹통이 되어 노드가 네트워크에서 이탈한다
  //   (스택은 자기가 수신 중이라 믿으므로 스스로 되살리지 않는다).
  if (restore_try(resume_channel)) {
    s_radio_deaf = false;
    return;
  }
  s_radio_deaf   = true;          // tick 이 iq_radio_recover() 로 계속 재시도
  s_deaf_channel = resume_channel;
}

bool iq_radio_is_deaf(void) { return s_radio_deaf; }

// ★ 라디오를 패킷 모드 + 지정 채널 수신으로 되돌린다(RAIL 레벨).
//   [왜 필요한가]
//     iq_capture_burst() 와 iq_stream_stop() 은 끝날 때 restore_packet_mode() 로
//     RAIL_StartRx() 를 직접 호출해 라디오를 "측정 채널" 수신에 묶어 둔다.
//     스택은 이 사실을 모르기 때문에, 이후 emberSetRadioChannelExtended(home) 은
//     EMBER_SUCCESS 를 돌려주면서도 실제 채널을 바꾸지 못한다.
//     (실측: st=0x00 인데 emberGetRadioChannel()==5, 2800 회 강제해도 그대로.
//      그 사이 폴링·MEAS_CMD·ID_ANNOUNCE 가 전부 끊겨 네트워크가 죽었다.)
//   → home 복귀도 반드시 RAIL 레벨에서 같이 해 줘야 한다.
void iq_radio_resume_on(uint8_t channel)
{
  if (s_rail == NULL) return;
  restore_packet_mode(channel);
}

// tick 에서 주기 호출: 수신 복구가 안 된 상태면 계속 되살리기를 시도한다.
bool iq_radio_recover(void)
{
  if (!s_radio_deaf || s_rail == NULL) {
    return true;
  }
  if (restore_try(s_deaf_channel)) {
    s_radio_deaf = false;
    return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
//  연속 송신(TX stream) — RAIL 레벨로 직접 수행
//
//  ★ 왜 Connect 의 emberStartTxStream() 을 쓰지 않는가:
//    그 API 는 라이브러리 내부에서
//        if (emScanState != 0 || emNetworkState != 0) return EMBER_INVALID_CALL;
//    를 검사한다(디스어셈블로 확인). 즉 "네트워크에 가입되지 않은 상태"에서만
//    허용되는 RF 검증용 기능이라, 측정 중에도 네트워크를 유지해야 하는 우리
//    용도로는 영구히 0x70(EMBER_INVALID_CALL) 을 돌려준다.
//    → 우리는 이미 RAILCb_SetupRxFifo 로 스택의 RAIL 핸들을 확보해 두었고
//      IQ 캡처에서도 같은 방식으로 라디오를 잠깐 빌려 쓴다. 송신 스트림도
//      동일하게 RAIL 레벨에서 직접 켜고 끈다(스택 상태 검사 없음).
//    ※ 캡처와 마찬가지로 반드시 측정 슬롯 안에서만, 그리고 stop 을 모든 경로에서
//      호출해야 한다(켜진 채 남으면 해당 채널을 계속 점유).
// -----------------------------------------------------------------------------
bool iq_stream_start(uint8_t channel)
{
  if (s_rail == NULL) {
    return false;
  }
  // 진행 중인 라디오 동작 정리 후 스트림 시작.
  // (RAIL 문서: "All ongoing radio operations will be stopped before
  //  transmission begins" — 명시적으로 한 번 더 idle 시켜 상태를 확정한다.)
  RAIL_Idle(s_rail, RAIL_IDLE_ABORT, true);
  // PN9 = 의사난수 변조 스트림(실제 통신과 같은 대역폭을 점유).
  //   ※ 2GFSK 특성상 비트값에 따라 I/Q 위상 회전 방향이 뒤집힌다(정상 동작).
  //     진폭 √(I²+Q²) 은 그 영향을 받지 않으므로 측정에는 진폭을 쓴다.
  return (RAIL_StartTxStream(s_rail, (uint16_t)channel, RAIL_STREAM_PN9_STREAM)
          == RAIL_STATUS_NO_ERROR);
}

// 송신 스트림만 즉시 정지한다(패킷 모드 복구는 하지 않는다).
//   ★ 목적: 인터럽트 문맥에서 호출 가능한 최소 조치.
//     스트림이 켜진 채로 남으면 라디오가 계속 송신 상태라 Connect MAC 이 단 한
//     바이트도 보내지 못하고, 송신 큐가 가득 차 EMBER_MAC_TRANSMIT_QUEUE_FULL(0x39)
//     로 모든 통신이 영구히 죽는다. tick 이 굶어도 이것만은 반드시 멈춰야 한다.
//     (전체 복구 = 채널 원복/수신 재개는 tick 에서 iq_stream_stop 이 수행)
void iq_stream_abort(void)
{
  if (s_rail == NULL) {
    return;
  }
  s_last_stop_st = (int16_t)RAIL_StopTxStream(s_rail);
}

// [진단] 현재 RAIL 라디오 상태. 슬롯이 끝났는데 TX 비트가 남아 있으면
//   PN9 스트림이 안 꺼진 것이고, 그러면 MAC 이 계속 바빠 채널을 못 옮긴다.
//   RAIL_RF_STATE_IDLE=1, RX=2, TX=4 (ACTIVE 비트 포함)
uint8_t iq_radio_state(void)
{
  if (s_rail == NULL) return 0xFFU;
  return (uint8_t)RAIL_GetRadioState(s_rail);
}

// [진단] 마지막 RAIL_StopTxStream() 반환값(-1 = 아직 호출된 적 없음).
int16_t iq_last_stop_status(void) { return s_last_stop_st; }

void iq_stream_stop(uint8_t resume_channel)
{
  if (s_rail == NULL) {
    return;
  }
  s_last_stop_st = (int16_t)RAIL_StopTxStream(s_rail);
  // 송신 모드에서 빠져나온 뒤 패킷 수신을 되살린다(스택은 이 사실을 모르므로
  // 우리가 직접 StartRx 해야 한다 — restore_packet_mode 주석 참조).
  restore_packet_mode(resume_channel);
}

uint16_t iq_capture_burst(uint8_t channel, uint8_t resume_channel,
                          iq_sample_t *out,
                          uint16_t max_samples, uint16_t decim,
                          uint32_t timeout_ms)
{
  if (s_rail == NULL || out == NULL || max_samples == 0U) {
    return 0U;
  }
  if (decim == 0U) decim = 1U;

  // 1) 라디오 유휴 → IQ FIFO 모드로 전환.
  RAIL_Idle(s_rail, RAIL_IDLE_ABORT, true);
  if (RAIL_ConfigData(s_rail, &k_cfg_iq) != RAIL_STATUS_NO_ERROR) {
    restore_packet_mode(resume_channel);
    return 0U;
  }
  RAIL_ResetFifo(s_rail, false, true);   // RX FIFO 비우고 캡처 시작 준비

  // 2) 캡처 채널에서 수신 시작(스택 우회 — 이 순간엔 우리가 라디오 소유).
  if (RAIL_StartRx(s_rail, channel, NULL) != RAIL_STATUS_NO_ERROR) {
    restore_packet_mode(resume_channel);
    return 0U;
  }

  // 3) "한 번에 한 덩어리" 캡처.
  //
  //    ★ 왜 스트림을 계속 쫓아가며 읽지 않는가:
  //      IQ 소스는 하드웨어 오버샘플레이트로 쏟아지며, rail.h 는 300 kSps 이상이면
  //      CPU 가 못 따라간다고 경고한다. 우리 PHY 는 그보다 빠르다. 흘러나오는 것을
  //      루프로 쫓아가면 도중에 FIFO 오버플로가 나고, 유실 바이트 수가 4의 배수가
  //      아니면 이후 모든 샘플이 1~3바이트 밀려 읽힌다.
  //      (실측 증상: 큰 값들의 하위 바이트가 예외 없이 0x00/0xFF → 8bit 데이터가
  //       상위 바이트로 밀려 들어간 전형적 misalignment. 정상값의 256배로 부풀려짐.)
  //    → FIFO 가 리셋된 직후부터 필요한 양(need 바이트)이 모일 때까지만 기다렸다가
  //      "연속된 한 덩어리"로 읽고 즉시 종료한다. 오버플로 이전에 끝나므로 정렬이
  //      깨질 여지가 없고, 샘플이 시간적으로 연속이라 파형 분석에도 유리하다.
  //    ※ decim 은 더 이상 스트림을 건너뛰는 데 쓰지 않는다(무필터 데시메이션은
  //      에일리어싱을 유발). 인자는 호환을 위해 남겨두되 무시한다.
  (void)decim;

  //    ★ 앞부분 버리기(IQ_SETTLE_DISCARD): 수신 시작 직후에는 AGC(자동 이득 조절)가
  //      아직 정착하지 않아 강한 스트림 신호에 이득이 과도하게 걸려 값이 ±32767 로
  //      포화한다. 실측: idx 0~25 구간 포화율 50%(진폭 32944), idx 32 이후 포화 0%
  //      (진폭 ~8700, 깨끗한 신호). → 정착 전 구간을 읽어 버린 뒤 그 다음부터
  //      기록하면 64샘플 전부를 쓸 수 있다.
  uint16_t want = max_samples;
  const uint16_t cap = (uint16_t)(IQ_RXFIFO_SIZE / 4U);   // FIFO 최대 샘플수
  if (want > cap) {
    want = cap;
  }
  uint16_t discard = IQ_SETTLE_DISCARD;
  if ((uint16_t)(discard + want) > cap) {          // FIFO 용량 초과 방지
    discard = (uint16_t)(cap - want);
  }
  const uint16_t need = (uint16_t)((discard + want) * 4U);

  const uint32_t t0 = sl_sleeptimer_get_tick_count();
  // ★ sl_sleeptimer_ms_to_tick() 의 인자는 uint16_t 다. 호출자가 65535 를 넘는
  //   값을 주면 조용히 잘려 타임아웃이 엉뚱해지므로 여기서 미리 묶는다.
  uint32_t tmo_ms = (timeout_ms > 60000U) ? 60000U : timeout_ms;
  const uint32_t timeout_ticks = sl_sleeptimer_ms_to_tick((uint16_t)tmo_ms);

  uint16_t avail = 0U;
  while (avail < need) {
    if ((sl_sleeptimer_get_tick_count() - t0) > timeout_ticks) {
      break;   // 무신호/수신 미개시 → 상한에서 탈출(모인 만큼만 사용)
    }
    avail = RAIL_GetRxFifoBytesAvailable(s_rail);
  }

  // ★★ 읽기 전에 수신을 멈춘다 — 정렬 붕괴를 원천 차단하는 핵심 단계.
  //    읽는 동안에도 IQ 가 계속 쏟아지면(오버샘플레이트 > 처리속도) 읽는 도중
  //    FIFO 오버플로가 나고, 유실 바이트가 4의 배수가 아니면 이후 전부 밀려
  //    읽힌다. RAIL_IDLE 은 rail_types.h 문서상 "수신을 끄고 예약된 동작을
  //    취소"할 뿐 버퍼를 훼손하지 않는다(버퍼를 망가뜨리는 것은
  //    RAIL_IDLE_FORCE_SHUTDOWN). 따라서 여기서 수신만 정지시키면 FIFO 내용은
  //    정지 상태로 남아 안전하게 회수할 수 있다.
  RAIL_Idle(s_rail, RAIL_IDLE, true);

  // 정지 후 최종 확정된 양을 다시 조회(정지 직전까지 들어온 분 포함).
  avail = RAIL_GetRxFifoBytesAvailable(s_rail);

  // 4의 배수로 내림(샘플 경계 유지).
  uint16_t take = (avail < need) ? avail : need;
  take &= (uint16_t)~3U;

  // (a) AGC 정착 전 구간을 읽어서 버린다(위 IQ_SETTLE_DISCARD 주석 참조).
  //     ★ 모인 양이 discard+want 에 못 미치면 "버리는 양"을 먼저 줄여서
  //       기록할 want 개를 최대한 확보한다. (이 조건을 drop>avail_samp 로
  //       두면 예컨대 80샘플만 모였을 때 48개를 그대로 버려 32개만 남는다.
  //       아래처럼 drop+want 로 비교해야 16개만 버리고 64개를 온전히 얻는다.)
  uint16_t avail_samp = (uint16_t)(take / 4U);
  uint16_t drop = discard;
  if ((uint16_t)(drop + want) > avail_samp) {
    drop = (avail_samp > want) ? (uint16_t)(avail_samp - want) : 0U;
  }
  for (uint16_t d = 0; d < drop; d++) {
    uint8_t junk[4];
    if (RAIL_ReadRxFifo(s_rail, junk, 4U) != 4U) {
      break;
    }
  }

  // (b) 정착된 구간을 기록한다.
  uint16_t n = 0U;
  while (n < want && (uint16_t)((drop + n) * 4U) < take) {
    uint8_t raw[4];
    if (RAIL_ReadRxFifo(s_rail, raw, 4U) != 4U) {
      break;   // 예기치 못한 부분 읽기 → 정렬 보호를 위해 즉시 중단
    }
    out[n].i = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    out[n].q = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    n++;
  }

  // 4) 패킷 모드로 원복 + 수신 재개(Connect/OTA 정상 동작 재개).
  restore_packet_mode(resume_channel);
  return n;
}
