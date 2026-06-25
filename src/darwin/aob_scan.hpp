#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace mac {

// 바이트 시그니처: mask[i]==0xFF → pat[i] 일치 필수, 0x00 → 와일드카드(??).
struct Sig {
    std::vector<uint8_t> pat;
    std::vector<uint8_t> mask;
};

// "FC 6F ?? A9" 식 문자열 → Sig. 토큰은 2자리 hex 또는 "??". 형식 오류 시 false.
bool aob_parse(const char* s, Sig& out);

// [buf,buf+size) 에서 sig 의 첫 일치 오프셋(없으면 SIZE_MAX). *out_count=총 일치 수.
size_t aob_find(const uint8_t* buf, size_t size, const Sig& sig, size_t* out_count);

// code[0..3]=ADRP, code[4..7]=ADD(imm) 또는 LDR(imm,unsigned) 를 디코드해
// 형성되는 절대 주소 반환. pc_vmaddr = code 위치의 VM 주소. ADRP 아니면 0.
uintptr_t adrp_target(const uint8_t* code, uintptr_t pc_vmaddr);

enum class AnchorKind { Function, GlobalAdrp };
struct AnchorSig { const char* name; const char* sig; AnchorKind kind; };

// 6앵커 시그니처 테이블. *out_n = 개수.
const AnchorSig* anchor_table(size_t* out_n);

// 버퍼 [text,text+size) (VM주소 text_vmaddr)에서 a 를 해석. 유일 일치 아니면 0.
// Function → 일치 위치의 VM주소. GlobalAdrp → 일치 위치의 ADRP+ADD/LDR 디코드 절대주소.
uintptr_t aob_resolve(const uint8_t* text, size_t size, uintptr_t text_vmaddr, const AnchorSig& a);

// 런타임 메인이미지 __text 를 잡아 anchor_table 의 name 항목을 해석(주입 환경).
uintptr_t aob_resolve_runtime(const char* anchor_name);

}  // namespace mac
