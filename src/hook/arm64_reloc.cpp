#include "hook/arm64_reloc.hpp"
#include "darwin/log.hpp"
#include <capstone/capstone.h>
#include <cstring>
#include <cstdlib>
namespace mac {

// 진단 로그 게이트(기본 OFF). 환경변수 UE4SS_MAC_DIAG가 비어있지 않고 "0"이 아니면 ON.
// 평소엔 조용하고, 후킹 디버깅(AutoHatch 등) 시 켜서 어떤 타깃이 b/bl 베니어를 타는지 추적한다.
static bool diag_enabled() {
    static const bool on = []{ const char* e = std::getenv("UE4SS_MAC_DIAG"); return e && e[0] && e[0] != '0'; }();
    return on;
}

// 절대점프 베니어(LDR X16,#8 ; BR X16 ; <8B 절대주소>) — inline_hook.cpp의 abs_jump와 동일 인코딩.
static constexpr uint32_t LDR_X16_8  = 0x58000050u; // LDR X16,#8
static constexpr uint32_t BR_X16     = 0xD61F0200u; // BR X16
// 절대콜 베니어(LDR X16,#12 ; BLR X16 ; B #12 ; <8B 절대주소>) — bl용. BLR 후 LR이 B #12를
// 가리켜 8B 리터럴을 건너뛰고 다음 재배치 명령으로 복귀한다.
static constexpr uint32_t LDR_X16_12 = 0x58000070u; // LDR X16,#12
static constexpr uint32_t BLR_X16    = 0xD63F0200u; // BLR X16
static constexpr uint32_t B_OVER_LIT = 0x14000003u; // B #12 (8B 리터럴 건너뜀)

static bool is_pc_relative(const cs_insn& in) {
    for (int i = 0; i < in.detail->groups_count; i++)
        if (in.detail->groups[i] == CS_GRP_BRANCH_RELATIVE) return true;
    switch (in.id) { case ARM64_INS_ADR: case ARM64_INS_ADRP:
        case ARM64_INS_LDR: case ARM64_INS_LDRSW: case ARM64_INS_PRFM:
            // LDR literal 형태만 PC상대(베이스 레지스터 없음) — detail로 판별
            for (int i=0;i<in.detail->arm64.op_count;i++)
                if (in.detail->arm64.operands[i].type == ARM64_OP_IMM && in.id!=ARM64_INS_ADD) return true;
            return false;
        default: return false; }
}

// 무조건 분기 b: 분기 명령이면서 조건이 없음(AL/INVALID). 조건분기 b.cond는 제외.
static bool is_uncond_b(const cs_insn& in) {
    return in.id == ARM64_INS_B &&
           (in.detail->arm64.cc == ARM64_CC_AL || in.detail->arm64.cc == ARM64_CC_INVALID);
}

size_t relocate_prologue(const uint8_t* src, uint8_t* dst, size_t dst_cap, size_t min_bytes, size_t* out_emitted) {
    csh h;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &h) != CS_ERR_OK) { mac::logf("cs_open fail"); return 0; }
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
    size_t consumed = 0; // src에서 먹은 원본 바이트(복귀 주소·패치 길이 기준)
    size_t emitted  = 0; // dst에 써낸 바이트(베니어 확장 포함, 트램펄린 레이아웃 기준)
    const uint8_t* p = src;
    while (consumed < min_bytes) {
        cs_insn* in = nullptr;
        size_t n = cs_disasm(h, p, 16, (uint64_t)(uintptr_t)p, 1, &in);
        if (n != 1) { mac::logf("cs_disasm fail @%p", (void*)p); cs_close(&h); return 0; }

        if (is_pc_relative(*in)) {
            if (is_uncond_b(*in)) {
                // b #target → 절대점프 베니어로 보정(상대 오프셋은 트램펄린서 깨지므로 절대주소로 고정).
                uint64_t tgt = (uint64_t)in->detail->arm64.operands[0].imm; // capstone이 해석한 절대 타깃
                if (diag_enabled()) mac::logf("[DIAG] reloc b-veneer @src=%p off=%zu tgt=%#llx", (void*)p, consumed, (unsigned long long)tgt);
                if (emitted + 16 > dst_cap) {
                    mac::logf("reloc: dst overflow (b veneer) emitted=%zu cap=%zu", emitted, dst_cap);
                    cs_free(in, 1); cs_close(&h); return 0;
                }
                memcpy(dst + emitted + 0, &LDR_X16_8, 4);
                memcpy(dst + emitted + 4, &BR_X16, 4);
                memcpy(dst + emitted + 8, &tgt, 8);
                emitted  += 16;
                consumed += in->size; p += in->size;
                cs_free(in, 1);
                continue;
            }
            if (in->id == ARM64_INS_BL) {
                // bl #target → LR 보존 콜 베니어. 콜에서 돌아오면 다음 재배치 명령으로 이어짐.
                uint64_t tgt = (uint64_t)in->detail->arm64.operands[0].imm;
                if (diag_enabled()) mac::logf("[DIAG] reloc bl-veneer @src=%p off=%zu tgt=%#llx", (void*)p, consumed, (unsigned long long)tgt);
                if (emitted + 20 > dst_cap) {
                    mac::logf("reloc: dst overflow (bl veneer) emitted=%zu cap=%zu", emitted, dst_cap);
                    cs_free(in, 1); cs_close(&h); return 0;
                }
                memcpy(dst + emitted + 0,  &LDR_X16_12, 4);
                memcpy(dst + emitted + 4,  &BLR_X16, 4);
                memcpy(dst + emitted + 8,  &B_OVER_LIT, 4);
                memcpy(dst + emitted + 12, &tgt, 8);
                emitted  += 20;
                consumed += in->size; p += in->size;
                cs_free(in, 1);
                continue;
            }
            // 그 외 PC-상대(조건분기·adr/adrp·ldr-literal)는 미지원 → 안전하게 중단(fail-loud).
            mac::logf("reloc: PC-relative insn '%s %s' not handled (spike)", in->mnemonic, in->op_str);
            cs_free(in, 1); cs_close(&h); return 0;
        }

        // PC-독립 명령: 바이트 그대로 복사
        if (emitted + in->size > dst_cap) {
            mac::logf("reloc: dst buffer overflow: emitted=%zu in->size=%u dst_cap=%zu", emitted, (unsigned)in->size, dst_cap);
            cs_free(in, 1); cs_close(&h); return 0;
        }
        memcpy(dst + emitted, p, in->size);
        emitted  += in->size;
        consumed += in->size; p += in->size;
        cs_free(in, 1);
    }
    cs_close(&h);
    if (out_emitted) *out_emitted = emitted;
    return consumed;
}

void* resolve_thunk_target(void* fn) {
    if (!fn) return nullptr;  // 널 입력 가드: cs_disasm(.., nullptr, ..) = addr 0 역참조(SEGV) 방지. 호출처는 null→AOB 폴백.
    csh h;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &h) != CS_ERR_OK) return fn;
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);

    void* cur = fn;
    for (int hops = 0; hops < 8; ++hops) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(cur);
        cs_insn* in = nullptr;
        size_t n = cs_disasm(h, p, 4, reinterpret_cast<uint64_t>(p), 1, &in);
        if (n == 0) { if (in) cs_free(in, n); break; }

        bool is_branch = (in->id == ARM64_INS_B || in->id == ARM64_INS_BL)
                         && in->detail
                         && (in->detail->arm64.cc == ARM64_CC_AL || in->detail->arm64.cc == ARM64_CC_INVALID)
                         && in->detail->arm64.op_count >= 1
                         && in->detail->arm64.operands[0].type == ARM64_OP_IMM;
        if (!is_branch) { cs_free(in, n); break; }

        void* target = reinterpret_cast<void*>(static_cast<uintptr_t>(in->detail->arm64.operands[0].imm));
        cs_free(in, n);
        if (target == cur) break;       // 자기참조 가드
        cur = target;
    }
    cs_close(&h);
    return cur;
}

// 조건분기 여부: b.cond / cbz / cbnz / tbz / tbnz.
// 주의: capstone에서 tbz/tbnz/cbz/cbnz 는 cc==INVALID(0) 로 나오므로 cc 가 아니라
// 명령 id 로 판별해야 한다(무조건 b 도 cc==INVALID 이라 cc 로는 구분 불가).
static bool is_cond_branch(const cs_insn& in) {
    switch (in.id) {
        case ARM64_INS_CBZ: case ARM64_INS_CBNZ:
        case ARM64_INS_TBZ: case ARM64_INS_TBNZ:
            return true;
        case ARM64_INS_B:   // b.cond 만 조건분기(무조건 b 는 제외)
            return in.detail->arm64.cc != ARM64_CC_AL &&
                   in.detail->arm64.cc != ARM64_CC_INVALID;
        default:
            return false;
    }
}

// 분기 타깃(절대주소) = 마지막 IMM 피연산자. capstone이 PC-상대 분기의 imm 을
// 절대주소로 해석해 둔다. b.cond:op0, cbz:op1, tbz:op2 모두 "마지막 IMM" 으로 통일.
static bool branch_target(const cs_insn& in, uint64_t* out) {
    const auto& a = in.detail->arm64;
    for (int i = a.op_count - 1; i >= 0; --i)
        if (a.operands[i].type == ARM64_OP_IMM) { *out = (uint64_t)a.operands[i].imm; return true; }
    return false;
}

void* walk_to_local_script_fn(void* process_internal) {
    if (!process_internal) return nullptr;
    csh h;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &h) != CS_ERR_OK) { mac::logf("walk_plsf: cs_open fail"); return nullptr; }
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);

    // 상한: 함수 본문(팔월드 PI=0x138B)+여유. 추종 횟수는 조건분기 골격 깊이 가드.
    constexpr int kMaxInsns   = 256; // 총 디코드 명령 상한(≈1KB) — 런어웨이 방지
    constexpr int kMaxFollows = 16;  // 조건분기 추종 상한 — 백워드 루프 무한추종 방지

    const uint8_t* cur = reinterpret_cast<const uint8_t*>(process_internal);
    void* result = nullptr;
    int insns = 0, follows = 0;

    while (insns < kMaxInsns) {
        cs_insn* in = nullptr;
        size_t n = cs_disasm(h, cur, 4, reinterpret_cast<uint64_t>(cur), 1, &in);
        if (n != 1) { if (in) cs_free(in, n); mac::logf("walk_plsf: cs_disasm fail @%p", (const void*)cur); break; }
        ++insns;

        const unsigned id = in->id;

        // (1) 무조건 b → 테일콜 = PLSF. 타깃을 돌려준다(추가 베니어는 썽크추적으로 흡수).
        if (is_uncond_b(*in)) {
            uint64_t tgt = (uint64_t)in->detail->arm64.operands[0].imm;
            cs_free(in, n);
            result = reinterpret_cast<void*>(static_cast<uintptr_t>(tgt));
            break;
        }

        // (2) 조건분기 → taken 타깃으로 추종(조건분기 골격을 따라 내려간다).
        // ★취한방향 그리디 가정: taken 경로가 테일콜로 이어진다고 본다(upstream x86 워크와 동일 가정).
        //   주의: AOB 폴백(5b)은 walk가 **null**을 반환할 때만 돈다 — 그리디 오예측이 *틀린-비널*
        //   주소를 내면 그게 그대로 assign·후킹된다(AOB가 안 받침). Palworld는 실바이너리로 검증됨.
        //   다른 monolithic UE5.1 바이너리 범용 보강(워크 결과를 main_text_range로 바운드체크 등)은
        //   '범용 토대' 미래 과제(docs/research/07). 역방향 무한추종은 kMaxFollows 가 막는다.
        if (is_cond_branch(*in)) {
            uint64_t tgt = 0;
            bool got = branch_target(*in, &tgt);
            cs_free(in, n);
            if (!got) { mac::logf("walk_plsf: cond branch w/o imm target"); break; }
            if (++follows > kMaxFollows) { mac::logf("walk_plsf: follow cap hit"); break; }
            cur = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(tgt));
            continue;
        }

        // (3) ret / 간접 br → 테일콜 못 만나고 경로 종료 → 실패(fail-loud).
        if (id == ARM64_INS_RET || id == ARM64_INS_BR) { mac::logf("walk_plsf: hit %s before tail-call", in->mnemonic); cs_free(in, n); break; }

        // (4) 그 외(bl/blr 호출 포함) → 선형 진행(arm64 명령은 모두 4바이트).
        cs_free(in, n);
        cur += 4;
    }

    cs_close(&h);
    if (!result) return nullptr;
    // 테일콜 타깃이 브랜치 아일랜드/베니어를 한 번 더 거치면 최종 진입점까지 추적.
    // PLSF 진입은 비분기(sub sp,...)라 보통 그대로 반환된다.
    return resolve_thunk_target(result);
}
}
