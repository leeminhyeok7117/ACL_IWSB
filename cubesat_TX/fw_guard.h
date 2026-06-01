/***************************************************************************//**
 * @file
 * @brief fw_guard.h — TX(코디네이터) 펌웨어 롤백 가드 (A안, 단일 슬롯 부트로더)
 *
 * [RX fw_guard 와의 결정적 차이 — 반드시 이해할 것]
 *   TX의 SLOT0는 두 용도로 공유된다:
 *     (1) TX 자체 펌웨어 OTA staging (target=0)
 *     (2) RX 펌웨어 OTA staging → 각 RX로 배포 (target=1~4)
 *   따라서 "아무 healthy 부팅에서나 SLOT0를 golden으로 캡처"하면, 직전에
 *   RX 이미지를 staging한 SLOT0를 TX golden으로 박제할 수 있다.
 *   → 롤백 시 TX에 RX 펌웨어가 설치되어 코디네이터가 brick 된다.
 *
 *   [방어] TX는 golden을 **pending_commit(=TX 자체 OTA 직후)일 때만** 캡처한다.
 *          (FW_GUARD_CAPTURE_ON_BOOTSTRAP = 0). RX는 SLOT0가 항상 자기 이미지
 *          이므로 부트스트랩 캡처가 안전(=1)하지만 TX는 절대 안 됨.
 *
 *   [운용 제약] TX 자체 OTA 재부팅 후 golden 캡처(~15초 헬스 윈도우)가 끝나기
 *          전에는 RX OTA(SLOT0 erase/stream)를 시작하지 말 것.
 *          그 사이 SLOT0가 RX 이미지로 덮이면 golden이 오염된다.
 *
 * [무접근 위성 전제]
 *   - J-Link/CLI 불가, OTA만 가능. TX가 brick 되면 전 네트워크가 마비되고
 *     추가 OTA 경로 자체가 사라지므로(단일 장애점) TX 롤백 가드가 특히 중요.
 ******************************************************************************/
#ifndef FW_GUARD_H
#define FW_GUARD_H

#include <stdint.h>
#include <stdbool.h>

// ─────────────────────────────────────────────────────────────────────────────
//  설정값
// ─────────────────────────────────────────────────────────────────────────────
#define FW_GUARD_NVM3_KEY          0x0003UL    // TX: 0x0002=slave_table, 0x0003=guard
#define FW_GUARD_SLOT0_ID          0U
#define FW_GUARD_MAX_BOOT_ATTEMPTS 5U          // 이 횟수 미확정 부팅 → 롤백

// 부트스트랩(최초 golden 없음) 시 자동 캡처 여부.
//   TX는 SLOT0가 RX 이미지일 수 있으므로 0(금지). pending_commit 일 때만 캡처.
#define FW_GUARD_CAPTURE_ON_BOOTSTRAP  0

// 외부 SPI 플래시 golden 영역 (SLOT0: 0x84000~0xF4000 와 겹치지 않게 높은 주소)
//   외부 플래시 32Mbit(4MB) 확인됨 → 2.0MB 경계까지 사용. partSize 런타임 검증으로
//   안 맞으면 롤백 자동 비활성(fail-safe).
#define FW_GUARD_GOLDEN_A_ADDR     0x00180000UL  // 1.5 MB
#define FW_GUARD_GOLDEN_B_ADDR     0x001C0000UL  // 1.75 MB
#define FW_GUARD_GOLDEN_SIZE       0x00040000UL  // 256 KB (이미지 ~155KB 여유)

// ─────────────────────────────────────────────────────────────────────────────
//  API (RX fw_guard 와 시그니처 동일)
// ─────────────────────────────────────────────────────────────────────────────

/// 부팅 직후 "최우선"으로 호출 (emberAfInitCallback 최상단, bootloader_init 이후).
/// pending_commit 상태면 boot_attempts 증가, 임계 도달 시 즉시 롤백(복귀 안 함).
void fw_guard_init(void);

/// TX 자체 펌웨어가 정상 동작(스택 up 등) 확인됐을 때 1회 호출.
/// pending_commit 일 때만 SLOT0(=방금 설치한 TX 이미지)를 golden에 캡처.
void fw_guard_confirm_healthy(void);

/// TX 자체 이미지 설치(rebootAndInstall) "직전"에 호출.
/// pending_commit=1, boot_attempts=0 으로 무장 → 다음 부팅이 probation.
void fw_guard_arm_pending(void);

/// 현재 부팅이 미확정(probation) 상태인지.
bool fw_guard_is_on_probation(void);

/// golden이 유효하게 보관돼 있는지(롤백 가능 여부).
bool fw_guard_golden_valid(void);

/// 워치독 급이기 — 메인 루프(emberAfTickCallback)에서 매번 호출.
void fw_guard_feed_watchdog(void);

#endif // FW_GUARD_H
