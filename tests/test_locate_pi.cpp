#include <cstdint>
#include <cstdio>
#include <cstring>
#include "hook/arm64_reloc.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/fat.h>

// arm64 인코딩: B #imm = 0x14000000 | (imm>>2 & 0x3FFFFFF); BL = 0x94000000 | ...
// NOP = 0xD503201F. 모두 4바이트, 리틀엔디언.
int main() {
    int failures = 0;

    // ---- resolve_thunk_target (Task 5 이전 기존 테스트) ----

    // case 1: buf[0] = B #8 (앞으로 8바이트), buf[2] = NOP -> 추적 결과 = &buf[2]
    {
        uint32_t buf[4] = { 0x14000002u /*B #8*/, 0xD503201Fu, 0xD503201Fu /*NOP@+8*/, 0xD503201Fu };
        void* got = mac::resolve_thunk_target((void*)buf);
        void* want = (void*)((uint8_t*)buf + 8);
        bool ok = (got == want);
        printf("test_locate_pi(b-thunk): got=%p want=%p => %s\n", got, want, ok ? "PASS" : "FAIL");
        if (!ok) failures++;
    }

    // case 2: BL #8 도 동일하게 타깃까지 추적
    {
        uint32_t buf[4] = { 0x94000002u /*BL #8*/, 0xD503201Fu, 0xD503201Fu, 0xD503201Fu };
        void* got = mac::resolve_thunk_target((void*)buf);
        void* want = (void*)((uint8_t*)buf + 8);
        bool ok = (got == want);
        printf("test_locate_pi(bl-thunk): got=%p want=%p => %s\n", got, want, ok ? "PASS" : "FAIL");
        if (!ok) failures++;
    }

    // case 3: 비분기 시작 -> 자기 자신 반환
    {
        uint32_t buf[2] = { 0xD503201Fu /*NOP*/, 0xD65F03C0u /*RET*/ };
        void* got = mac::resolve_thunk_target((void*)buf);
        bool ok = (got == (void*)buf);
        printf("test_locate_pi(no-thunk): got=%p want=%p => %s\n", got, (void*)buf, ok ? "PASS" : "FAIL");
        if (!ok) failures++;
    }

    // ---- Task 5a: walk_to_local_script_fn (조건분기 추종 → 첫 무조건 b = PLSF) ----

    // case 4(SYNTH): 실 바이너리 구조 모델. 조건분기(tbnz)가 디코이 무조건 b 를
    // 건너뛰고, 그 뒤 무조건 b 가 테일콜(PLSF)로 가는 형태.
    //   [0] NOP
    //   [1] NOP
    //   [2] TBNZ w0,#1,&[6]   ; 조건분기 → taken 추종(디코이 건너뜀)
    //   [3] B    &[11]        ; 디코이 무조건 b — 선형스캔이면 여기 걸려 오답
    //   [4] NOP / [5] NOP     ; (디코이 본문)
    //   [6] NOP               ; (L1: 추종 도착점)
    //   [7] B    &[9]         ; 무조건 테일콜 = PLSF
    //   [8] NOP
    //   [9] NOP               ; (PLSF 진입: 비분기 → 그대로 반환)
    //   [10] NOP / [11] NOP   ; (디코이 타깃: 비분기)
    // capstone 검증된 인코딩: TBNZ w0,#1,+16 = 0x37080080, B +16 = 0x14000008,
    //                         B +8 = 0x14000002.
    {
        uint32_t buf[12] = {
            0xD503201Fu, // 0
            0xD503201Fu, // 1
            0x37080080u, // 2 TBNZ w0,#1,&[6]
            0x14000008u, // 3 B &[11] (디코이)
            0xD503201Fu, // 4
            0xD503201Fu, // 5
            0xD503201Fu, // 6 L1
            0x14000002u, // 7 B &[9] (테일콜)
            0xD503201Fu, // 8
            0xD503201Fu, // 9 PLSF
            0xD503201Fu, // 10
            0xD503201Fu, // 11 디코이 타깃
        };
        void* got  = mac::walk_to_local_script_fn((void*)buf);
        void* want = (void*)((uint8_t*)buf + 9 * 4);
        void* decoy = (void*)((uint8_t*)buf + 11 * 4);
        bool ok = (got == want);
        printf("test_locate_pi(walk-synth): got=%p want=%p (decoy=%p) => %s\n",
               got, want, decoy, ok ? "PASS" : "FAIL");
        if (!ok) failures++;
    }

    // case 5(NULL): 테일콜 없이 ret 로 끝나는 코드 → nullptr (fail-loud).
    {
        uint32_t buf[3] = { 0xD503201Fu /*NOP*/, 0xD503201Fu /*NOP*/, 0xD65F03C0u /*RET*/ };
        void* got = mac::walk_to_local_script_fn((void*)buf);
        bool ok = (got == nullptr);
        printf("test_locate_pi(walk-null): got=%p want=NULL => %s\n", got, ok ? "PASS" : "FAIL");
        if (!ok) failures++;
    }

    // case 6(REAL): 실 게임 바이너리 __text 를 매핑해 ProcessInternal 파일포인터에서
    // walk 실행 → 결과가 PLSF 심볼주소(파일포인터 환산)와 일치하는지 검증.
    // __text 내부에서는 파일 레이아웃과 VM 레이아웃의 상대오프셋이 동일하므로,
    // cs_disasm 에 파일포인터를 주소로 넘기면 분기 타깃도 파일포인터로 나온다.
    {
        const char* k_game = "/Applications/Palworld.app/Contents/MacOS/Palworld";
        const char* PI_SYM   = "__ZN7UObject15ProcessInternalEPS_R6FFramePv";
        const char* PLSF_SYM  = "__Z26ProcessLocalScriptFunctionP7UObjectR6FFramePv";

        int fd = open(k_game, O_RDONLY);
        if (fd < 0) {
            printf("test_locate_pi(walk-real): SKIP (게임 바이너리 없음)\n");
        } else {
            struct stat st; fstat(fd, &st);
            size_t fsize = (size_t)st.st_size;
            const uint8_t* base = (const uint8_t*)mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (base == MAP_FAILED) {
                printf("test_locate_pi(walk-real): SKIP (mmap 실패)\n");
            } else {
                // FAT 이면 arm64 슬라이스 선택.
                const uint8_t* macho = base;
                uint32_t magic = *(const uint32_t*)base;
                if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
                    auto* fh = (const struct fat_header*)base;
                    uint32_t na = __builtin_bswap32(fh->nfat_arch);
                    auto* a = (const struct fat_arch*)(fh + 1);
                    for (uint32_t i = 0; i < na; i++)
                        if ((uint32_t)__builtin_bswap32((uint32_t)a[i].cputype) == 0x0100000Cu /*ARM64*/)
                            { macho = base + __builtin_bswap32(a[i].offset); break; }
                }
                auto* mh = (const struct mach_header_64*)macho;
                const uint8_t* text_ptr = nullptr; uintptr_t text_vm = 0;
                uint64_t pi_vm = 0, plsf_vm = 0;
                // __text 구간 + 두 심볼 주소 수집.
                {
                    const uint8_t* p = (const uint8_t*)(mh + 1);
                    for (uint32_t i = 0; i < mh->ncmds; i++) {
                        auto* lc = (const struct load_command*)p;
                        if (lc->cmd == LC_SEGMENT_64) {
                            auto* seg = (const struct segment_command_64*)lc;
                            if (!strcmp(seg->segname, "__TEXT")) {
                                auto* s = (const struct section_64*)((const uint8_t*)seg + sizeof(*seg));
                                for (uint32_t k = 0; k < seg->nsects; k++)
                                    if (!strcmp(s[k].sectname, "__text"))
                                        { text_ptr = macho + s[k].offset; text_vm = (uintptr_t)s[k].addr; }
                            }
                        } else if (lc->cmd == LC_SYMTAB) {
                            auto* sc = (const struct symtab_command*)lc;
                            auto* sy = (const struct nlist_64*)(macho + sc->symoff);
                            const char* str = (const char*)(macho + sc->stroff);
                            for (uint32_t k = 0; k < sc->nsyms; k++) {
                                uint32_t idx = sy[k].n_un.n_strx;
                                if (!idx) continue;
                                const char* nm = str + idx;
                                if (!strcmp(nm, PI_SYM))   pi_vm   = sy[k].n_value;
                                if (!strcmp(nm, PLSF_SYM)) plsf_vm = sy[k].n_value;
                            }
                        }
                        p += lc->cmdsize;
                    }
                }
                if (!text_ptr || !pi_vm || !plsf_vm) {
                    printf("test_locate_pi(walk-real): SKIP (text/심볼 미발견 text=%p pi=%#llx plsf=%#llx)\n",
                           (const void*)text_ptr, (unsigned long long)pi_vm, (unsigned long long)plsf_vm);
                } else {
                    const uint8_t* pi_file        = text_ptr + (pi_vm   - text_vm);
                    const uint8_t* plsf_file_want = text_ptr + (plsf_vm - text_vm);
                    void* got = mac::walk_to_local_script_fn((void*)pi_file);
                    // VM 환산(사람이 읽기 쉽게): got 파일포인터 → VM 주소.
                    uint64_t got_vm = got ? ((uintptr_t)got - (uintptr_t)text_ptr + text_vm) : 0;
                    bool ok = (got == (void*)plsf_file_want);
                    printf("test_locate_pi(walk-real): walk_vm=%#llx PLSF_sym_vm=%#llx "
                           "(PI_vm=%#llx) => %s\n",
                           (unsigned long long)got_vm, (unsigned long long)plsf_vm,
                           (unsigned long long)pi_vm, ok ? "PASS" : "FAIL");
                    if (!ok) failures++;
                }
                munmap((void*)base, fsize);
            }
        }
    }

    printf("test_locate_pi: %s\n", failures == 0 ? "ALL PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
