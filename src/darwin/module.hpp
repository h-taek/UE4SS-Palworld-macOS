#pragma once
#include <cstdint>
#include <cstddef>
namespace mac {
// 메인 실행 이미지(Palworld)의 ASLR 슬라이드. 링크타임 심볼주소 + slide = 런타임 주소.
intptr_t main_slide();
// 메인 실행파일(MH_EXECUTE)의 런타임 __TEXT,__text 구간.
// out_start=슬라이드 적용 시작, out_size=바이트, out_file_vmaddr(옵션)=슬라이드 전 VM주소.
bool main_text_range(const uint8_t** out_start, size_t* out_size, uintptr_t* out_file_vmaddr);
}
