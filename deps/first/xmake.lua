-- macOS arm64 inline hooker — compiled from src/hook/*.cpp + src/darwin/*.cpp.
-- Provides mac::install_hook, which the x64Detour adapter (macos_stubs) calls.
-- Must be defined before Unreal so it can be used as a dep there.
if is_plat("macosx") then
    target("MacHook")
        set_kind("static")
        set_languages("cxx20")
        add_rules("ue4ss.dependency")
        add_files("$(projectdir)/src/hook/*.cpp", "$(projectdir)/src/darwin/*.cpp")
        -- Expose src/ so consumers can #include "hook/inline_hook.hpp" etc.
        add_includedirs("$(projectdir)/src", {public = true})
        -- Capstone (disassembler used by arm64_reloc.cpp) — resolved via brew at
        -- config time; must be in on_load because os.iorun is script-scope only.
        -- includedirs만 public 전파; 실제 링크는 UE4SS xmake.lua에서
        -- capstone .a를 ldflags로 직접 추가해 self-contained dylib 생성.
        on_load(function(target)
            local capstone_prefix = os.iorun("brew --prefix capstone"):trim()
            target:add("includedirs", capstone_prefix .. "/include", {public = true})
        end)
    target_end()
end

-- includes("ArgsParser")
includes("ASMHelper")
includes("Constructs")
includes("DynamicOutput")
includes("File")
includes("Function")
includes("Helpers")
includes("IniParser")
includes("Input")
includes("JSON")
includes("LuaMadeSimple")
includes("LuaRaw")
includes("MProgram")
includes("ParserBase")
includes("Profiler")
includes("ScopedTimer")
includes("SinglePassSigScanner")
includes("Unreal")

-- patternsleuth is a Rust AOB scanner. macOS does not use Rust (the Palworld
-- macOS binary is not stripped, so symbols are resolved from the Mach-O symbol
-- table in P3). On macOS we replace it with a C++ static lib providing the same
-- `ps_scan` ABI symbol as a link-time stub. See deps/first/patternsleuth_bind/macos/ps_scan.cpp.
if not is_plat("macosx") then
    includes("patternsleuth_bind")

    if is_config("patternsleuth", "local") then
        -- The patternsleuth target is managed by the cargo.build rule.
        target("patternsleuth")
            set_kind("static")
            add_rules("cargo.build", {project_name = "patternsleuth", is_debug = is_mode_debug(), features= { "process-internal" }})
            add_files("patternsleuth/Cargo.toml")
            -- Exposes the rust *.rs files to the Visual Studio project filters.
            add_extrafiles("patternsleuth/**.rs")
    end

    add_requires("cargo::patternsleuth_bind", { debug = is_mode_debug(), configs = { cargo_toml = path.join(os.scriptdir(), "patternsleuth_bind/Cargo.toml"), runtimes = get_mode_runtimes() } })

    target("patternsleuth_bind")
        set_kind("static")
        set_values("rust.cratetype", "staticlib")
        set_values("rust.edition", "2021")
        add_files("patternsleuth_bind/src/lib.rs")
        if is_plat("linux") and is_host("windows") then
            add_rcflags("--target=x86_64-unknown-linux-gnu", {force = true})
        end
        add_packages("cargo::patternsleuth_bind")
        if is_plat("windows") then
            add_links("ws2_32", "advapi32", "userenv", "ntdll", "oleaut32", "bcrypt", "ole32", { public = true })
        end
else
    -- macOS: C++ replacement for the Rust patternsleuth_bind crate (link-time stub).
    target("patternsleuth_bind")
        set_kind("static")
        set_languages("cxx20")
        add_rules("ue4ss.dependency")
        add_deps("MacHook")   -- ★추가: aob_scan(+module) 링크 + $(projectdir)/src include 전파
        add_files("patternsleuth_bind/macos/ps_scan.cpp")
end
-- Patternsleuth -> END
