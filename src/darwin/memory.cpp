#include "darwin/memory.hpp"
#include "darwin/log.hpp"
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <libkern/OSCacheControl.h>
namespace mac {
static void span(void* a, size_t l, vm_address_t& base, vm_size_t& size) {
    long ps = getpagesize();
    base = (vm_address_t)((uintptr_t)a & ~((uintptr_t)ps - 1));
    size = ((uintptr_t)a + l) - base;
}
// 로그 없는 raw 보호 변경 — ScopedThreadFreeze 임계영역 안에서 호출(데드락 방지).
bool code_protect_raw(void* a, size_t l, bool writable) {
    vm_address_t b; vm_size_t s; span(a,l,b,s);
    vm_prot_t prot = writable ? (VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY)
                              : (VM_PROT_READ|VM_PROT_EXECUTE);
    kern_return_t kr = vm_protect(mach_task_self(), b, s, FALSE, prot);
    if (!writable) sys_icache_invalidate(a, l); // RX 복구 시 i-cache 무효화
    return kr == KERN_SUCCESS;
}

ScopedThreadFreeze::ScopedThreadFreeze() {
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
        // 정지 실패 — 로그는 아직 아무도 안 멈춰서 안전. 패치는 비동기화로 진행됨.
        mac::logf("ScopedThreadFreeze: task_threads failed (patch UNSYNCHRONIZED)");
        return;
    }
    mach_port_t self = pthread_mach_thread_np(pthread_self());
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        if (threads[i] != self) thread_suspend(threads[i]); // 자신은 정지 금지(데드락)
    }
    // ★여기부터 소멸자까지 로그/malloc 금지
    m_threads = threads;
    m_count   = count;
    m_active  = true;
}

ScopedThreadFreeze::~ScopedThreadFreeze() {
    if (!m_active) return;
    auto threads = reinterpret_cast<thread_act_array_t>(m_threads);
    mach_port_t self = pthread_mach_thread_np(pthread_self());
    for (mach_msg_type_number_t i = 0; i < m_count; i++) {
        if (threads[i] != self) thread_resume(threads[i]);
        mach_port_deallocate(mach_task_self(), threads[i]); // task_threads가 준 send right 정리
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, m_count * sizeof(thread_act_t));
}
bool code_make_writable(void* a, size_t l) {
    vm_address_t b; vm_size_t s; span(a,l,b,s);
    kern_return_t kr = vm_protect(mach_task_self(), b, s, FALSE,
                                  VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY);
    if (kr) mac::logf("vm_protect(W) kr=%d %s", kr, mach_error_string(kr));
    return kr == KERN_SUCCESS;
}
bool code_make_executable(void* a, size_t l) {
    vm_address_t b; vm_size_t s; span(a,l,b,s);
    kern_return_t kr = vm_protect(mach_task_self(), b, s, FALSE, VM_PROT_READ|VM_PROT_EXECUTE);
    sys_icache_invalidate(a, l);
    if (kr) mac::logf("vm_protect(X) kr=%d %s", kr, mach_error_string(kr));
    return kr == KERN_SUCCESS;
}
void* jit_alloc(const void* code, size_t len) {
    long ps = getpagesize();
    void* p = mmap(nullptr, (size_t)ps, PROT_READ|PROT_WRITE|PROT_EXEC,
                   MAP_ANON|MAP_PRIVATE|MAP_JIT, -1, 0);
    if (p == MAP_FAILED) { mac::logf("mmap(JIT) failed"); return nullptr; }
    pthread_jit_write_protect_np(0);
    memcpy(p, code, len);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(p, len);
    return p;
}
}
