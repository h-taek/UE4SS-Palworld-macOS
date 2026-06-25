// macOS (arm64) entry/platform shim.
//
// Provides the two Platform.hpp functions (called from UE4SSProgram.cpp) and a
// DYLD_INSERT_LIBRARIES constructor that bootstraps UE4SS on a worker thread.
//
// Linux entry uses __libc_start_main hooking + ELF __cxa_throw patching + LD_PRELOAD
// removal — none of that applies on macOS. DYLD already injected us before game main
// runs, so the constructor fires at the right time.

#include <cstdio>
#include <filesystem>
#include <vector>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>

#include "DynamicOutput/Output.hpp"
#include "Platform.hpp"
#include "UE4SSProgram.hpp"

using namespace RC;

static pthread_t ue4ss_worker_thread;
static bool UE4SSInited = false;

// ---------------------------------------------------------------------------
// Platform.hpp implementations
// ---------------------------------------------------------------------------

std::filesystem::path get_executable_path()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, 0);
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
    {
        return {};
    }
    return std::filesystem::canonical(std::filesystem::path(buf.data()));
}

void add_dlsearch_folder(std::filesystem::path&)
{
    // macOS no-op (same as Linux).
}

// ---------------------------------------------------------------------------
// Worker thread: creates UE4SSProgram and calls init().
// init() calls setup_unreal() which calls UnrealInitializer::initialize(),
// which contains a while(true) retry loop around ps_scan — so it polls until
// the game engine is ready. pthread_detach keeps game main unblocked.
// ---------------------------------------------------------------------------

static void* ue4ss_worker(void* arg)
{
    fprintf(stderr, "[UE4SS-mac] worker start\n");
    fflush(stderr);

    auto* pathPtr = reinterpret_cast<SystemStringType*>(arg);

    auto* program = new UE4SSProgram(*pathPtr, {});
    delete pathPtr;
    UE4SSInited = true;

    const int has_error = program->get_error_object()->has_error() ? 1 : 0;
    fprintf(stderr, "[UE4SS-mac] program constructed (has_error=%d)\n", has_error);
    fflush(stderr);

    if (!has_error)
    {
        fprintf(stderr, "[UE4SS-mac] init() entering\n");
        fflush(stderr);

        program->init(); // blocks on m_event_loop.join() — intentional on worker

        // If we return here UE4SS shut down (or init() threw internally).
        fprintf(stderr, "[UE4SS-mac] init() returned\n");
        fflush(stderr);
    }

    if (auto e = program->get_error_object(); e->has_error())
    {
        if (!Output::has_internal_error())
        {
            Output::send<LogLevel::Error>(SYSSTR("Fatal Error: {}\n"), e->get_message());
        }
        else
        {
            fprintf(stderr, "UE4SS Fatal Error: %s\n", e->get_message());
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// DYLD_INSERT_LIBRARIES constructor — fired before game main().
// ---------------------------------------------------------------------------

__attribute__((constructor)) static void ue4ss_darwin_ctor()
{
    Dl_info dl_info;
    if (dladdr(reinterpret_cast<void*>(&ue4ss_darwin_ctor), &dl_info) == 0)
    {
        fprintf(stderr, "[UE4SS] dladdr failed — cannot determine dylib path\n");
        return;
    }

    // SystemStringType = std::string on macOS (non-WIN32, non-ANSI branch in Macros.hpp)
    // dl_info.dli_fname is const char* — same to_system_string path as EntryLinux.
    auto* pathPtr = new SystemStringType(to_system_string(dl_info.dli_fname));

    if (pthread_create(&ue4ss_worker_thread, nullptr, ue4ss_worker, pathPtr) != 0)
    {
        fprintf(stderr, "[UE4SS] pthread_create failed\n");
        delete pathPtr;
        return;
    }

    // Detach so the game main thread is not blocked; ue4ss_worker blocks on
    // init() → m_event_loop.join() until UE4SS shuts down.
    pthread_detach(ue4ss_worker_thread);
}

// ---------------------------------------------------------------------------
// Destructor — cleanup when dylib is unloaded (game exit).
// ---------------------------------------------------------------------------

__attribute__((destructor)) static void ue4ss_darwin_dtor()
{
    if (UE4SSInited)
    {
        UE4SSProgram::static_cleanup();
        UE4SSInited = false;
    }
}
