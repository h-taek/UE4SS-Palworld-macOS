#include "darwin/aob_scan.hpp"
#include "darwin/module.hpp"
#include <cctype>
#include <cstring>

namespace mac {

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool aob_parse(const char* s, Sig& out) {
    out.pat.clear(); out.mask.clear();
    if (!s) return false;
    for (size_t i = 0; s[i]; ) {
        if (isspace((unsigned char)s[i])) { i++; continue; }
        if (s[i] == '?') {
            // "??" 또는 "?" 모두 와일드카드 1바이트로 허용
            out.pat.push_back(0x00); out.mask.push_back(0x00);
            i++; if (s[i] == '?') i++;
            continue;
        }
        int hi = hexval(s[i]);
        int lo = (hi >= 0 && s[i+1]) ? hexval(s[i+1]) : -1;
        if (hi < 0 || lo < 0) return false;          // 형식 오류
        out.pat.push_back((uint8_t)((hi << 4) | lo));
        out.mask.push_back(0xFF);
        i += 2;
    }
    return !out.pat.empty();
}

size_t aob_find(const uint8_t* buf, size_t size, const Sig& sig, size_t* out_count) {
    const size_t plen = sig.pat.size();
    size_t count = 0, first = SIZE_MAX;
    if (out_count) *out_count = 0;
    if (plen == 0 || plen > size) return SIZE_MAX;

    // 첫 바이트가 고정(mask[0]==0xFF)이면 memchr 로 후보 위치만 점프(277MB 스캔 가속).
    const bool anchored = sig.mask[0] == 0xFF;
    const uint8_t first_byte = sig.pat[0];
    const size_t last = size - plen;

    for (size_t i = 0; i <= last; ) {
        if (anchored) {
            const void* hit = memchr(buf + i, first_byte, last - i + 1);
            if (!hit) break;
            i = (size_t)((const uint8_t*)hit - buf);
        }
        bool ok = true;
        for (size_t j = 0; j < plen; j++) {
            if (sig.mask[j] && (buf[i + j] & sig.mask[j]) != (sig.pat[j] & sig.mask[j])) { ok = false; break; }
        }
        if (ok) { if (first == SIZE_MAX) first = i; count++; }
        i++;
    }
    if (out_count) *out_count = count;
    return first;
}

uintptr_t adrp_target(const uint8_t* code, uintptr_t pc_vmaddr) {
    uint32_t adrp; memcpy(&adrp, code, 4);
    if ((adrp & 0x9F000000u) != 0x90000000u) return 0;          // ADRP 아님
    int64_t immlo = (adrp >> 29) & 0x3;
    int64_t immhi = (adrp >> 5) & 0x7FFFF;                       // 19비트
    int64_t imm = (immhi << 2) | immlo;                         // 21비트
    imm = (imm << 43) >> 43;                                    // 부호확장
    uintptr_t page = (pc_vmaddr & ~(uintptr_t)0xFFF) + (uintptr_t)(imm << 12);

    uint32_t next; memcpy(&next, code + 4, 4);
    if ((next & 0xFF800000u) == 0x91000000u) {                  // ADD (imm, 64비트, shift=0)
        uint32_t add_imm = (next >> 10) & 0xFFF;
        return page + add_imm;
    }
    if ((next & 0xFFC00000u) == 0xF9400000u) {                  // LDR (imm, unsigned, 64비트)
        uint32_t scaled = ((next >> 10) & 0xFFF) << 3;          // imm12 * 8
        return page + scaled;                                   // 포인터 슬롯의 주소
    }
    return page;                                                // ADD/LDR 아니면 페이지만
}

// 시그니처 출처: tools/derive-aob.py (2026-06-24 바이너리). 재생성 가능.
// GlobalAdrp 2개는 Task 6에서 도출(전역 참조 ADRP). 우선 함수 4개 확정.
static const AnchorSig k_anchors[] = {
    { "FName::ToString",                "FC 6F BB A9 F8 5F 01 A9 F6 57 02 A9 F4 4F 03 A9 FD 7B 04 A9 FD 03 01 91 FF 03 20 D1", AnchorKind::Function },
    { "FName::FName",                   "F4 4F BE A9 FD 7B 01 A9 FD 43 00 91 E8 03 01 AA F3 03 00 AA ?? ?? ?? ?? 0B 01 40 79", AnchorKind::Function },
    { "StaticConstructObject_Internal", "F8 5F BC A9 F6 57 01 A9 F4 4F 02 A9 FD 7B 03 A9 FD C3 00 91 FF C3 08 D1 F3 03 00 AA ?? ?? ?? ?? 08 05 41 F9", AnchorKind::Function },
    { "FText::FText",                   "FF 43 01 D1 F6 57 02 A9 F4 4F 03 A9 FD 7B 04 A9 FD 03 01 91 F5 03 01 AA F3 03 00 AA 00 E4 00 6F E0 03 00 AD", AnchorKind::Function },
    // 전역 2개 — Task 6에서 GlobalAdrp 시그니처로 채움:
    { "GUObjectArray",                  "?? ?? ?? ?? 00 40 2C 91 ?? ?? ?? ?? A0 C3 02 D1 ?? ?? ?? ?? A0 C3 02 D1", AnchorKind::GlobalAdrp },
    { "GMalloc",                        "?? ?? ?? ?? 08 41 12 91 00 01 40 F9 08 00 40 F9 08 39 40 F9",             AnchorKind::GlobalAdrp },
    // Task 3: 리플렉션 폴백용 ProcessInternal 앵커 (tools/derive-aob.py 2026-06-25, instrs=9, matches=1)
    { "UObject::ProcessInternal",       "F8 5F BC A9 F6 57 01 A9 F4 4F 02 A9 FD 7B 03 A9 FD C3 00 91 F3 03 02 AA F4 03 01 AA F6 03 00 AA 35 08 40 F9", AnchorKind::Function },
    // Task 5b: PLSF 폴백 앵커 (tools/derive-aob.py 2026-06-25, instrs=12, matches=1)
    { "ProcessLocalScriptFunction",     "FF 03 02 D1 F6 57 05 A9 F4 4F 06 A9 FD 7B 07 A9 FD C3 01 91 F3 03 02 AA F4 03 01 AA ?? ?? ?? ?? 08 05 41 F9 08 01 40 F9 A8 83 1D F8 35 08 40 F9", AnchorKind::Function },
};

const AnchorSig* anchor_table(size_t* out_n) {
    if (out_n) *out_n = sizeof(k_anchors) / sizeof(k_anchors[0]);
    return k_anchors;
}

uintptr_t aob_resolve(const uint8_t* text, size_t size, uintptr_t text_vmaddr, const AnchorSig& a) {
    Sig sig;
    if (!aob_parse(a.sig, sig)) return 0;
    size_t count = 0;
    size_t off = aob_find(text, size, sig, &count);
    if (off == SIZE_MAX || count != 1) return 0;          // 유일 일치만 신뢰
    uintptr_t hit_vm = text_vmaddr + off;
    if (a.kind == AnchorKind::Function) return hit_vm;
    // GlobalAdrp: 일치 위치의 ADRP(+ADD/LDR) 를 디코드.
    return adrp_target(text + off, hit_vm);
}

uintptr_t aob_resolve_runtime(const char* anchor_name) {
    const uint8_t* text=nullptr; size_t size=0; uintptr_t vm=0;
    if (!main_text_range(&text, &size, &vm)) return 0;
    size_t n=0; const AnchorSig* tbl = anchor_table(&n);
    for (size_t i=0;i<n;i++) if (!__builtin_strcmp(tbl[i].name, anchor_name))
        return aob_resolve(text, size, (uintptr_t)text, tbl[i]);   // text_vmaddr == runtime VM addr of text[0]
    return 0;
}

}  // namespace mac
