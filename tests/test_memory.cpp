#include "darwin/memory.hpp"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

// 별도 페이지에 victim 함수(add)를 두어 자기발밑 방지
static int run(void) {
    long ps = getpagesize();
    void* page = mmap(nullptr, ps, PROT_READ|PROT_WRITE|PROT_EXEC,
                      MAP_ANON|MAP_PRIVATE|MAP_JIT, -1, 0);
    uint32_t add_ret[2] = {0x0B000020u /*add w0,w1,w0*/, 0xD65F03C0u /*ret*/};
    pthread_jit_write_protect_np(0); memcpy(page, add_ret, 8); pthread_jit_write_protect_np(1);
    sys_icache_invalidate(page, 8);
    using fn = int(*)(int,int);
    int before = ((fn)page)(5,7);                       // 12

    // movz w0,#1337; ret 로 패치
    uint32_t patch[2] = {0x5280A720u, 0xD65F03C0u};
    if (!mac::code_make_writable(page, 8)) { printf("FAIL writable\n"); return 1; }
    memcpy(page, patch, 8);
    if (!mac::code_make_executable(page, 8)) { printf("FAIL exec\n"); return 1; }
    int after = ((fn)page)(5,7);                        // 1337

    printf("test_memory: before=%d after=%d => %s\n", before, after,
           (before==12 && after==1337) ? "PASS" : "FAIL");
    return (before==12 && after==1337) ? 0 : 1;
}
int main(){ return run(); }
