#pragma once
#include <cstdint>
#include <cstddef>
namespace mac {
// 반환값 = src에서 소비한 원본 바이트(>= min_bytes), 실패 시 0.
// out_emitted(옵션) = dst에 써낸 바이트. PC-상대 분기를 절대 베니어로 보정하면
// 써낸 바이트가 소비 바이트보다 커질 수 있으므로 둘을 분리해 보고한다.
size_t relocate_prologue(const uint8_t* src, uint8_t* dst, size_t dst_cap, size_t min_bytes, size_t* out_emitted = nullptr);

// arm64 분기 썽크 추적기. fn이 무조건 b/bl 로 시작하면 타깃까지 따라가 최종
// 비분기 명령 주소를 돌려준다(최대 8단계). x86 ASM::resolve_function_address_
// _from_potential_jmp 의 arm64 대응물. PC-상대 b/bl 만 추적(adr/adrp/조건분기 X).
void* resolve_thunk_target(void* fn);

// ProcessInternal 코드를 디스어셈블해 ProcessLocalScriptFunction(PLSF)의 런타임
// 주소를 도출한다(심볼·하드코딩 오프셋 비의존, 런타임 일반 방식).
//
// 동작 원리(실 바이너리 디스어셈블 근거, UE5.1 arm64/clang):
//   ProcessInternal 본문은 조건분기 골격을 타고 내려가다, 콜리세이브 레지스터를
//   복원하는 에필로그 뒤에 PLSF로 무조건 b(테일콜)를 친다. 예)
//     tbz  w0,  #0, L1        ; 조건분기 → taken 추종
//     tbnz w23, #1, L2        ; 조건분기 → taken 추종
//     ... (L2에서 ldp 로 프레임 복원) ...
//     b    ProcessLocalScriptFunction   ; 무조건 테일콜 = PLSF
//   따라서 "조건분기는 taken 타깃으로 추종하고, 처음 만나는 무조건 b 의 타깃이
//   PLSF" 이다. 이는 업스트림 x86(Zydis: COND_BR 추종 후 첫 JMP 해석)과 동형이며,
//   조건분기를 건너뛰므로 함수 내부 루프용 b 를 만나지 않는다.
//
// 반환: PLSF 의 런타임(또는 입력과 동일 주소공간) 주소. 추적 불가 시 nullptr
//       (호출측이 AOB 폴백을 쓰도록 — 본 함수 범위 밖). 명령 수/추종 횟수에
//       상한을 두어 런어웨이 대신 fail-loud-nullptr.
void* walk_to_local_script_fn(void* process_internal);
}
