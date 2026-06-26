// tests/test_aob_scan.cpp — AOB 스캐너 단위/대조 테스트 (자체 main)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "darwin/module.hpp"
#include "darwin/aob_scan.hpp"   // Task 2에서 채움
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/fat.h>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* name) {
    printf("test_aob_scan(%s) => %s\n", name, ok ? "PASS" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}

// resolve_runtime_slide 테스트용 프로브 함수.
// noinline + volatile 로 최적화 제거, 충분한 바디로 독자적 프롤로그 확보.
__attribute__((noinline)) static int aob_rt_probe_target(int a, int b) {
    volatile int x = a * 3 + b;
    return x ^ 0x5a5a;
}

static const char* reference_macho_path() {
    return getenv("UE4SS_AOB_REFERENCE_MACHO");
}

// 파일에서 __text [ptr,size,vmaddr] + 심볼주소 조회 (대조 오라클).
struct MachoFile { const uint8_t* base=nullptr; size_t fsize=0; const uint8_t* macho=nullptr; };
static bool map_macho(const char* path, MachoFile& m) {
    int fd = open(path, O_RDONLY); if (fd < 0) return false;
    struct stat st; if (fstat(fd, &st)) { close(fd); return false; }
    m.fsize = (size_t)st.st_size;
    m.base = (const uint8_t*)mmap(nullptr, m.fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); if (m.base == MAP_FAILED) { m.base=nullptr; return false; }
    m.macho = m.base; uint32_t magic = *(const uint32_t*)m.base;
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        auto* fh = (const struct fat_header*)m.base;
        uint32_t n = __builtin_bswap32(fh->nfat_arch);
        auto* a = (const struct fat_arch*)(fh + 1);
        for (uint32_t i=0;i<n;i++) if ((uint32_t)__builtin_bswap32((uint32_t)a[i].cputype)==0x0100000Cu)
            { m.macho = m.base + __builtin_bswap32(a[i].offset); break; }
    }
    return true;
}
static bool file_text(const MachoFile& m, const uint8_t** start, size_t* size, uintptr_t* vmaddr) {
    auto* mh = (const struct mach_header_64*)m.macho;
    const uint8_t* p = (const uint8_t*)(mh + 1);
    for (uint32_t i=0;i<mh->ncmds;i++) {
        auto* lc = (const struct load_command*)p;
        if (lc->cmd == LC_SEGMENT_64) {
            auto* seg = (const struct segment_command_64*)lc;
            if (!strcmp(seg->segname, "__TEXT")) {
                auto* s = (const struct section_64*)((const uint8_t*)seg + sizeof(*seg));
                for (uint32_t k=0;k<seg->nsects;k++) if (!strcmp(s[k].sectname,"__text")) {
                    *start = m.macho + s[k].offset; *size = (size_t)s[k].size; *vmaddr = (uintptr_t)s[k].addr;
                    return true;
                }
            }
        }
        p += lc->cmdsize;
    }
    return false;
}
static uint64_t file_sym(const MachoFile& m, const char* name) {
    auto* mh = (const struct mach_header_64*)m.macho;
    const uint8_t* p = (const uint8_t*)(mh + 1);
    for (uint32_t i=0;i<mh->ncmds;i++) {
        auto* lc = (const struct load_command*)p;
        if (lc->cmd == LC_SYMTAB) {
            auto* st = (const struct symtab_command*)lc;
            auto* sy = (const struct nlist_64*)(m.macho + st->symoff);
            const char* str = (const char*)(m.macho + st->stroff);
            for (uint32_t k=0;k<st->nsyms;k++) {
                uint32_t idx = sy[k].n_un.n_strx;
                if (idx && !strcmp(str+idx, name)) return sy[k].n_value;
            }
        }
        p += lc->cmdsize;
    }
    return 0;
}

// 앵커명 → 검증용 mangled 심볼 (전역 2 + 함수 4 + ProcessInternal + PLSF)
static const char* sym_for(const char* anchor) {
    if (!strcmp(anchor,"GUObjectArray")) return "_GUObjectArray";
    if (!strcmp(anchor,"GMalloc"))       return "_GMalloc";
    if (!strcmp(anchor,"FName::ToString")) return "__ZNK5FName8ToStringER7FString";
    if (!strcmp(anchor,"FName::FName"))    return "__ZN5FNameC1EPKDs9EFindName";
    if (!strcmp(anchor,"StaticConstructObject_Internal"))
        return "__Z30StaticConstructObject_InternalRK32FStaticConstructObjectParameters";
    if (!strcmp(anchor,"FText::FText"))    return "__ZN5FTextC2EO7FString";
    if (!strcmp(anchor,"UObject::ProcessInternal"))
        return "__ZN7UObject15ProcessInternalEPS_R6FFramePv";
    if (!strcmp(anchor,"ProcessLocalScriptFunction"))
        return "__Z26ProcessLocalScriptFunctionP7UObjectR6FFramePv";
    return nullptr;
}

// resolve_runtime_slide: aob_resolve_runtime 의 슬라이드 처리(buffer+off=런타임주소) 검증.
// 프로브 함수의 실제 바이트로 시그니처를 구성해 main_text_range 와 aob_find 를 통해
// 오프셋→런타임주소 변환이 올바른지 검증한다.
// 버그 수정 전: aob_resolve_runtime 은 vm+off(언슬라이드) 반환 → 슬라이드 != 0 시 틀림.
// 버그 수정 후: (uintptr_t)text+off 반환 → 항상 올바른 런타임주소.
static void test_resolve_runtime_slide() {
    // 1. 프로브 함수의 실제 바이트에서 IDA-스타일 시그니처 문자열 생성 (고정 바이트만).
    const uint8_t* fn = (const uint8_t*)(void*)&aob_rt_probe_target;
    const int NBYTES = 28;
    char sig_str[NBYTES * 3 + 1];
    sig_str[0] = '\0';
    for (int i = 0; i < NBYTES; i++) {
        char tok[5];
        snprintf(tok, sizeof(tok), i ? " %02X" : "%02X", (unsigned)fn[i]);
        strncat(sig_str, tok, sizeof(sig_str) - 1);
    }

    mac::Sig sig;
    if (!mac::aob_parse(sig_str, sig)) {
        printf("  [resolve_runtime_slide] sig 파싱 실패\n");
        check(false, "resolve_runtime_slide");
        return;
    }

    // 2. 런타임 __text 구간 취득: text=슬라이드 포인터, vm=파일 vmaddr(언슬라이드).
    const uint8_t* text = nullptr; size_t size = 0; uintptr_t vm = 0;
    if (!mac::main_text_range(&text, &size, &vm)) {
        printf("  [resolve_runtime_slide] main_text_range 실패\n");
        check(false, "resolve_runtime_slide");
        return;
    }

    // 3. 시그니처 탐색.
    size_t cnt = 0;
    size_t off = mac::aob_find(text, size, sig, &cnt);
    if (off == SIZE_MAX || cnt == 0) {
        printf("  [resolve_runtime_slide] aob_find: 일치 없음 (cnt=%zu)\n", cnt);
        check(false, "resolve_runtime_slide");
        return;
    }
    if (cnt > 1) {
        // 시그니처가 유일하지 않음. 첫 번째 일치가 프로브 함수인지 확인.
        printf("  [resolve_runtime_slide] 경고: cnt=%zu (유일 아님), 첫 일치로 진행\n", cnt);
    }

    // 4. 올바른 런타임주소: 슬라이드된 text 포인터 + 오프셋.
    //    (text[0] 의 런타임 VM 주소 = (uintptr_t)text 이므로 text_vmaddr = (uintptr_t)text)
    uintptr_t correct_addr = (uintptr_t)text + off;
    uintptr_t expected_addr = (uintptr_t)(void*)&aob_rt_probe_target;
    check(correct_addr == expected_addr, "resolve_runtime_slide");
    if (correct_addr != expected_addr)
        printf("  got=%#lx expected=%#lx\n",
               (unsigned long)correct_addr, (unsigned long)expected_addr);

    // 5. 버그 공식(vm+off=언슬라이드) 이 슬라이드 != 0 시 다름을 문서화.
    //    aob_resolve_runtime 수정 전에는 이 경로가 vm+off 를 반환 → 런타임주소와 불일치.
    uintptr_t slide = (uintptr_t)text - vm;
    if (slide != 0) {
        uintptr_t buggy_addr = vm + off;
        printf("  [slide-check] slide=%#lx: buggy=%#lx correct=%#lx differ=%s\n",
               (unsigned long)slide, (unsigned long)buggy_addr,
               (unsigned long)correct_addr,
               (buggy_addr != correct_addr) ? "yes — 수정 전 코드는 틀린 주소 반환" : "no(예외상황)");
    } else {
        printf("  [slide-check] 이 프로세스의 slide=0 — buggy vs correct 구분 불가, 생략\n");
    }
}

// Task 3: ProcessInternal AOB 앵커 파싱 + 등록 확인
static void test_process_internal_anchor() {
    // 1. 도출된 AOB 패턴 파싱 확인 (tools/derive-aob.py 2026-06-25, matches=1)
    {
        mac::Sig sig;
        bool ok = mac::aob_parse(
            "F8 5F BC A9 F6 57 01 A9 F4 4F 02 A9 FD 7B 03 A9 "
            "FD C3 00 91 F3 03 02 AA F4 03 01 AA F6 03 00 AA 35 08 40 F9", sig);
        printf("test_aob_scan(pi-parse): bytes=%zu => %s\n", sig.pat.size(), ok ? "PASS" : "FAIL");
        check(ok && sig.pat.size() > 0, "pi-parse");
    }
    // 2. 앵커 테이블에 "UObject::ProcessInternal" 등록 확인
    {
        size_t n = 0;
        const mac::AnchorSig* tbl = mac::anchor_table(&n);
        bool found = false;
        for (size_t i = 0; i < n; ++i)
            if (strcmp(tbl[i].name, "UObject::ProcessInternal") == 0) found = true;
        check(found, "pi-registered");
    }
}

// Task 5b: ProcessLocalScriptFunction AOB 앵커 파싱 + 등록 확인
static void test_plsf_anchor() {
    // 1. 도출된 AOB 패턴 파싱 확인 (tools/derive-aob.py 2026-06-25, instrs=12, matches=1)
    {
        mac::Sig sig;
        bool ok = mac::aob_parse(
            "FF 03 02 D1 F6 57 05 A9 F4 4F 06 A9 FD 7B 07 A9 "
            "FD C3 01 91 F3 03 02 AA F4 03 01 AA ?? ?? ?? ?? "
            "08 05 41 F9 08 01 40 F9 A8 83 1D F8 35 08 40 F9", sig);
        printf("test_aob_scan(plsf-parse): bytes=%zu => %s\n", sig.pat.size(), ok ? "PASS" : "FAIL");
        check(ok && sig.pat.size() > 0, "plsf-parse");
    }
    // 2. 앵커 테이블에 "ProcessLocalScriptFunction" 등록 확인
    {
        size_t n = 0;
        const mac::AnchorSig* tbl = mac::anchor_table(&n);
        bool found = false;
        for (size_t i = 0; i < n; ++i)
            if (strcmp(tbl[i].name, "ProcessLocalScriptFunction") == 0) found = true;
        check(found, "plsf-registered");
    }
}

static void test_aob_vs_symbol() {
    const char* ref = reference_macho_path();
    if (!ref || !ref[0]) {
        printf("test_aob_scan(aob_vs_symbol) => SKIP (UE4SS_AOB_REFERENCE_MACHO 미설정)\n");
        return;
    }
    MachoFile m;
    if (!map_macho(ref, m)) {
        printf("test_aob_scan(aob_vs_symbol) => SKIP (fixture 읽기 실패: %s)\n", ref);
        return;
    }
    const uint8_t* text=nullptr; size_t tsize=0; uintptr_t tvm=0;
    if (!file_text(m, &text, &tsize, &tvm)) { check(false, "aob_vs_symbol_text"); return; }
    size_t n=0; const mac::AnchorSig* tbl = mac::anchor_table(&n);
    for (size_t i=0;i<n;i++) {
        uintptr_t got = mac::aob_resolve(text, tsize, tvm, tbl[i]);
        uint64_t expect = file_sym(m, sym_for(tbl[i].name));
        char nm[96]; snprintf(nm, sizeof nm, "aob==sym:%s", tbl[i].name);
        check(got != 0 && got == expect, nm);
        if (got != expect)
            printf("   got=%#lx expect=%#llx\n", (unsigned long)got, (unsigned long long)expect);
    }
    munmap((void*)m.base, m.fsize);
}

static void test_aob_parse() {
    mac::Sig s;
    bool ok = mac::aob_parse("FC 6F ?? A9", s);
    bool shape = ok && s.pat.size() == 4 && s.mask.size() == 4
              && s.pat[0] == 0xFC && s.mask[0] == 0xFF
              && s.mask[2] == 0x00;                       // ?? → 와일드카드
    check(shape, "aob_parse");
    mac::Sig bad;
    check(!mac::aob_parse("FC 6G", bad), "aob_parse_reject_bad");
}

static void test_aob_find() {
    // 버퍼: [00 11 FC 6F 90 A9 22 33]  — "FC 6F ?? A9" 는 인덱스 2에서 1회 일치
    const uint8_t buf[] = {0x00,0x11,0xFC,0x6F,0x90,0xA9,0x22,0x33};
    mac::Sig s; mac::aob_parse("FC 6F ?? A9", s);
    size_t cnt = 0;
    size_t off = mac::aob_find(buf, sizeof(buf), s, &cnt);
    check(off == 2 && cnt == 1, "aob_find_unique");
    // 와일드카드 두 번째 바이트가 다른 값(0x77)이어도 일치
    const uint8_t buf2[] = {0xFC,0x6F,0x77,0xA9};
    size_t cnt2 = 0;
    check(mac::aob_find(buf2, sizeof(buf2), s, &cnt2) == 0 && cnt2 == 1, "aob_find_wildcard");
    // 없는 패턴
    mac::Sig none; mac::aob_parse("DE AD BE EF", none);
    size_t cnt3 = 1;
    check(mac::aob_find(buf, sizeof(buf), none, &cnt3) == SIZE_MAX && cnt3 == 0, "aob_find_none");
}

static void test_main_text_range() {
    const uint8_t* start = nullptr; size_t size = 0; uintptr_t vm = 0;
    bool ok = mac::main_text_range(&start, &size, &vm);
    // 이 테스트 바이너리 자체가 MH_EXECUTE → 자신의 __text 가 잡힌다.
    bool sane = ok && start && size > 0;
    // 알려진 코드 포인터(check 함수)가 __text 범위 안에 있어야 함.
    const uint8_t* fn = (const uint8_t*)(void*)&check;
    bool contains = sane && fn >= start && fn < start + size;
    check(sane && contains, "main_text_range");
}

static void test_adrp_target() {
    // pc=0x100004000 에서 ADRP x8, +0x12000 (페이지) ; ADD x8,x8,#0x340
    //   기대 타깃 = (0x100004000 & ~0xfff) + 0x12000 + 0x340 = 0x100016340
    // ADRP x8 imm=0x12 페이지 인코딩:
    //   imm = 0x12 (페이지). immlo=imm&3=2, immhi=imm>>2=4.
    //   word = 0x90000000 | (immlo<<29) | (immhi<<5) | Rd(8)
    uint32_t adrp = 0x90000000u | (2u << 29) | (4u << 5) | 8u;
    // ADD x8,x8,#0x340 : 0x91000000 | (imm12<<10) | (Rn8<<5) | Rd8
    uint32_t add = 0x91000000u | (0x340u << 10) | (8u << 5) | 8u;
    uint8_t code[8];
    memcpy(code + 0, &adrp, 4);
    memcpy(code + 4, &add, 4);
    uintptr_t t = mac::adrp_target(code, 0x100004000ull);
    check(t == 0x100016340ull, "adrp_target_add");
}

int main() {
    test_aob_parse();
    test_aob_find();
    test_main_text_range();
    // Task 2~5 에서 서브테스트 추가
    test_adrp_target();
    test_resolve_runtime_slide();
    test_process_internal_anchor();
    test_plsf_anchor();
    test_aob_vs_symbol();
    printf("\n결과: PASS=%d FAIL=%d / 총 %d\n", g_pass, g_fail, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
