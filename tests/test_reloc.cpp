#include "hook/arm64_reloc.hpp"
#include <cstdio>
#include <cstring>
#include <cstdint>

// 절대점프 베니어 인코딩(LDR X16,#8 ; BR X16)
static constexpr uint32_t LDR_X16_8 = 0x58000050u;
static constexpr uint32_t BR_X16    = 0xD61F0200u;

int main() {
    // --- 1) PC-독립 prologue: 바이트동일 복사 ---
    // stp x29,x30,[sp,#-16]! ; mov x29,sp ; sub sp,sp,#16 ; nop  (모두 PC-독립)
    {
        uint32_t prologue[4] = {0xA9BF7BFDu, 0x910003FDu, 0xD10043FFu, 0xD503201Fu};
        uint8_t dst[64] = {0};
        size_t emitted = 0;
        size_t consumed = mac::relocate_prologue((const uint8_t*)prologue, dst, sizeof(dst), 16, &emitted);
        bool ok = (consumed == 16) && (emitted == 16) && (memcmp(dst, prologue, 16) == 0);
        printf("test_reloc(pic): consumed=%zu emitted=%zu => %s\n", consumed, emitted, ok ? "PASS" : "FAIL");
        if (!ok) return 1;
    }

    // --- 2) 무조건 분기 b #target → 절대점프 베니어로 재배치 ---
    // b #0x20 ; nop ; nop ; nop  (원본 b는 PC-상대라 그대로 복사하면 깨짐)
    {
        uint32_t prog[4] = {0x14000008u /* b #0x20 */, 0xD503201Fu, 0xD503201Fu, 0xD503201Fu};
        uint8_t dst[64] = {0};
        size_t emitted = 0;
        size_t consumed = mac::relocate_prologue((const uint8_t*)prog, dst, sizeof(dst), 16, &emitted);
        uint64_t expect_tgt = (uint64_t)(uintptr_t)prog + 0x20; // capstone이 해석하는 절대 타깃
        uint32_t ldr = 0, br = 0; uint64_t tgt = 0;
        memcpy(&ldr, dst + 0, 4); memcpy(&br, dst + 4, 4); memcpy(&tgt, dst + 8, 8);
        bool ok = (consumed == 16)        // 원본 4명령(16B) 소비
               && (emitted == 16 + 12)    // 베니어 16B + nop 3개(12B)
               && (ldr == LDR_X16_8)      // LDR X16,#8
               && (br  == BR_X16)         // BR X16
               && (tgt == expect_tgt);    // 절대 타깃 = 원래 b가 가리키던 곳
        printf("test_reloc(b->veneer): consumed=%zu emitted=%zu tgt=%#llx => %s\n",
               consumed, emitted, (unsigned long long)tgt, ok ? "PASS" : "FAIL");
        if (!ok) return 1;
    }

    // --- 3) bl #target → LR 보존 콜 베니어 ---
    // bl #0x20 ; nop ; nop ; nop  (콜 후 트램펄린의 다음 명령으로 복귀해야 함)
    {
        uint32_t prog[4] = {0x94000008u /* bl #0x20 */, 0xD503201Fu, 0xD503201Fu, 0xD503201Fu};
        uint8_t dst[64] = {0};
        size_t emitted = 0;
        size_t consumed = mac::relocate_prologue((const uint8_t*)prog, dst, sizeof(dst), 16, &emitted);
        uint64_t expect_tgt = (uint64_t)(uintptr_t)prog + 0x20;
        uint32_t ldr = 0, blr = 0, b = 0; uint64_t tgt = 0;
        memcpy(&ldr, dst + 0, 4); memcpy(&blr, dst + 4, 4); memcpy(&b, dst + 8, 4); memcpy(&tgt, dst + 12, 8);
        bool ok = (consumed == 16)        // 원본 4명령(16B) 소비
               && (emitted == 20 + 12)    // bl 베니어 20B + nop 3개(12B)
               && (ldr == 0x58000070u)    // LDR X16,#12
               && (blr == 0xD63F0200u)    // BLR X16
               && (b   == 0x14000003u)    // B #12 (8B 리터럴 건너뜀 → 다음 명령 복귀)
               && (tgt == expect_tgt);    // 절대 타깃
        printf("test_reloc(bl->veneer): consumed=%zu emitted=%zu tgt=%#llx => %s\n",
               consumed, emitted, (unsigned long long)tgt, ok ? "PASS" : "FAIL");
        if (!ok) return 1;
    }

    return 0;
}
