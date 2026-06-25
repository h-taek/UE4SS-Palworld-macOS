-- First party dependencies
includes("first")
-- Third party dependencies
includes("third")
-- Third party dependencies repository
-- Everything that is an xmake package should be inside of this repository
add_repositories("third-party deps/third-repo", { rootdir = get_config("ue4ssRoot") })

add_requires("zycore v1.5.0", { debug = is_mode_debug(), configs = {runtimes = get_mode_runtimes()} })
add_requires("zydis v4.1.0", { debug = is_mode_debug(), configs = {runtimes = get_mode_runtimes()} })
-- PolyHook2 is x86-64 only and its cmake build fails on macOS. On macOS inline
-- hooking is stubbed (P2) and replaced by our own arm64 hooker in P4, so the
-- package is excluded and PLH:: usage is satisfied by stub headers
-- (deps/first/Unreal/macos_stubs/polyhook2/*). zydis still builds on macOS and
-- is kept (ASMHelper depends on it).
if not is_plat("macosx") then
    add_requires("polyhook_2", { debug = is_mode_debug(), configs = {runtimes = get_mode_runtimes()} })
end