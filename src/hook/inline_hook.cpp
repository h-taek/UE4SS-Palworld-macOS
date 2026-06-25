#include "hook/inline_hook.hpp"
#include "hook/arm64_reloc.hpp"
#include "darwin/memory.hpp"
#include "darwin/log.hpp"
#include <cstdint>
#include <cstring>
namespace mac {
static constexpr uint32_t LDR_X16_8 = 0x58000050u; // LDR X16,#8
static constexpr uint32_t BR_X16    = 0xD61F0200u; // BR X16

// addr로 가는 절대점프 16B(LDR X16,#8; BR X16; <addr>)를 buf에 기록
static void abs_jump(uint8_t* buf, uint64_t addr) {
    memcpy(buf+0, &LDR_X16_8, 4); memcpy(buf+4, &BR_X16, 4); memcpy(buf+8, &addr, 8);
}
Hook install_hook(void* target, void* hook_fn) {
    Hook h; h.target = target;
    // 0) 원본 prologue 저장 (I-1: rollback용 — 패치 전에 저장)
    memcpy(h.original, target, 16);
    uint8_t original_copy[16];
    memcpy(original_copy, target, 16);

    // 1) 트램펄린 = (재배치된 원본 prologue) + (target+stolen 로 복귀하는 절대점프)
    //    stolen   = 원본에서 소비한 바이트(복귀 주소·target 패치 길이 기준)
    //    emitted  = 트램펄린에 써낸 바이트(PC-상대 분기를 절대 베니어로 보정하면 stolen보다 큼)
    uint8_t tbuf[64] = {0};
    size_t emitted = 0;
    size_t stolen = relocate_prologue((const uint8_t*)target, tbuf, sizeof(tbuf), 16, &emitted);
    if (stolen == 0) { mac::logf("install_hook: relocate failed @%p", target); return h; }
    // I-2: 트램펄린 버퍼 오버플로우 방어(복귀점프는 emitted 뒤에 붙음)
    if (emitted + 16 > sizeof(tbuf)) {
        mac::logf("install_hook: emitted(%zu)+16 exceeds tbuf capacity @%p", emitted, target);
        return h;
    }
    abs_jump(tbuf + emitted, (uint64_t)((uint8_t*)target + stolen)); // 위치=emitted, 복귀=target+stolen
    void* tramp = jit_alloc(tbuf, emitted + 16);
    if (!tramp) return h;
    h.trampoline = tramp;
    // 2) target 앞 16B를 hook_fn 절대점프로 패치(서명 코드 → COW)
    //    ★임계영역: 다른 스레드를 정지(ScopedThreadFreeze)한 채 RW→memcpy→RX 를 끝낸다.
    //    이 창에서 게임 스레드가 패치 중인 코드를 실행하면 BUS 오류(W^X 과도기 레이스)이므로
    //    정지로 막는다. 정지 중에는 로그/malloc 금지 → 결과만 캡처하고 해제 후 로깅.
    uint8_t patch[16]; abs_jump(patch, (uint64_t)hook_fn);
    enum { P_OK, P_WFAIL, P_XFAIL } pstatus = P_WFAIL;
    {
        ScopedThreadFreeze freeze; // 자신 제외 전 스레드 정지, 스코프 종료 시 resume
        if (code_protect_raw(target, 16, /*writable=*/true)) {
            memcpy(target, patch, 16);
            if (code_protect_raw(target, 16, /*writable=*/false)) {
                pstatus = P_OK;
            } else {
                memcpy(target, original_copy, 16);            // 반패치 롤백
                code_protect_raw(target, 16, /*writable=*/false);
                pstatus = P_XFAIL;
            }
        }
    } // ← 모든 스레드 resume
    if (pstatus == P_WFAIL) { mac::logf("install_hook: protect(W) failed @%p", target); return h; }
    if (pstatus == P_XFAIL) { mac::logf("install_hook: protect(X) failed after patch, rolled back @%p", target); return h; }
    h.ok = true;
    mac::logf("install_hook OK: target=%p tramp=%p stolen=%zu emitted=%zu%s", target, tramp, stolen, emitted, (emitted != stolen ? " [VENEER]" : ""));
    return h;
}
}
