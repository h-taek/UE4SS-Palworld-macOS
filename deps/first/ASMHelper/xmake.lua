local projectName = "ASMHelper"

target(projectName)
    set_kind("static")
    set_languages("cxx20")
    set_exceptions("cxx")
    add_rules("ue4ss.dependency")

    add_includedirs("include", { public = true })
    add_headerfiles("include/**.hpp")

    add_files("src/**.cpp")
    
    add_deps("File", "DynamicOutput", "Constructs")
    -- ASMHelper.hpp exposes Zydis types in its public API, so consumers of this
    -- header (e.g. UE4SS/CustomProperty.cpp) need the zydis include dir. On macOS
    -- export it publicly so the include propagates transitively.
    if is_plat("macosx") then
        add_packages("zydis", { public = true })
    else
        add_packages("zydis")
    end