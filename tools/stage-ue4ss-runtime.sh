#!/usr/bin/env bash
# stage-ue4ss-runtime.sh
# 팔월드 샌드박스 컨테이너에 UE4SS 작업셋(설정·Mods)을 설치한다.
# 멱등: 이미 있으면 덮어쓰기(ini) 또는 스킵(디렉터리/mods.txt).
# 실행: ./tools/stage-ue4ss-runtime.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# 실제 홈(샌드박스 밖) — 스크립트 실행자 홈
REAL_HOME="$HOME"

# 팔월드 샌드박스 컨테이너 Data 경로
CONTAINER_DATA="$REAL_HOME/Library/Containers/com.pocketpair.palworld.mac/Data"
CONTAINER_UE4SS="$CONTAINER_DATA/UE4SS"
CONTAINER_MODS="$CONTAINER_UE4SS/Mods"

SRC_INI="$PROJECT_ROOT/assets/UE4SS-settings.ini"

echo "=== stage-ue4ss-runtime ==="
echo "컨테이너 경로: $CONTAINER_UE4SS"
echo ""

# 1. 컨테이너 존재 확인
if [ ! -d "$CONTAINER_DATA" ]; then
    echo "경고: 컨테이너가 없습니다 ($CONTAINER_DATA)"
    echo "팔월드를 최소 한 번 실행해 컨테이너를 생성한 뒤 다시 실행하세요."
    exit 1
fi

# 2. UE4SS 디렉터리 생성
mkdir -p "$CONTAINER_MODS"
echo "[OK] 디렉터리: $CONTAINER_MODS"

# 3. UE4SS-settings.ini 렌더링 (템플릿 → 평문 ini)
#    assets/UE4SS-settings.ini 는 빌드 때 OS별로 해석되는 "템플릿"이다:
#      - ${if os == "..."} ... ${endif}  : OS 조건 블록 (mac 분기 없음 → 통째 드롭)
#      - $VarName                         : 빌드 치환 변수 (UE4SS 표준 기본값으로 채움)
#    이걸 안 풀면 IniParser가 ${if} 구문에서 "Invalid state" 로 죽는다.
if [ -f "$SRC_INI" ]; then
    awk '/\$\{if/{skip=1} skip==0{print} /\$\{endif\}/{skip=0}' "$SRC_INI" \
      | sed -e 's/= \$EnableHotReloadSystem/= 0/' \
            -e 's/= \$IgnoreEngineAndCoreUObject/= 1/' \
            -e 's/= \$ConsoleEnabled/= 1/' \
            -e 's/= \$GuiConsoleVisible/= 0/' \
            -e 's/= \$MaxMemoryUsageDuringAssetLoading/= 80/' \
            -e 's/= \$GUIUFunctionCaller/= 0/' \
      > "$CONTAINER_UE4SS/UE4SS-settings.ini"
    echo "[OK] 렌더: UE4SS-settings.ini (template → plain) → $CONTAINER_UE4SS/UE4SS-settings.ini"
    if grep -qE '\$\{|= \$[A-Za-z]' "$CONTAINER_UE4SS/UE4SS-settings.ini"; then
        echo "[경고] 렌더 후에도 미해석 \${} 또는 \$변수 잔존 — 확인 필요:"
        grep -nE '\$\{|= \$[A-Za-z]' "$CONTAINER_UE4SS/UE4SS-settings.ini"
    fi
else
    echo "[SKIP] assets/UE4SS-settings.ini 없음 (빌드 산출물에서 복사해야 함)"
fi

# 4. mods.txt 생성 (없으면)
MODS_TXT="$CONTAINER_MODS/mods.txt"
if [ ! -f "$MODS_TXT" ]; then
    touch "$MODS_TXT"
    echo "[OK] 생성: $MODS_TXT"
else
    echo "[SKIP] 이미 존재: $MODS_TXT"
fi

# 4.5 Lua 프로브 모드 스테이징 (네이티브 Lua 구동 실증용 — Plan 04)
#     기본 모드(BPModLoaderMod 등)는 일부러 복사하지 않는다: 빈 mods.txt + 프로브 enabled.txt로
#     "오직 프로브만 기동"하게 격리하여, Lua VM 동작과 기본 모드의 엔진-후킹을 분리 검증한다.
PROBE_SRC="$PROJECT_ROOT/assets/Mods/PmmLuaProbe"
if [ -d "$PROBE_SRC" ]; then
    rm -rf "$CONTAINER_MODS/PmmLuaProbe"
    cp -R "$PROBE_SRC" "$CONTAINER_MODS/PmmLuaProbe"
    echo "[OK] 프로브 모드 스테이징: $CONTAINER_MODS/PmmLuaProbe"
else
    echo "[SKIP] 프로브 모드 없음: $PROBE_SRC"
fi

# 4.6 AutoHatch 모드 스테이징 (선택 — 외부 모드, 별도 레포)
#     AutoHatch는 Palworld 전용 테스트/데모 모드라 이 포팅 레포에는 포함하지 않는다(별도 모드 레포로 분리).
#     로컬 실게임 테스트 시에만 외부 경로에서 끌어온다. 기본값 = ../04_AutoHatch,
#     환경변수 PMM_AUTOHATCH_DIR 로 재지정 가능. 경로가 없으면 건너뛴다
#     (포팅 자체의 Lua 런타임 검증은 위 PmmLuaProbe 로 충분).
AUTOHATCH_SRC="${PMM_AUTOHATCH_DIR:-$PROJECT_ROOT/../04_AutoHatch}"
if [ -d "$AUTOHATCH_SRC" ]; then
    rm -rf "$CONTAINER_MODS/AutoHatch"
    cp -R "$AUTOHATCH_SRC" "$CONTAINER_MODS/AutoHatch"
    echo "[OK] AutoHatch 스테이징(외부): $AUTOHATCH_SRC → $CONTAINER_MODS/AutoHatch"
else
    echo "[SKIP] AutoHatch 외부 모드 없음(선택): $AUTOHATCH_SRC  (필요 시 PMM_AUTOHATCH_DIR 설정)"
fi

# 5. 결과 출력
echo ""
echo "=== 컨테이너 내 UE4SS 파일 목록 ==="
ls -R "$CONTAINER_UE4SS"
echo ""
echo "스테이징 완료."
echo ""
echo "다음 단계:"
echo "  ① 이 스크립트 1회 실행 완료 (지금)"
echo "  ② ./tools/launch-ue4ss.sh 로 게임 주입"
echo ""
echo "주입 후 stderr에서 기대 로그:"
echo "  [UE4SS-mac] worker start"
echo "  [UE4SS-mac] StderrDevice registered (log mirror active)"
echo "  [UE4SS] UE4SS version ..."
echo "  [UE4SS] ps_scan Found ..."
