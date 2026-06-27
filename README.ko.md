# UE4SS — macOS 포팅 (Apple Silicon / arm64)

*[English README](README.md)*

[RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)의 **macOS arm64 네이티브 포팅**이다.
UE4SS의 "두뇌"(Unreal Engine 리플렉션 + Lua 스크립팅)는 재사용하고, "손발" — dylib 주입,
arm64 인라인 후킹, Mach-O 심볼 해석 — 만 Apple Silicon용으로 재작성했다.

WINE·Rosetta·에뮬레이션이 **아니다**. 네이티브 arm64 dylib을 네이티브 게임 프로세스에 직접 주입한다.

## 현재 단계

**능력 실증 — macOS 네이티브 Palworld(App Store, arm64, UE5.1)에서 end-to-end 검증 완료.**
Lua 모드가 **게임 `UFunction`을 후킹·호출해 실제 게임 상태를 바꾸는 것**까지 실증했다
(즉시 부화 + 자동 수령 데모). 전체 사슬이 macOS에서 동작한다: 주입 → 부트스트랩(엔진 앵커 6종)
→ 반응형 후킹 → `UObject` 리스너 → Lua 리플렉션 → `ProcessEvent` / `ProcessInternal` /
`ExecuteInGameThread`.

### 범위와 한계 (반드시 확인)

이것은 **샘플 1개(Palworld, n=1)** 에서 검증된 것이며, **아직 범용 UE5.1 로더가 아니다.**

- **부트스트랩이 Palworld에 과적합되어 있다.** 엔진 앵커를 심볼 우선 + Palworld에 맞춘 AOB
  폴백으로 찾으므로, 다른 UE5.1 macOS 게임은 그대로는 지원하지 않는다. 진짜 범용화(스캐너 업그레이드
  + arm64 시그니처 신규 작성 + 샘플 2개 이상)는 별도의 더 큰 과제다.
- **런타임 멤버 오프셋 표가 일부만 맥 검증됐다.** 3개 구조체(`UClass`·`UEnum`·`AGameModeBase`)는
  디스어셈블로 macOS 교정했지만, 나머지는 ABI 우연으로 맞는 리눅스 값이며 맥에서 미검증이다.
- **GUI 서브시스템 비활성**(`--ue4ssUI=None` 빌드).
- **macOS 키 입력 백엔드 미포팅** → `RegisterKeyBind` 사용 불가. Lua는 이벤트 후킹으로 트리거한다.
- **범위 밖:** 게임 탐색, 제품형 런처/주입 실행기, 모드 설치·활성화·업데이트, 모드 매니저 UI.
  이 레포에는 실험과 최소 배포용 스크립트만 둔다.

## 요구 사항

- Apple Silicon(arm64) macOS
- Xcode Command Line Tools
- [xmake](https://xmake.io/) (예: `brew install xmake`)
- capstone (`brew install capstone`)
- 런타임 테스트용 대상 게임(예: Palworld)

## 빌드

```sh
# 유닛 테스트 — 게임 불필요
make test

# 전체 UE4SS dylib (Shipping, arm64, GUI 비활성)
xmake f -P . -p macosx -a arm64 -m Game__Shipping__Mac --ue4ssUI=None -y
xmake build -P . -j4 UE4SS
```

## 실행 (Palworld 예시)

```sh
./tools/stage-ue4ss-runtime.sh   # 샌드박스 컨테이너에 설정·모드 스테이징
./tools/launch-ue4ss.sh          # DYLD_INSERT_LIBRARIES 주입 후 실행
./tools/tail-log.sh              # 런타임 로그 스트리밍
```

주입은 반드시 스크립트로 실행한다(Spotlight·더블클릭으로는 주입 안 됨).
UE4SS 런타임 데이터(로그·Mods·설정)는 이 레포가 아니라 게임 샌드박스 컨테이너에 위치한다.

### 게임 실행용 최소 배포물 구조

압축 배포 전 최소 패키지는 `tools/package-ue4ss-mac.sh`로 `dist/UE4SS_mac/`에 생성한다.

```sh
./tools/package-ue4ss-mac.sh --zip
```

사용자가 이 폴더를 받은 뒤 `launch-palworld.command`를 실행하면, 스크립트가 기본 preflight
(arm64 여부, dylib/설정/모드 파일 존재, Palworld 샌드박스 컨테이너, dylib 의존성, ad-hoc 서명)를
확인한다. 통과하면 패키지 안의 설정/모드 파일을 Palworld 샌드박스 컨테이너로 먼저 복사하고,
그 다음 `libUE4SS.dylib`만 게임 프로세스에 주입한다.

배포 폴더:

```text
dist/UE4SS_mac/
  launch-palworld.command
  libUE4SS.dylib
  UE4SS/
    UE4SS-settings.ini
    Mods/
      mods.txt
```

실행 시 스크립트가 만드는 Palworld 컨테이너 쪽 런타임 배치:

```text
~/Library/Containers/com.pocketpair.palworld.mac/Data/UE4SS/
  UE4SS-settings.ini
  Mods/
    mods.txt
```

`launch-palworld.command`가 주입하는 것은 패키지 안의 `libUE4SS.dylib` 하나다.

```sh
DYLD_INSERT_LIBRARIES="$PACKAGE_DIR/libUE4SS.dylib" \
  /Applications/Palworld.app/Contents/MacOS/Palworld
```

그 뒤 게임 프로세스 안에서 로드된 UE4SS가 `$HOME/UE4SS`를 작업 디렉터리로 잡고,
Palworld 샌드박스 환경에서는 이 경로가 위 컨테이너의 `Data/UE4SS`로 해석되어
`UE4SS-settings.ini`와 `Mods/`를 읽는다. 모드를 배포물에 포함하려면
`dist/UE4SS_mac/UE4SS/Mods/<ModName>/` 아래에 넣으면 된다.

## 모드

- **`PmmLuaProbe`** (포함) — 최소 Lua 런타임 스모크 테스트. 주입된 dylib 안에서 Lua VM이 로드·실행됨을
  증명한다. 최소 예제이자 인-레포 헬스체크다.
- **AutoHatch** (Palworld 전용 데모)는 여기가 아니라 **별도 모드 레포**에 둔다. 로컬 실게임 테스트 시
  스테이징 스크립트가 로컬 사본을 가리키게 한다:
  ```sh
  PMM_AUTOHATCH_DIR=/경로/AutoHatch ./tools/stage-ue4ss-runtime.sh
  ```
  (기본값 `../04_AutoHatch`, 없으면 건너뜀.)

Lua API와 모드 작성은 `docs/lua-api/`의 번들 레퍼런스를 참고한다. 들여온 UE4SS 스냅샷 기준 API이며,
맥 포팅은 API 표면을 바꾸지 않으므로 그대로 적용된다. `RegisterKeyBind` 등 맥 한정 런타임 예외는 위 한계 항목을 참조한다.

## 테스트 / 회귀

```sh
./tools/regression-check.sh      # 유닛 테스트 + 바이너리 심볼 스캔
```

## 구조

| 경로 | 내용 |
|------|------|
| `src/darwin/` | Mach-O 메모리 접근, 모듈/세그먼트 파싱, AOB 패턴 스캔, 로깅 |
| `src/hook/` | arm64 relocator(B/BL/thunk 해석)와 인라인 후킹 프리미티브 |
| `src/entry.cpp` | 주입 dylib 진입점 |
| `UE4SS/` | 업스트림 UE4SS 코어(리플렉션 + Lua), Darwin 진입점으로 빌드 |
| `deps/first/` | `UEPseudo`·`patternsleuth` 서브모듈 |
| `tools/` | 빌드·스테이징·실행·로그 스크립트 및 AOB 도출 도구 |
| `tests/` | C++ 유닛 테스트(AOB·후킹·재배치·메모리·심볼 스캔) |

## 라이선스

MIT — [RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS)(MIT) 포팅.
`UEPseudo`는 게이트 서브모듈(Epic EULA)이며 여기에 **vendoring 하지 않는다.**
