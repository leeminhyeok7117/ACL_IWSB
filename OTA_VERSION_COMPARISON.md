# CubeSat OTA 펌웨어 ver1 → ver2 비교 기록

- **ver1 (과거)**: `~/efr32fg12 복사본/firmware 1/cubesat_{RX,TX}_AUTO+1:N/`
- **ver2 (현재)**: `~/SimplicityStudio/v5_workspace/cubesat_{RX,TX}/`
- 플랫폼: EFR32FG12P433F1024GL125, BRD4253A, Silicon Labs Connect (Flex SDK 4.5.0)
- 구성: 1 TX(Master/Coordinator/OTA Server) + 4 RX(Slave/End Device/OTA Client)
- 고정 상수(양 버전 공통): Channel 0, PAN 0xFFFF, 보안키 AES-128, 외부 SPI 플래시 SLOT0=0x84000(448KB)

---

## 1. ver1 개요 — "TDMA push 기반 자동 OTA"

ver1은 **각 RX가 스스로 주기적으로 자기 상태를 TX에 밀어 넣는(push)** 방식이다. TX는 받기만 하고 폴링하지 않는다.

### ver1 RX 기능
- **TDMA 슬롯 송신**: device_id로 슬롯 오프셋 계산(`(id-1)*SLOT`), 사이클 30s. RF 충돌 회피 목적.
  - ID 알림(`0xA0`+id, 2B), FW버전 리포트(`0xF2`+version), Heartbeat(주기 재알림)를 각자 슬롯에 전송.
- **FW 버전 리포트 재시도**: 5회 실패 시 `NVIC_SystemReset()` (TX 못 찾으면 RX 스스로 리셋).
- **Join**: 미가입이면 tick에서 5초마다 `try_rejoin()`. `emberGetDefaultChannel()`, PAN 0xFFFF, EMBER_STAR_END_DEVICE.
- **`emberAfMessageSentCallback`**: 실패해도 **로그만**. (soft-rejoin 없음 ← ver2와 결정적 차이)
- **OTA 클라이언트**: NewIncomingImage → IncomingImageSegment(슬롯0 직접 write) → DownloadComplete(100ms 후 verifyImage+setImageToBootload) → IncomingRequestBootload(검증됐으면 rebootAndInstall).
- **OTA PREPARE 처리**: 메시지 콜백에서 **슬롯 erase를 직접 수행(수백ms 블로킹)** 후 ACK.
- ❌ 롤백/golden 없음, ❌ 워치독 없음, ❌ 폴링 없음, ❌ sleep 제어 없음.

### ver1 TX 기능
- **AP_OBC I2C 프로토콜**: `0x01 PREPARE`(device_id+size+tag, 슬롯 erase) / `0x02 WRITE_CHUNK` / `0x03 START_OTA` / `0x04 STATUS`.
- **BTN0로 OTA 시작** (지상/우주 모드). CLI `ota_target`로 타겟 선택.
- **slave_table**: device_id↔node_id, ID_ANNOUNCE 수신 시 등록. **RAM에만 저장(NVM3 영속화 없음)** → TX 리셋 시 소실.
- **OTA 상태머신**: PREPARE→배포→bootload→**OTA_VERIFYING**(RX 재join + FW버전 리포트 수신으로 성공 확인)→완료.
- **OTA 완료 후 TX가 20초 뒤 자동 리셋**(`tx_reset_cb`→`NVIC_SystemReset`). RX 재join 유도 목적.
- Slave prepare 타임아웃 시 **무한 재시도**(최대 횟수 제한 없음).
- ❌ 롤백/golden 없음, ❌ 워치독 없음, ❌ 폴링 없음, ❌ slave_table NVM3 없음.

### ver1의 구조적 약점 (ver2가 해결하려는 동기)
1. TX가 OTA 후 자기를 리셋 → 코디네이터가 잠시 사라지고 전 네트워크 흔들림.
2. slave_table이 RAM뿐 → TX 리셋되면 슬레이브 목록 전부 소실.
3. 불량 펌웨어 OTA 시 복구 수단 없음(롤백 부재) → brick.
4. hang에 대한 워치독 없음.
5. RX가 TDMA로 push만 함 → TX가 능동적으로 상태/생존을 확인하는 수단 없음.
6. OTA PREPARE erase가 콜백 블로킹.

---

## 2. ver2 업그레이드 (ver1 대비)

### A. 🔴 롤백 가드 (fw_guard) — 가장 큰 추가 (ver1엔 전무)
- 외부 SPI 플래시에 **golden A/B 핑퐁**으로 "직전 정상 펌웨어" 보관(각 256KB, A=0x180000/B=0x1C0000).
- **5중 방어**: ①verify-before-install ②부팅 카운터(NVM3 0x0003) ③헬스 타임아웃 self-reset(probation 한정) ④워치독(WDOG0 ULFRCO ~128s) ⑤A/B golden.
- **상태머신**: 설치 직전 `arm_pending`(probation 무장) → 부팅마다 카운터++ → 5회 미확정 → golden 복원·설치(자동 롤백).
- **헬스 확정**: RX는 network_joined 15s 유지 시, TX는 emberStackIsUp 15s 시 → golden 캡처(A↔B 토글) + 카운터 리셋.
- **TX 특이점**: SLOT0가 TX-self/RX-staging 공유 → `CAPTURE_ON_BOOTSTRAP=0`(pending_commit일 때만 캡처). RX는 =1(부트스트랩 캡처 허용).
- 전원안전: golden 갱신은 비활성 슬롯 write → readback → NVM3 active 플립(원자적).

### B. 통신 모델 전환: TDMA push → **폴링(polling)**
- ver2는 **TX가 5초 사이클로 각 RX를 폴링**(`POLL_REQUEST`), RX가 `POLL_RESPONSE` 응답(device_id+fw버전+**golden 텔레메트리**).
- 폴 사이클 완료 = `count_pollable_slaves()` 기준(등록+node_id 아는 수). `Cycle done (N/N)`.
- ver1의 TDMA 슬롯/FW리포트/heartbeat push 모델 폐기 → TX가 능동적으로 생존·상태 파악.
- **golden 텔레메트리**: 폴 응답 4번째 바이트(golden_valid/probation/슬롯) → TX 로그 `golden=Y(A/B) prob=N`. 로그 없는 RX의 롤백 상태를 TX에서 원격 확인.

### C. Join 안정화 (ver1보다 대폭 강화)
- **init에서 do_join() 직접 호출 금지** — `emberAfInitCallback`은 첫 tick 이전이라 PHY 미보정 → `emberJoinNetwork`가 항상 0x8E(PHY_CALIBRATING) 실패. ver2는 join을 tick에 위임(~300ms 후 첫 시도).
- **매 부팅 `emberResetNetworkState()` + fresh join**(구 위성 호환).
- **지수 백오프** 2s→20s 상한.
- **stale JOINED 복구**: soft-rejoin 후 스택이 옛 부모에 EMBER_JOINED_NETWORK로 남는 버그 → do_join에서 강제 reset.
- **5분 연속 미가입 → 자가 리셋**(join 실패는 hang 아니라 워치독이 못 잡으므로 별도 복구 경로).
- **stale coordinator 복구**: NodeID==0x0000(잘못된 코디네이터 상태) 감지 → 네트워크 비우고 재부팅.

### D. TX 복구/영속성
- **slave_table NVM3 영속화(0x0002)** — TX 리셋/전원OFF 후 device_id 목록 자동 복원(node_id는 rejoin 후 갱신). ver1은 소실.
- **OTA 후 TX 자가 리셋 제거** — 코디네이터 유지(단일 장애점 안정). 이미지는 FW_READY 유지 → 같은 이미지를 다른 RX에 재배포 가능.
- **permit-join 주기 재확인(30s)** — resume 타이밍 갭 보험.
- **워치독 하트비트(2s 주기 타이머)** — 폴 응답 대기 등 어떤 상태에서도 tick 보장(오프라인 RX 폴링 중 워치독 스푸리어스 리셋 방지).

### E. OTA 신뢰성 (ver2에서 버그 수정)
- **전송 간격 100ms→50ms** — ver1 기본 100ms는 160KB가 ~190s 걸려 타임아웃(180s) 직전 90%서 끊김. 50ms로 ~100s.
- **앱 타임아웃 추가**: 배포 300s, bootload-wait 30s, prepare 최대 3회(ver1은 무한 재시도).
- **OTA 중 자가 교란 억제(`ota_download_active`)** — 다운로드 중 soft-rejoin 금지 + ID_ANNOUNCE 억제 + golden캡처 보류. (ver1은 soft-rejoin 자체가 없어 이 문제 없었음 → ver2가 soft-rejoin을 추가하며 생긴 부작용을 다시 막은 것.)
- **OTA PREPARE erase를 tick으로 defer**(ver1은 콜백 블로킹).
- **per-chunk readback 검증**(TX FW 스트리밍 시).

### F. 전력/sleep
- **`block_sleep_while_unjoined`** — 미가입 동안 EM1 요구 + 1초 주기 wakeup 타이머(RTCC)로 tick 보장. 가입 후 EM2 허용(저전력 복귀). ver1엔 없음.

### G. AP_OBC I2C 프로토콜 변경 (ver1과 비호환)
- ver2: `0x01 data` / `0x02 FW stream`([cc][pkt_num:2 LE][len:1][data], 마지막=0xFFFF 센티넬) / `0x03 erase` / `0x04 START [target]`(0=TX자체, 1~4=RX).
- **TX 자체 OTA(target=0)** 추가 — ver1엔 없던 기능.
- I2C 슬레이브 0x71, SDA=PC10/LOC16, SCL=PC11/LOC14. IRQ 핸들러 검증본(드레인+SSTOP 지연).

### H. device_id 결정 (ver2 강화)
- 우선순위: **FALLBACK(1~4, 권위적·NVM3 덮어씀)** > NVM3(0x0001) > EUI64 맵. ver1보다 NVM3 오염 자가교정 강함.
- ID_ANNOUNCE 2B→**10B**(device_id + EUI64 8B) → TX 로그로 EUI64 수집(맵 채우기용).

### I. 제거/대체된 것
- BTN0 지상모드 → 커스텀 보드 버튼 없음 → CLI `ota_start`(테스트용) / I2C START로 대체.
- OTA_VERIFYING + FW버전리포트 검증 모델 → 폴링 + golden 텔레메트리로 대체.
- TX 20초 자가 리셋 → 제거.

---

## 3. 한 줄 요약
> **ver1** = 동작하는 자동 OTA(TDMA push), 단 롤백·워치독·TX복구·폴링 없음 → 불량 펌웨어/장애에 취약.
> **ver2** = ver1에 **롤백 가드 + 폴링 + NVM3 영속화 + 워치독 + Join안정화 + OTA신뢰성 수정**을 더한 "무접근 위성 운용" 지향 버전.

## 4. 알아둘 함정 (ver2 운용)
- 첫 OTA는 golden 없음 = 미보호(brick 위험) → 지상 검증 필수.
- TX/RX 교차 설치 방지 장치 없음(verifyImage가 역할 구분 못 함) → 운용 규칙으로 회피.
- TX eval(1MB)은 golden 영역 초과로 롤백 DISABLED(RX 4MB는 정상).
- ver2 펌웨어가 OTA를 "받을 때"만 soft-rejoin 문제 발생 → 수정본은 J-Link로 first 적재 권장. (구 ver1→ver2 첫 OTA는 ver1에 soft-rejoin이 없어 정상.)
