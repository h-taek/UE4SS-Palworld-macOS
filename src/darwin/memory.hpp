#pragma once
#include <cstddef>
namespace mac {
bool code_make_writable(void* addr, size_t len);
bool code_make_executable(void* addr, size_t len);
// 로그/malloc 없는 raw 보호 변경(스레드 정지 임계영역 전용).
// writable=true → RW(+COPY), false → RX(+icache invalidate). 성공 시 true.
bool code_protect_raw(void* addr, size_t len, bool writable);
void* jit_alloc(const void* code, size_t len);

// 코드 패치 동안 자신을 제외한 모든 스레드를 일시정지(thread_suspend)해
// W^X 과도기(RW↔RX 전환·memcpy) 중 다른 스레드가 패치 중인 코드를 실행하다
// BUS 오류로 죽는 레이스를 막는다(RAII). ★임계영역(생성~소멸) 안에서는
// 로그/malloc 금지 — 정지된 스레드가 그 락을 쥐고 있으면 데드락.
class ScopedThreadFreeze {
public:
    ScopedThreadFreeze();
    ~ScopedThreadFreeze();
    ScopedThreadFreeze(const ScopedThreadFreeze&) = delete;
    ScopedThreadFreeze& operator=(const ScopedThreadFreeze&) = delete;
    bool active() const { return m_active; }
private:
    void*        m_threads = nullptr; // thread_act_array_t (opaque)
    unsigned int m_count   = 0;       // mach_msg_type_number_t
    bool         m_active  = false;
};
}
