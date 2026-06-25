-- PmmLuaProbe — 네이티브 arm64 빌드에서 Lua 모드가 end-to-end 실행되는지 증명하는 최소 프로브.
-- 의도적으로 엔진 후킹/오프셋/FName에 의존하지 않는다: top-level print + UE4SS Lua async 루프만 사용.
-- (엔진-준비 콜백 검증은 오프셋 리스크가 있어 Plan 05에서 별도로 다룬다.)

print("[PmmLuaProbe] main.lua executed (top-level)\n")

-- step3 반응형 실증: PLSF 후킹(디스패처)이 살아 있으면 이 콜백 중 하나가 발화한다.
-- 게임플레이 진입 부담을 줄이려 여러 트리거를 걸고 먼저 오는 걸로 1회 FIRED:
--   ReceiveBeginPlay — 레벨/월드(메뉴 레벨 포함) 로드 시 발화 (가장 이른 후보)
--   ReceiveTick      — 월드에서 액터 틱마다 발화 (게임플레이 중)
--   ClientRestart    — 플레이어컨트롤러 리스타트(플레이 진입) 시 1회. CheatManagerEnablerMod 검증 함수.
local fired = false
local function announce(which)
    if not fired then
        fired = true
        print(string.format("[PmmLuaProbe] RegisterHook FIRED (%s) — reactive mode LIVE\n", which))
    end
end

local targets = {
    "/Script/Engine.Actor:ReceiveBeginPlay",
    "/Script/Engine.Actor:ReceiveTick",
    "/Script/Engine.PlayerController:ClientRestart",
}
for _, fn in ipairs(targets) do
    local ok = pcall(function()
        return RegisterHook(fn, function(self) announce(fn) end)
    end)
    print(string.format("[PmmLuaProbe] RegisterHook setup %s ok=%s\n", fn, tostring(ok)))
end

local announced = false
LoopAsync(1000, function()
    if not announced then
        announced = true
        print("[PmmLuaProbe] LoopAsync alive — UE4SS Lua async thread running\n")
    end
    return announced  -- true 반환 시 루프 종료(한 번만 알림)
end)
