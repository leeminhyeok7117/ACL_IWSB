#include "sl_component_catalog.h"
#include "sl_system_init.h"
#include "rail.h"
#include "rail_types.h"
#include "sl_rail_util_init.h"

// RAIL 핸들은 sl_rail_util_init 컴포넌트가 자동 생성
extern RAIL_Handle_t sl_rail_util_get_handle(sl_rail_util_handle_type_t handle);

void app_init(void)
{
  RAIL_Handle_t rail_handle = sl_rail_util_get_handle(SL_RAIL_UTIL_HANDLE_INST0);

  // 1. IDLE 상태로 전환 (CW 설정 전 필수)
  RAIL_Idle(rail_handle, RAIL_IDLE, true);

  // 2. TX Power 10 dBm 설정 (단위: deci-dBm = 100)
  RAIL_Status_t status = RAIL_SetTxPowerDbm(rail_handle, 100);
  (void)status;

  // 3. CW 연속 반송파 송신 시작
  //    RAIL_STREAM_CARRIER_WAVE = 고품질 CW (측정용 권장)
  RAIL_StartTxStream(rail_handle, 0, RAIL_STREAM_CARRIER_WAVE);
}


