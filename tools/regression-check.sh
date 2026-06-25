#!/bin/bash
# 회귀 기준선 게이트 — AOB 작업 전/후로 "현재 GREEN"이 유지되는지 확인.
#  (1) 유닛 4종(test_inline_hook / test_memory / test_reloc / test_ps_scan_darwin) = make test
#  (2) 런타임 스모크(--runtime): 주입 후 Lua 프로브 구동 로그 확인 절차(수동 게이트)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

echo "== [기준선] 유닛 테스트 (make test) =="
if make test; then
  echo "== [기준선] 유닛 4종 PASS =="
else
  echo "!! [기준선] 유닛 테스트 실패 — AOB 작업 전이라면 환경 점검, 후라면 회귀!"
  exit 1
fi

if [ "${1:-}" = "--runtime" ]; then
  LOG="$HOME/Library/Caches/ue4ss-mac-sdd/smoke-load.log"
  cat <<EOF

== [기준선] 런타임 스모크 게이트(수동) ==
  1) ./tools/stage-ue4ss-runtime.sh   # 컨테이너에 settings·Mods 스테이징
  2) ./tools/launch-ue4ss.sh          # DYLD 주입 (게임 창 뜨면 잠깐 후 Cmd+Q)
  3) 아래로 Lua 프로브 구동 증명:
     grep -c "\[Lua\] \[PmmLuaProbe\] main.lua executed" "$LOG"   # >=1 이면 GREEN
     grep -c "LoopAsync alive" "$LOG"                            # >=1 이면 GREEN
  통과 기준 = 두 grep 모두 >=1 (이전 세션 9/9 안정 상태와 동일).
EOF
fi
