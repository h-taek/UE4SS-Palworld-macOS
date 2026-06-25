// macOS (arm64) ps_scan 실구현 — patternsleuth Rust AOB 스캐너 대체.
//
// 팔월드 맥 바이너리는 미stripped(심볼 ~955k) → AOB 패턴 스캔 없이
// Mach-O LC_SYMTAB을 직접 파싱해 이름→파일VM 주소를 얻는다.
// 런타임 주소 = 파일VM 주소 + ASLR slide (MH_EXECUTE 이미지 기준).
//
// ABI 요구사항:
//   extern "C" bool ps_scan(PsCtx&, PsScanResults&)
//   PsCtx / PsScanResults 는 UnrealInitializer.cpp:109-145 에 인라인 정의됨.
//   여기서 동일 레이아웃으로 재선언 (헤더 공유 없음 — 틀리면 조용한 메모리 깨짐).
//
// P3 구현. 모두 찾으면 true 반환 → UnrealInitializer가 루프를 탈출.
// engine_version 은 UE5.1 하드코딩 (Palworld 고정; P4에서 CFBundleVersion 파싱 예정).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdlib>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/fat.h>

#include "darwin/aob_scan.hpp"   // mac::aob_resolve_runtime — MacHook 가 $(projectdir)/src 노출

// ── ABI 미러: UnrealInitializer.cpp:109-145 와 필드·순서·타입 바이트 동일 ──
// 주의: 이 구조체들은 헤더에 없음. 변경 시 UnrealInitializer.cpp 와 동기 필수.

struct PsScanConfig
{
    bool g_uobject_array{};
    bool fname_tostring_fstring{};
    bool fname_ctor_wchar{};
    bool gmalloc{};
    bool static_construct_object_internal{};
    bool ftext_fstring{};
    bool engine_version{};
};

struct PsCtx
{
    void (*default_)(char16_t* msg);
    void (*normal)(char16_t* msg);
    void (*verbose)(char16_t* msg);
    void (*warning)(char16_t* msg);
    void (*error)(char16_t* msg);
    PsScanConfig config{};
};

struct PsEngineVersion
{
    uint16_t major{};
    uint16_t minor{};
};

struct PsScanResults
{
    void* g_uobject_array{};
    void* fname_tostring_fstring{};
    void* fname_ctor_wchar{};
    void* gmalloc{};
    void* static_construct_object_internal{};
    void* ftext_fstring{};
    PsEngineVersion engine_version{};
};

// ── 헬퍼: char16 문자열 로그 ──────────────────────────────────────────────
// PsCtx.normal/verbose 콜백은 char16_t*를 받는다.
// 내부 로그는 char* → 변환 후 전달.
static void ctx_log(void (*cb)(char16_t*), const char* msg)
{
    if (!cb || !msg) return;
    // ASCII 범위 안에서만 변환 (로그용 — 비-ASCII 있으면 '?' 대체)
    static char16_t buf[512];
    int i = 0;
    for (; msg[i] && i < 510; i++)
        buf[i] = (uint8_t)msg[i] < 0x80 ? (char16_t)(uint8_t)msg[i] : u'?';
    buf[i] = 0;
    cb(buf);
}

// ── ASLR slide: MH_EXECUTE 이미지 기준 ──────────────────────────────────
// dyld4 + DYLD_INSERT_LIBRARIES 환경에서 인덱스 0은 메인이 아닐 수 있음.
static intptr_t main_slide()
{
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header* h = _dyld_get_image_header(i);
        if (h && h->filetype == MH_EXECUTE)
            return _dyld_get_image_vmaddr_slide(i);
    }
    return 0;
}

// ── 메인 실행파일 경로 ─────────────────────────────────────────────────────
static bool get_exe_path(char* out, size_t cap)
{
    // MH_EXECUTE 이미지의 경로를 dyld에서 가져옴
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header* h = _dyld_get_image_header(i);
        if (h && h->filetype == MH_EXECUTE) {
            const char* name = _dyld_get_image_name(i);
            if (name) {
                strncpy(out, name, cap - 1);
                out[cap - 1] = '\0';
                return true;
            }
        }
    }
    return false;
}

// ── Mach-O LC_SYMTAB 파서 ─────────────────────────────────────────────────
// binary_path: 디스크 상의 Mach-O (fat 포함). symbol_name: 정확한 mangled 명.
// 반환: 파일VM 주소(n_value). 못 찾으면 0.
static uint64_t resolve_macho_symbol(const char* binary_path, const char* symbol_name)
{
    int fd = open(binary_path, O_RDONLY);
    if (fd < 0) return 0;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return 0; }
    const size_t file_size = (size_t)st.st_size;

    const uint8_t* base = (const uint8_t*)mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) return 0;

    // Fat binary: arm64 슬라이스 탐색
    const uint8_t* macho_start = base;
    uint32_t magic = *(const uint32_t*)base;

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        const struct fat_header* fh = (const struct fat_header*)base;
        uint32_t nfat = __builtin_bswap32(fh->nfat_arch);
        const struct fat_arch* archs = (const struct fat_arch*)(fh + 1);
        bool found = false;
        for (uint32_t i = 0; i < nfat; i++) {
            uint32_t cputype = (uint32_t)__builtin_bswap32((uint32_t)archs[i].cputype);
            uint32_t offset  = __builtin_bswap32(archs[i].offset);
            if (cputype == 0x0100000Cu) { // CPU_TYPE_ARM64
                macho_start = base + offset;
                found = true;
                break;
            }
        }
        if (!found) { munmap((void*)base, file_size); return 0; }
    }

    const struct mach_header_64* mh = (const struct mach_header_64*)macho_start;
    if (mh->magic != MH_MAGIC_64) { munmap((void*)base, file_size); return 0; }

    // LC_SYMTAB 탐색
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
    if (!symtab) { munmap((void*)base, file_size); return 0; }

    // nlist_64 순회
    const struct nlist_64* syms = (const struct nlist_64*)(macho_start + symtab->symoff);
    const char*            strs = (const char*)(macho_start + symtab->stroff);

    for (uint32_t i = 0; i < symtab->nsyms; i++) {
        uint32_t str_idx = syms[i].n_un.n_strx;
        if (str_idx == 0) continue;
        if (strcmp(strs + str_idx, symbol_name) == 0) {
            uint64_t addr = syms[i].n_value;
            munmap((void*)base, file_size);
            return addr;
        }
    }

    munmap((void*)base, file_size);
    return 0;
}

// ── 심볼 6종 mangled 명 ─────────────────────────────────────────────────────
// (nm -arch arm64 /Applications/Palworld.app/Contents/MacOS/Palworld 기준, 2026-06-22)
static const char* k_sym_g_uobject_array                  = "_GUObjectArray";
static const char* k_sym_fname_tostring_fstring            = "__ZNK5FName8ToStringER7FString";
static const char* k_sym_fname_ctor_wchar                  = "__ZN5FNameC1EPKDs9EFindName";
static const char* k_sym_gmalloc                           = "_GMalloc";
static const char* k_sym_static_construct_object_internal  =
    "__Z30StaticConstructObject_InternalRK32FStaticConstructObjectParameters";
static const char* k_sym_ftext_fstring                     = "__ZN5FTextC2EO7FString";

// ── ps_scan 본체 ────────────────────────────────────────────────────────────
extern "C" bool ps_scan(PsCtx& ctx, PsScanResults& results)
{
    char exe_path[4096] = {};
    if (!get_exe_path(exe_path, sizeof(exe_path))) {
        ctx_log(ctx.error, "[ps_scan] 실행 파일 경로 취득 실패\n");
        return false;
    }

    ctx_log(ctx.verbose, "[ps_scan] 시작\n");

    const intptr_t slide = main_slide();

    // 진단 게이트: UE4SS_MAC_FORCE_AOB 설정 시 심볼 조회를 건너뛰고 AOB 전용 경로를
    // 강제한다(미stripped 팔월드로 stripped 게임을 시뮬레이션 — AOB 런타임 해석을
    // 실게임에서 실증). 기존 UE4SS_MAC_DIAG 게이트와 동일한 패턴, 기본 OFF.
    const bool force_aob = (getenv("UE4SS_MAC_FORCE_AOB") != nullptr);
    if (force_aob)
        ctx_log(ctx.normal, "[ps_scan] UE4SS_MAC_FORCE_AOB=1 — 심볼 우회, AOB 전용 경로 강제\n");

    // 심볼 우선(미stripped) → 실패 시 AOB 폴백(stripped 게임 대응, ADR-6).
    // aob_anchor 가 nullptr 이면 폴백 없음(심볼 전용).
    auto resolve = [&](const char* mangled, const char* aob_anchor) -> void* {
        uint64_t file_vm = force_aob ? 0 : resolve_macho_symbol(exe_path, mangled);
        if (file_vm != 0)
            return reinterpret_cast<void*>(static_cast<uintptr_t>(file_vm) + slide);
        if (aob_anchor) {                                  // 심볼 없음 → AOB 시도
            uintptr_t rt = mac::aob_resolve_runtime(aob_anchor);
            if (rt) {
                ctx_log(ctx.normal, "[ps_scan] AOB로 해석(심볼 우회)\n");
                return reinterpret_cast<void*>(rt);        // 이미 런타임 절대주소
            }
        }
        return nullptr;
    };

    bool all_ok = true;

    // engine_version: Palworld = UE5.1 하드코딩
    // (P4에서 CFBundleVersion / Version.h 파싱으로 대체 예정)
    if (ctx.config.engine_version) {
        results.engine_version.major = 5;
        results.engine_version.minor = 1;
        ctx_log(ctx.normal, "[ps_scan] engine_version = 5.1 (hardcoded)\n");
    }

    if (ctx.config.g_uobject_array) {
        void* addr = resolve(k_sym_g_uobject_array, "GUObjectArray");
        if (addr) {
            results.g_uobject_array = addr;
            ctx_log(ctx.normal, "[ps_scan] GUObjectArray OK\n");
        } else {
            ctx_log(ctx.error, "[ps_scan] GUObjectArray NOT FOUND\n");
            all_ok = false;
        }
    }

    if (ctx.config.fname_tostring_fstring) {
        void* addr = resolve(k_sym_fname_tostring_fstring, "FName::ToString");
        if (addr) {
            results.fname_tostring_fstring = addr;
            ctx_log(ctx.normal, "[ps_scan] FName::ToString OK\n");
        } else {
            ctx_log(ctx.error, "[ps_scan] FName::ToString NOT FOUND\n");
            all_ok = false;
        }
    }

    if (ctx.config.fname_ctor_wchar) {
        void* addr = resolve(k_sym_fname_ctor_wchar, "FName::FName");
        if (addr) {
            results.fname_ctor_wchar = addr;
            ctx_log(ctx.normal, "[ps_scan] FName ctor(wchar) OK\n");
        } else {
            ctx_log(ctx.error, "[ps_scan] FName ctor(wchar) NOT FOUND\n");
            all_ok = false;
        }
    }

    if (ctx.config.gmalloc) {
        void* addr = resolve(k_sym_gmalloc, "GMalloc");
        if (addr) {
            results.gmalloc = addr;
            ctx_log(ctx.normal, "[ps_scan] GMalloc OK\n");
        } else {
            ctx_log(ctx.error, "[ps_scan] GMalloc NOT FOUND\n");
            all_ok = false;
        }
    }

    if (ctx.config.static_construct_object_internal) {
        void* addr = resolve(k_sym_static_construct_object_internal, "StaticConstructObject_Internal");
        if (addr) {
            results.static_construct_object_internal = addr;
            ctx_log(ctx.normal, "[ps_scan] StaticConstructObject_Internal OK\n");
        } else {
            ctx_log(ctx.error, "[ps_scan] StaticConstructObject_Internal NOT FOUND\n");
            all_ok = false;
        }
    }

    if (ctx.config.ftext_fstring) {
        void* addr = resolve(k_sym_ftext_fstring, "FText::FText");
        if (addr) {
            results.ftext_fstring = addr;
            ctx_log(ctx.normal, "[ps_scan] FText ctor(FString) OK\n");
        } else {
            ctx_log(ctx.error, "[ps_scan] FText ctor(FString) NOT FOUND\n");
            all_ok = false;
        }
    }

    if (all_ok) {
        ctx_log(ctx.normal, "[ps_scan] 심볼 6종 해결 완료\n");
    }

    return all_ok;
}
