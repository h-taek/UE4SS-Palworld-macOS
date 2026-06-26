// test_ps_scan_darwin.cpp — 단위테스트: Mach-O LC_SYMTAB 심볼 리졸버 검증
// UE4SS_PS_SCAN_REFERENCE_MACHO 로 지정한 Mach-O fixture가 있을 때 심볼 대조를 수행한다.
// make test 로 실행.
//
// 검증 전략:
//   1. resolve_macho_symbol()로 fixture 바이너리에서 심볼 파일VM 주소를 파싱
//   2. 필요한 UE 앵커 심볼을 찾을 수 있으면 PASS
//   3. fixture 미지정 시 SKIP(exit 0)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cstdlib>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/fat.h>

// ── Mach-O LC_SYMTAB 파서 ──────────────────────────────────────────────────
//
// Mach-O 바이너리(fat binary, arm64 슬라이스)에서 심볼 파일VM 주소를 반환.
// 심볼을 못 찾으면 0 반환.
// 주의: 로컬 심볼(lowercase t/s)도 포함 — dlsym 불가능하지만 여기선 직접 파싱.

static uint64_t resolve_macho_symbol(const char* binary_path, const char* symbol_name)
{
    // 1. 파일 열기 + mmap
    int fd = open(binary_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "  [resolve] open failed: %s (%s)\n", binary_path, strerror(errno));
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return 0;
    }

    const size_t file_size = (size_t)st.st_size;
    const uint8_t* base = (const uint8_t*)mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        fprintf(stderr, "  [resolve] mmap failed: %s\n", strerror(errno));
        return 0;
    }

    // 2. Fat binary 처리 — arm64 슬라이스 찾기
    const uint8_t* macho_start = base;
    uint32_t magic = *(const uint32_t*)base;

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        // Big-endian fat header
        const struct fat_header* fh = (const struct fat_header*)base;
        uint32_t nfat = __builtin_bswap32(fh->nfat_arch);
        const struct fat_arch* archs = (const struct fat_arch*)(fh + 1);
        bool found = false;
        for (uint32_t i = 0; i < nfat; i++) {
            uint32_t cputype = (uint32_t)__builtin_bswap32((uint32_t)archs[i].cputype);
            uint32_t offset  = __builtin_bswap32(archs[i].offset);
            // CPU_TYPE_ARM64 = 0x0100000C
            if (cputype == 0x0100000Cu) {
                macho_start = base + offset;
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "  [resolve] arm64 슬라이스 없음\n");
            munmap((void*)base, file_size);
            return 0;
        }
    }

    // 3. mach_header_64 확인
    const struct mach_header_64* mh = (const struct mach_header_64*)macho_start;
    if (mh->magic != MH_MAGIC_64) {
        fprintf(stderr, "  [resolve] MH_MAGIC_64 불일치: 0x%x\n", mh->magic);
        munmap((void*)base, file_size);
        return 0;
    }

    // 4. Load command 순회 → LC_SYMTAB 탐색
    const uint8_t* cmd_ptr = (const uint8_t*)(mh + 1);
    const struct symtab_command* symtab = nullptr;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command* lc = (const struct load_command*)cmd_ptr;
        if (lc->cmd == LC_SYMTAB) {
            symtab = (const struct symtab_command*)lc;
            break;
        }
        cmd_ptr += lc->cmdsize;
    }

    if (!symtab) {
        fprintf(stderr, "  [resolve] LC_SYMTAB 없음\n");
        munmap((void*)base, file_size);
        return 0;
    }

    // 5. nlist_64 + strtab 포인터 (파일 오프셋 기준)
    //    단일-아치 슬라이스가 fat 내 오프셋에 위치 →
    //    symoff/stroff 는 해당 Mach-O 슬라이스의 내부 파일 오프셋이므로
    //    "슬라이스 시작"에서 더해야 한다.
    const struct nlist_64* syms = (const struct nlist_64*)(macho_start + symtab->symoff);
    const char*            strs = (const char*)(macho_start + symtab->stroff);
    uint32_t               nsyms = symtab->nsyms;

    // 6. 심볼명 비교 (strtab 엔트리는 1B 오프셋 헤더 포함, 첫 바이트 skip)
    for (uint32_t i = 0; i < nsyms; i++) {
        uint32_t str_idx = syms[i].n_un.n_strx;
        if (str_idx == 0) continue;
        const char* name = strs + str_idx;
        if (strcmp(name, symbol_name) == 0) {
            uint64_t addr = syms[i].n_value;
            munmap((void*)base, file_size);
            return addr;
        }
    }

    munmap((void*)base, file_size);
    return 0;
}

// ── 공식 fixture에서 확인해야 할 UE 앵커 심볼 ───────────────────────────────
static const struct {
    const char* mangled;
    const char* description;
} k_symbols[] = {
    { "_GUObjectArray",
      "GUObjectArray (global UObject 배열)" },
    { "__ZNK5FName8ToStringER7FString",
      "FName::ToString(FString&) const" },
    { "__ZN5FNameC1EPKDs9EFindName",
      "FName::FName(const char16_t*, EFindName)" },
    { "_GMalloc",
      "GMalloc (FMalloc* 전역)" },
    { "__Z30StaticConstructObject_InternalRK32FStaticConstructObjectParameters",
      "StaticConstructObject_Internal(...)" },
    { "__ZN5FTextC2EO7FString",
      "FText::FText(FString&&)" },
};

int main()
{
    printf("=== test_ps_scan_darwin ===\n");

    const char* fixture_path = getenv("UE4SS_PS_SCAN_REFERENCE_MACHO");
    if (!fixture_path || !fixture_path[0]) {
        printf("SKIP: UE4SS_PS_SCAN_REFERENCE_MACHO 미설정\n");
        return 0;
    }
    printf("바이너리: %s\n\n", fixture_path);

    if (access(fixture_path, R_OK) != 0) {
        printf("SKIP: fixture 읽기 실패 (%s)\n", strerror(errno));
        return 0;
    }

    int pass = 0, fail = 0;
    const int n = (int)(sizeof(k_symbols) / sizeof(k_symbols[0]));

    for (int i = 0; i < n; i++) {
        uint64_t got = resolve_macho_symbol(fixture_path, k_symbols[i].mangled);
        if (got == 0) {
            printf("FAIL [%d] %s\n     심볼 찾기 실패: %s\n",
                   i+1, k_symbols[i].description, k_symbols[i].mangled);
            fail++;
        } else {
            printf("PASS [%d] %s\n     주소=0x%llx (심볼 발견)\n",
                   i+1, k_symbols[i].description, (unsigned long long)got);
            pass++;
        }
    }

    printf("\n결과: PASS=%d FAIL=%d / 총 %d\n", pass, fail, n);

    if (fail > 0) {
        printf("테스트 FAILED — 리졸버 버그 또는 심볼명 불일치\n");
        return 1;
    }
    printf("테스트 PASSED\n");
    return 0;
}
