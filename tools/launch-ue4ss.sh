#!/bin/bash
# P2 연기 테스트: 진짜 libUE4SS.dylib(두뇌)를 Palworld에 주입해 "로드 중 크래시
# 없이 올라오나"만 확인한다. 게임 번들은 수정하지 않음(DYLD_INSERT만).
# 성공 신호 = 게임 창이 정상적으로 뜸. 실패 = 로드 즉시 크래시/조용히 종료.
# 전체 dyld 로드 로그는 $LOG에 저장(분석용).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DYLIB="$ROOT/Binaries/Game__Shipping__Mac/UE4SS/libUE4SS.dylib"
GAME="/Applications/Palworld.app/Contents/MacOS/Palworld"
LOG="$HOME/Library/Caches/ue4ss-mac-sdd/smoke-load.log"
mkdir -p "$(dirname "$LOG")"
[ -f "$DYLIB" ] || { echo "dylib 없음 — 빌드 먼저: $DYLIB"; exit 1; }
[ -x "$GAME" ] || { echo "Palworld 실행파일 없음: $GAME"; exit 1; }
codesign -s - --force --timestamp=none "$DYLIB"
echo "== libUE4SS.dylib 주입 + dyld 로드추적, 로그 -> $LOG =="
echo "== 게임 창이 뜨면 = 로드 성공. 잠깐 확인 후 Cmd+Q(또는 창 닫기)로 종료하세요. =="
echo "== 창이 안 뜨고 바로 꺼지면 = 로드 실패(로그/크래시리포트 분석). =="
DYLD_PRINT_LIBRARIES=1 DYLD_INSERT_LIBRARIES="$DYLIB" "$GAME" > "$LOG" 2>&1
echo "== 게임 종료됨(exit=$?). 로그: $LOG =="
