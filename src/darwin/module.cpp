#include "darwin/module.hpp"
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <cstring>
namespace mac {
// 메인 실행 파일(MH_EXECUTE)의 ASLR 슬라이드. 인덱스 0이 항상 메인은 아님
// (dyld4 / DYLD_INSERT_LIBRARIES 환경에선 0번이 다른 이미지일 수 있음) → 타입으로 탐색.
intptr_t main_slide() {
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header* h = _dyld_get_image_header(i);
        if (h && h->filetype == MH_EXECUTE)
            return _dyld_get_image_vmaddr_slide(i);
    }
    return 0;
}
bool main_text_range(const uint8_t** out_start, size_t* out_size, uintptr_t* out_file_vmaddr) {
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header* h = _dyld_get_image_header(i);
        if (!h || h->filetype != MH_EXECUTE) continue;
        const intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        if (h->magic != MH_MAGIC_64) continue;   // 이 모듈은 arm64 전용 — 64비트 이미지만
        const uint8_t* lcp = (const uint8_t*)h + sizeof(struct mach_header_64);
        for (uint32_t c = 0; c < h->ncmds; c++) {
            const struct load_command* lc = (const struct load_command*)lcp;
            if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64* seg = (const struct segment_command_64*)lc;
                if (strcmp(seg->segname, "__TEXT") == 0) {
                    const struct section_64* sect =
                        (const struct section_64*)((const uint8_t*)seg + sizeof(struct segment_command_64));
                    for (uint32_t s = 0; s < seg->nsects; s++) {
                        if (strcmp(sect[s].sectname, "__text") == 0) {
                            if (out_start) *out_start = (const uint8_t*)(uintptr_t)(sect[s].addr + slide);
                            if (out_size) *out_size = (size_t)sect[s].size;
                            if (out_file_vmaddr) *out_file_vmaddr = (uintptr_t)sect[s].addr;
                            return true;
                        }
                    }
                }
            }
            lcp += lc->cmdsize;
        }
    }
    return false;
}
}
