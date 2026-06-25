#pragma once
#include <cstdint>
namespace mac {
struct Hook {
    void* target=nullptr;
    void* trampoline=nullptr;
    bool  ok=false;
    uint8_t original[16]={};   // saved prologue bytes — enables rollback & future uninstall
};
Hook install_hook(void* target, void* hook_fn);
}
