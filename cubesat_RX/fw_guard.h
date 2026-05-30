/***************************************************************************//**
 * @file
 * @brief fw_guard.h — 앱 레벨 펌웨어 롤백 가드 (A안, 단일 슬롯 부트로더)
 *
 * [목적]
 *   외부 SPI 플래시의 여유 영역(raw storage)에 "직전 정상 펌웨어(golden)"를
 *   A/B 핑퐁으로 보관하고, 새 펌웨어가 N회 부팅 내 정상 동작을 확인하지 못하면
 *   golden을 SLOT0에 복원·설치하여 자동 복구한다.
 *
 * [무접근 위성 전제]
 *   - J-Link/CLI 불가, OTA만 가능, 전원만 들어오면 자동 복구되어야 함.
 *   - 부트로더는 단일 슬롯(SLOT0)이라 변경 불가 → raw storage API로 golden 관리.
 *
 * [방어 계층]
 *   1) verify-before-install (호출측에서)  — 거부될 이미지는 설치 안 함
 *   2) 부팅 카운터(NVM3)                    — N회 미확정 부팅 → 롤백
 *   3) 헬스 타임아웃 self-reset(호출측에서) — 복귀 실패 → 리셋(카운터 누적)
 *   4) 워치독(설정)                         — hang → 리셋(카운터 누적)
 *   5) A/B golden                           — 갱신 중 전원차단에도 직전 golden 보존
 ******************************************************************************/
#ifndef FW_GUARD_H
#define FW_GUARD_H

#include <stdint.h>
#include <stdbool.h>

// ─────────────────────────────────────────────────────────────────────────────
//  설정값 (운용 환경에 맞게 조정)
// ─────────────────────────────────────────────────────────────────────────────
#define FW_GUARD_NVM3_KEY          0x0003UL    // RX:0x0001=devid, 0x0002=(TX), 0x0003=guard
#define FW_GUARD_SLOT0_ID          0U
#define FW_GUARD_MAX_BOOT_ATTEMPTS 5U          // 이 횟수 미확정 부팅 → 롤백

// 외부 SPI 플래시 golden 영역 (SLOT0: 0x84000~0xF4000 와 겹치지 않게 높은 주소)
//   런타임에 partSize로 검증 — 안 맞으면 롤백 자동 비활성(fail-safe).
#define FW_GUARD_GOLDEN_A_ADDR     0x00180000UL  // 1.5 MB
#define FW_GUARD_GOLDEN_B_ADDR     0x001C0000UL  // 1.75 MB
#define FW_GUARD_GOLDEN_SIZE       0x00040000UL  // 256 KB (이미지 ~155KB 여유)

// ─────────────────────────────────────────────────────────────────────────────
//  API
// ─────────────────────────────────────────────────────────────────────────────

/// 부팅 직후 "최우선"으로 호출 (emberAfInitCallback 최상단).
/// pending_commit 상태면 boot_attempts 증가, 임계 도달 시 즉시 롤백(복귀 안 함).
/// bootloader_init() 이후에 호출해야 함(raw storage 사용).
void fw_guard_init(void);

/// 새 펌웨어가 정상 동작(네트워크 복귀 등) 확인됐을 때 1회 호출.
/// SLOT0(=현재 이미지)를 비활성 golden에 복사·검증 후 active 플립, 카운터 리셋.
void fw_guard_confirm_healthy(void);

/// 새 이미지 설치(rebootAndInstall) "직전"에 호출.
/// pending_commit=1, boot_attempts=0 으로 무장 → 다음 부팅이 probation이 됨.
void fw_guard_arm_pending(void);

/// 현재 부팅이 미확정(probation) 상태인지.
bool fw_guard_is_on_probation(void);

/// golden이 유효하게 보관돼 있는지(롤백 가능 여부).
bool fw_guard_golden_valid(void);

/// 워치독 급이기 — 메인 루프(emberAfTickCallback)에서 매번 호출.
/// tick이 멈추면(hang) ~64초 후 리셋 → probation이면 카운터 누적 → 롤백.
void fw_guard_feed_watchdog(void);

#endif // FW_GUARD_H
