#include "hook/inline_hook.hpp"
#include "darwin/memory.hpp"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <libkern/OSCacheControl.h>

using fn = int(*)(int,int);
static fn g_tramp = nullptr;
__attribute__((noinline)) static int my_hook(int a, int b){ return g_tramp(a,b) + 1000; }

int main() {
    // victim: add w0,w1,w0 ; nop ; nop ; nop ; ret  (앞 16B = 패치 자리)
    uint32_t body[5] = {0x0B000020u, 0xD503201Fu, 0xD503201Fu, 0xD503201Fu, 0xD65F03C0u};
    void* V = mac::jit_alloc(body, sizeof body);
    int before = ((fn)V)(5,7);                          // 12

    mac::Hook hk = mac::install_hook(V, (void*)my_hook);
    if (!hk.ok) { printf("test_inline_hook: install FAIL\n"); return 1; }
    g_tramp = (fn)hk.trampoline;

    int after = ((fn)V)(5,7);                           // 1012
    printf("test_inline_hook: before=%d after=%d => %s\n", before, after,
           (before==12 && after==1012) ? "PASS" : "FAIL");
    return (before==12 && after==1012) ? 0 : 1;
}
