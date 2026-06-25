#include "darwin/log.hpp"
#include "darwin/memory.hpp"
#include "darwin/module.hpp"
#include "hook/inline_hook.hpp"
#include <unistd.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdint>

// ── MVP 엔진 심볼 (nm 링크타임 VM주소; 런타임 = +slide). 바이너리 미stripped → 패턴 스캔 불필요. ──
namespace sym {
constexpr uintptr_t GUObjectArray   = 0x108e03b10;
constexpr uintptr_t GMalloc         = 0x108dd5490;
constexpr uintptr_t ProcessEvent    = 0x1039d0aa4; // UObject::ProcessEvent(UFunction*, void*)
constexpr uintptr_t FName_ToString  = 0x103803844; // FName::ToString(FString&) const
constexpr uintptr_t FName_ctor      = 0x1038026a8; // FName::FName(char const*, EFindName)
constexpr uintptr_t StaticConstruct = 0x1039f8668; // StaticConstructObject_Internal(...)
}

// ── UE5.1 레이아웃 (런타임 검증 대상) ──
struct FNameT   { uint32_t comparison_index; uint32_t number; };          // 8B
struct FStringT { char16_t* data; int32_t num; int32_t max; };            // TArray<TCHAR>, TCHAR=char16_t
static constexpr int UObject_NamePrivate = 0x18;                          // UObject::NamePrivate(FName)

using FName_ToString_fn = void (*)(const FNameT*, FStringT*);
using PE_fn             = void (*)(void* self, void* fn, void* params);

static FName_ToString_fn g_ToString = nullptr;
static PE_fn             g_pe_tramp = nullptr;

// FName → ASCII (로그용; 비-ASCII는 무시). FString 출력버퍼는 스파이크라 누수 허용.
static void name_to_buf(const FNameT* n, char* out, int cap) {
    FStringT s{};
    g_ToString(n, &s);
    int i = 0;
    if (s.data) for (; i < cap - 1 && i < s.num && s.data[i]; i++) out[i] = (char)s.data[i];
    out[i] = 0;
}

// ProcessEvent 후킹 콜백 — 엔진 가동 후 발화. 여기서 UObject 필드 + FName::ToString 검증.
static void pe_hook(void* self, void* fn, void* params) {
    static int count = 0;
    if (count < 15 && g_ToString && fn) {
        const FNameT* fname = (const FNameT*)((const uint8_t*)fn + UObject_NamePrivate);
        char buf[128];
        name_to_buf(fname, buf, sizeof buf);
        mac::logf("[L3] ProcessEvent fired: UFunction=%s", buf);
        count++;
        if (count == 15) mac::logf("[L3] (rate-limit: 이후 ProcessEvent 로그 생략) => 후킹·FName·리플렉션 동작");
    }
    g_pe_tramp(self, fn, params);  // 원본 체이닝
}

void ue4ss_spike_run() {
    mac::log_init("ue4ss-mac-spike");
    mac::logf("[L1] injected OK: pid=%d", getpid());

    // L2a: 서명 코드 COW 패치 (이미 PASS — 매 실행 재확인)
    if (void* real = dlsym(RTLD_DEFAULT, "getppid")) {
        pid_t before = getppid();
        uint8_t orig[8]; memcpy(orig, real, 8);
        const uint32_t patch[2] = { 0x5280A720u, 0xD65F03C0u };
        if (mac::code_make_writable(real, 8)) {
            memcpy(real, patch, 8); mac::code_make_executable(real, 8);
            pid_t patched = getppid();
            mac::code_make_writable(real, 8); memcpy(real, orig, 8); mac::code_make_executable(real, 8);
            mac::logf("[L2a] getppid COW patch: %d->%d->%d => %s",
                      before, patched, getppid(), (patched == 1337 && getppid() == before) ? "PASS" : "FAIL");
        }
    }

    // L3: 심볼 해결 (slide + nm주소). 생성자 시점이라 "주소 계산"만 — 엔진 호출은 아직 안 함(엔진 init 전).
    const intptr_t slide = mac::main_slide();
    g_ToString = (FName_ToString_fn)(sym::FName_ToString + slide);
    void* pe   = (void*)(sym::ProcessEvent + slide);
    mac::logf("[L3] slide=%#lx GUObjectArray=%p GMalloc=%p ProcessEvent=%p FName::FName=%p FName::ToString=%p StaticConstruct=%p",
              (long)slide, (void*)(sym::GUObjectArray + slide), (void*)(sym::GMalloc + slide),
              pe, (void*)(sym::FName_ctor + slide), (void*)g_ToString, (void*)(sym::StaticConstruct + slide));

    // L3: ProcessEvent 트램펄린 후킹. 생성자 시점엔 ProcessEvent가 아직 호출 전이라 설치 안전.
    // 콜백은 엔진 가동 후 첫 호출부터 발화 → 그 안전한 컨텍스트에서 FName/UObject 검증.
    mac::Hook hk = mac::install_hook(pe, (void*)&pe_hook);
    if (hk.ok) {
        g_pe_tramp = (PE_fn)hk.trampoline;
        mac::logf("[L3] ProcessEvent 후킹 설치 OK (tramp=%p) — 엔진 가동 대기 중...", hk.trampoline);
    } else {
        mac::logf("[L3] ProcessEvent install_hook FAILED");
    }
}

__attribute__((constructor))
static void ue4ss_spike_ctor() { ue4ss_spike_run(); }
