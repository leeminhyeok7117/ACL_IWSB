/***************************************************************************//**
 * @file
 * @brief fw_guard.c — TX(코디네이터) 펌웨어 롤백 가드 (A안)
 *
 * RX fw_guard.c 와 동일한 A/B 핑퐁 + readback + NVM3 원자 플립 구조.
 * 유일한 차이: golden 캡처를 pending_commit(=TX 자체 OTA 직후)일 때로 제한
 *   (FW_GUARD_CAPTURE_ON_BOOTSTRAP=0). TX SLOT0가 RX 이미지일 수 있어
 *   부트스트랩 캡처를 금지한다. 자세한 이유는 fw_guard.h 헤더 주석 참조.
 *
 * 보수적 설계 원칙:
 *   - golden은 "검증된 직후" 에만 캡처(잘못된 이미지가 golden이 되지 않게).
 *   - golden 갱신은 A/B 핑퐁 + readback 검증 + NVM3 플립(원자적) → 전원안전.
 *   - 어떤 단계든 실패하면 "현재 상태 유지"가 기본(절대 더 나쁘게 만들지 않음).
 ******************************************************************************/

#include "fw_guard.h"
#include "app_log.h"
#include "btl_interface.h"
#include "btl_interface_storage.h"
#include "nvm3_default.h"
#include "em_wdog.h"
#include <string.h>

// -----------------------------------------------------------------------------
//                              Macros and Typedefs
// -----------------------------------------------------------------------------
#define FW_GUARD_MAGIC     0x46475231UL   // 'FGR1'
#define COPY_CHUNK         256U           // raw 복사 단위
#define GBL_END_TAG        0xFC0404FCUL

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint16_t boot_attempts;   // 미확정 부팅 누적
  uint8_t  pending_commit;  // 1=새 이미지 설치 후 건강확인 대기
  uint8_t  active_golden;   // 0=A, 1=B (유효 golden)
  uint32_t golden_size;     // active golden 바이트 수
  uint8_t  golden_valid;    // 1=golden 보관됨
  uint8_t  rsv[3];
} fw_guard_state_t;

// -----------------------------------------------------------------------------
//                                Static Variables
// -----------------------------------------------------------------------------
static fw_guard_state_t s_st;
static bool             s_rollback_enabled = false;   // partSize 검증 통과 시 true
static const uint32_t   s_golden_addr[2] = {
  FW_GUARD_GOLDEN_A_ADDR, FW_GUARD_GOLDEN_B_ADDR
};
static uint32_t         s_flash_page = 4096;          // getStorageInfo로 갱신

// -----------------------------------------------------------------------------
//                          Static Function Declarations
// -----------------------------------------------------------------------------
static void state_defaults(void);
static void state_load(void);
static bool state_save(void);
static bool validate_geometry(void);
static bool capture_golden(void);
static bool restore_golden_and_install(void);   // 복귀 안 함(성공 시)
static void wdog_setup(void);

// -----------------------------------------------------------------------------
//                          Public Function Definitions
// -----------------------------------------------------------------------------

void fw_guard_init(void)
{
  wdog_setup();      // 워치독 먼저 무장(이후 restore가 길어도 여유)
  state_load();
  s_rollback_enabled = validate_geometry();

  app_log_info("[GUARD] init: pending=%u attempts=%u golden=%u(valid=%u,size=%lu) rb=%u\n",
               s_st.pending_commit, s_st.boot_attempts, s_st.active_golden,
               s_st.golden_valid, (unsigned long)s_st.golden_size,
               s_rollback_enabled);

  if (!s_st.pending_commit) {
    return;   // 정상 부팅 (probation 아님)
  }

  // ─── probation 부팅: 시도 횟수 증가 ──────────────────────────────────────
  s_st.boot_attempts++;
  if (!state_save()) {
    app_log_error("[GUARD] NVM3 save failed during probation count.\n");
  }
  app_log_info("[GUARD] PROBATION boot %u/%u\n",
               s_st.boot_attempts, FW_GUARD_MAX_BOOT_ATTEMPTS);

  if (s_st.boot_attempts < FW_GUARD_MAX_BOOT_ATTEMPTS) {
    return;   // 아직 기회 남음 — 정상 부팅 계속 시도
  }

  // ─── 임계 도달 → 롤백 ────────────────────────────────────────────────────
  if (!s_rollback_enabled || !s_st.golden_valid) {
    app_log_error("[GUARD] Threshold reached but no valid golden — cannot roll back.\n");
    app_log_error("[GUARD] Staying on current image; counter frozen at threshold.\n");
    if (s_st.boot_attempts > FW_GUARD_MAX_BOOT_ATTEMPTS) {
      s_st.boot_attempts = FW_GUARD_MAX_BOOT_ATTEMPTS;
      (void)state_save();
    }
    return;
  }

  app_log_info("[GUARD] === ROLLBACK to golden %c ===\n",
               s_st.active_golden ? 'B' : 'A');

  // 무한 롤백루프 방지: golden에도 새 probation 기회(카운터 리셋)를 주고 설치.
  s_st.boot_attempts = 0;
  s_st.pending_commit = 1;
  (void)state_save();

  if (restore_golden_and_install()) {
    app_log_error("[GUARD] rebootAndInstall returned — install failed.\n");
  } else {
    app_log_error("[GUARD] Rollback restore failed. Will retry next boot.\n");
  }
}

void fw_guard_arm_pending(void)
{
  s_st.pending_commit = 1;
  s_st.boot_attempts  = 0;
  if (!state_save()) {
    app_log_error("[GUARD] arm_pending NVM3 save FAILED — abort install!\n");
  } else {
    app_log_info("[GUARD] Armed pending-commit. Next boot is probation.\n");
  }
}

void fw_guard_confirm_healthy(void)
{
  // 이미 확정 상태이고 golden도 있으면 할 일 없음.
  if (!s_st.pending_commit && s_st.golden_valid && s_st.boot_attempts == 0) {
    return;
  }

  // ★ TX 전용: golden 캡처는 pending_commit(=TX 자체 OTA 직후) 일 때만.
  //   부트스트랩(!golden_valid) 자동 캡처는 금지 — 그 시점 SLOT0는 RX 이미지일
  //   수 있어 TX golden을 오염시킨다. (FW_GUARD_CAPTURE_ON_BOOTSTRAP=0)
  bool should_capture = (s_st.pending_commit
                         || (FW_GUARD_CAPTURE_ON_BOOTSTRAP && !s_st.golden_valid));

  if (should_capture && s_rollback_enabled) {
    if (capture_golden()) {
      app_log_info("[GUARD] Golden captured → %c (size=%lu). Confirmed healthy.\n",
                   s_st.active_golden ? 'B' : 'A',
                   (unsigned long)s_st.golden_size);
    } else {
      app_log_error("[GUARD] Golden capture FAILED — keep previous golden.\n");
    }
  }

  s_st.pending_commit = 0;
  s_st.boot_attempts  = 0;
  if (!state_save()) {
    app_log_error("[GUARD] confirm_healthy NVM3 save FAILED.\n");
  } else {
    app_log_info("[GUARD] Healthy confirmed. Counters cleared.\n");
  }
}

bool fw_guard_is_on_probation(void) { return (s_st.pending_commit != 0); }
bool fw_guard_golden_valid(void)    { return (s_st.golden_valid != 0); }

// -----------------------------------------------------------------------------
//                          Static Function Definitions
// -----------------------------------------------------------------------------

static void state_defaults(void)
{
  memset(&s_st, 0, sizeof(s_st));
  s_st.magic = FW_GUARD_MAGIC;
}

static void state_load(void)
{
  Ecode_t ec = nvm3_readData(nvm3_defaultHandle, FW_GUARD_NVM3_KEY,
                             &s_st, sizeof(s_st));
  if (ec != ECODE_NVM3_OK || s_st.magic != FW_GUARD_MAGIC) {
    app_log_info("[GUARD] No valid state in NVM3 — defaults.\n");
    state_defaults();
    (void)state_save();
  }
}

static bool state_save(void)
{
  Ecode_t ec = nvm3_writeData(nvm3_defaultHandle, FW_GUARD_NVM3_KEY,
                              &s_st, sizeof(s_st));
  return (ec == ECODE_NVM3_OK);
}

/**************************************************************************//**
 * golden 영역이 실제 플래시 용량/페이지에 맞는지 런타임 검증.
 * 안 맞으면 롤백 비활성(fail-safe) — 절대 잘못된 주소에 쓰지 않음.
 *****************************************************************************/
static bool validate_geometry(void)
{
  BootloaderStorageInformation_t info;
  bootloader_getStorageInfo(&info);
  if (info.info == NULL) {
    app_log_error("[GUARD] No storage info — rollback DISABLED.\n");
    return false;
  }
  s_flash_page = info.info->pageSize ? info.info->pageSize : 4096;

  uint32_t part = info.info->partSize;
  uint32_t a_end = FW_GUARD_GOLDEN_A_ADDR + FW_GUARD_GOLDEN_SIZE;
  uint32_t b_end = FW_GUARD_GOLDEN_B_ADDR + FW_GUARD_GOLDEN_SIZE;

  if (part == 0 || a_end > part || b_end > part) {
    app_log_error("[GUARD] golden region exceeds flash (part=%lu) — rollback DISABLED.\n",
                  (unsigned long)part);
    return false;
  }
  if ((FW_GUARD_GOLDEN_A_ADDR % s_flash_page) != 0
      || (FW_GUARD_GOLDEN_B_ADDR % s_flash_page) != 0) {
    app_log_error("[GUARD] golden addr not page-aligned (page=%lu) — rollback DISABLED.\n",
                  (unsigned long)s_flash_page);
    return false;
  }
  return true;
}

/**************************************************************************//**
 * SLOT0(현재 TX 이미지)를 "비활성" golden 영역에 복사·검증한 뒤 active 플립.
 *****************************************************************************/
static bool capture_golden(void)
{
  // 0) 현재 SLOT0 이미지가 유효한 GBL인지 검증(쓰레기를 golden 삼지 않음)
  //    ※ verifyImage는 GBL 무결성만 확인하며 TX/RX 이미지를 구분하지 못한다.
  //      그래서 호출 타이밍(pending_commit 직후)으로 TX 이미지임을 보장한다.
  if (bootloader_verifyImage(FW_GUARD_SLOT0_ID, NULL) != BOOTLOADER_OK) {
    app_log_error("[GUARD] SLOT0 verify failed — skip golden capture.\n");
    return false;
  }

  BootloaderStorageSlot_t slot;
  if (bootloader_getStorageSlotInfo(FW_GUARD_SLOT0_ID, &slot) != BOOTLOADER_OK) {
    app_log_error("[GUARD] getStorageSlotInfo failed.\n");
    return false;
  }

  // GBL 크기: END 태그 스캔으로 정확히 산출
  uint32_t img_size = 0;
  {
    uint8_t  buf[8];
    uint32_t off = 0;
    while (off + 8 <= slot.length) {
      if (bootloader_readStorage(FW_GUARD_SLOT0_ID, off, buf, 8) != BOOTLOADER_OK) {
        img_size = 0; break;
      }
      uint32_t tag = (uint32_t)buf[0] | ((uint32_t)buf[1]<<8)
                   | ((uint32_t)buf[2]<<16) | ((uint32_t)buf[3]<<24);
      uint32_t len = (uint32_t)buf[4] | ((uint32_t)buf[5]<<8)
                   | ((uint32_t)buf[6]<<16) | ((uint32_t)buf[7]<<24);
      if (len > slot.length) { img_size = 0; break; }
      off += 8 + len;
      if (tag == GBL_END_TAG) { img_size = off; break; }
    }
  }
  if (img_size == 0 || img_size > FW_GUARD_GOLDEN_SIZE) {
    app_log_error("[GUARD] bad GBL size %lu (max %lu) — skip capture.\n",
                  (unsigned long)img_size, (unsigned long)FW_GUARD_GOLDEN_SIZE);
    return false;
  }

  uint8_t  inactive = s_st.active_golden ? 0 : 1;
  uint32_t dst = s_golden_addr[inactive];

  if (bootloader_eraseRawStorage(dst, FW_GUARD_GOLDEN_SIZE) != BOOTLOADER_OK) {
    app_log_error("[GUARD] golden erase failed @0x%lX.\n", (unsigned long)dst);
    return false;
  }

  uint32_t copy_size = (img_size + 3U) & ~3U;
  uint8_t  rbuf[COPY_CHUNK];
  uint8_t  vbuf[COPY_CHUNK];
  for (uint32_t o = 0; o < copy_size; o += COPY_CHUNK) {
    uint32_t n = (copy_size - o > COPY_CHUNK) ? COPY_CHUNK : (copy_size - o);
    if (bootloader_readStorage(FW_GUARD_SLOT0_ID, o, rbuf, n) != BOOTLOADER_OK) {
      app_log_error("[GUARD] SLOT0 read fail @%lu.\n", (unsigned long)o);
      return false;
    }
    if (bootloader_writeRawStorage(dst + o, rbuf, n) != BOOTLOADER_OK) {
      app_log_error("[GUARD] golden write fail @0x%lX.\n", (unsigned long)(dst + o));
      return false;
    }
    if (bootloader_readRawStorage(dst + o, vbuf, n) != BOOTLOADER_OK
        || memcmp(rbuf, vbuf, n) != 0) {
      app_log_error("[GUARD] golden readback mismatch @0x%lX.\n",
                    (unsigned long)(dst + o));
      return false;
    }
  }

  // NVM3 active 플립 (원자적 커밋)
  s_st.active_golden = inactive;
  s_st.golden_size   = img_size;
  s_st.golden_valid  = 1;
  return state_save();
}

/**************************************************************************//**
 * active golden → SLOT0 복원 후 설치. 성공 시 rebootAndInstall로 복귀 안 함.
 *****************************************************************************/
static bool restore_golden_and_install(void)
{
  uint32_t src  = s_golden_addr[s_st.active_golden];
  uint32_t size = s_st.golden_size;
  if (size == 0 || size > FW_GUARD_GOLDEN_SIZE) {
    app_log_error("[GUARD] invalid golden size %lu.\n", (unsigned long)size);
    return false;
  }

  if (bootloader_eraseStorageSlot(FW_GUARD_SLOT0_ID) != BOOTLOADER_OK) {
    app_log_error("[GUARD] SLOT0 erase failed.\n");
    return false;
  }

  uint32_t copy_size = (size + 3U) & ~3U;
  uint8_t rbuf[COPY_CHUNK];
  uint8_t vbuf[COPY_CHUNK];
  for (uint32_t o = 0; o < copy_size; o += COPY_CHUNK) {
    uint32_t n = (copy_size - o > COPY_CHUNK) ? COPY_CHUNK : (copy_size - o);
    if (bootloader_readRawStorage(src + o, rbuf, n) != BOOTLOADER_OK) {
      app_log_error("[GUARD] golden read fail @0x%lX.\n", (unsigned long)(src + o));
      return false;
    }
    if (bootloader_writeStorage(FW_GUARD_SLOT0_ID, o, rbuf, n) != BOOTLOADER_OK) {
      app_log_error("[GUARD] SLOT0 write fail @%lu.\n", (unsigned long)o);
      return false;
    }
    if (bootloader_readStorage(FW_GUARD_SLOT0_ID, o, vbuf, n) != BOOTLOADER_OK
        || memcmp(rbuf, vbuf, n) != 0) {
      app_log_error("[GUARD] SLOT0 readback mismatch @%lu.\n", (unsigned long)o);
      return false;
    }
  }

  if (bootloader_verifyImage(FW_GUARD_SLOT0_ID, NULL) != BOOTLOADER_OK) {
    app_log_error("[GUARD] restored golden verify FAILED.\n");
    return false;
  }
  if (bootloader_setImageToBootload(FW_GUARD_SLOT0_ID) != BOOTLOADER_OK) {
    app_log_error("[GUARD] setImageToBootload failed.\n");
    return false;
  }

  app_log_info("[GUARD] golden restored to SLOT0, installing...\n");
  bootloader_rebootAndInstall();   // 복귀 안 함
  return true;
}

/**************************************************************************//**
 * 워치독 설정 — ULFRCO(~1kHz), ~128초 타임아웃.
 *   TX는 코디네이터라 hang 시 전 RX가 고아가 됨 → 워치독 자동 복구가 중요.
 *
 * ★ 발사 전 지상 보드에서 반드시 검증(디버거 분리 상태에서!):
 *    (1) 의도적 무한루프 → ~128초 후 리셋되는지
 *    (2) 정상 OTA(erase 포함) 중 리셋되지 "않는지"
 *   ※ J-Link 디버거 연결 시 WDOG는 정지(debugRun=false 기본)하므로
 *      디버거를 붙인 채로는 발동하지 않는다 — 단독 전원으로 검증할 것.
 *****************************************************************************/
static void wdog_setup(void)
{
#if defined(_SILICON_LABS_32B_SERIES_1)
  WDOG_Init_TypeDef init = WDOG_INIT_DEFAULT;
  init.enable  = true;
  init.em2Run  = true;                 // EM2에서도 동작
  init.em3Run  = true;
  init.clkSel  = wdogClkSelULFRCO;     // 항상 가용한 1kHz 내부 클록
  init.perSel  = wdogPeriod_128k;      // ~128s (131073 / 1024Hz)
  WDOGn_Init(WDOG0, &init);
  WDOGn_Feed(WDOG0);
#endif
}

void fw_guard_feed_watchdog(void)
{
#if defined(_SILICON_LABS_32B_SERIES_1)
  WDOGn_Feed(WDOG0);
#endif
}
