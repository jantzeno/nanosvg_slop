set_project("NanoSVG")
set_version("2.0.0")
set_languages("c99", "cxx23")
add_rules("mode.debug", "mode.release")

option("all_color_keywords")
    set_default(false)
    set_showmenu(true)
option_end()

target("nanosvg")
    set_kind("$(kind)")
    set_version("2.0.0", {soname = true})
    on_load(function (target)
        local source = path.join(target:autogendir(), "src", "nanosvg.cpp")
        if not os.isfile(source) then
            io.writefile(source, '#define NANOSVG_IMPLEMENTATION\n#include "nanosvg.h"\n')
        end
        target:add("files", source)
    end)
    add_includedirs("src", {public = true})
    add_headerfiles("src/nanosvg.h", "src/nanosvg.hpp", {prefixdir = "nanosvg"})
    if is_plat("linux", "bsd", "android", "mingw") then
        add_syslinks("m", {public = true})
    end
    if has_config("all_color_keywords") then
        add_defines("NANOSVG_ALL_COLOR_KEYWORDS")
    end
target_end()

target("nanosvgrast")
    set_kind("$(kind)")
    set_version("2.0.0", {soname = true})
    on_load(function (target)
        local source = path.join(target:autogendir(), "src", "nanosvgrast.cpp")
        if not os.isfile(source) then
            io.writefile(source, '#define NANOSVGRAST_IMPLEMENTATION\n#include "nanosvgrast.h"\n')
        end
        target:add("files", source)
    end)
    add_deps("nanosvg")
    add_headerfiles("src/nanosvgrast.h", "src/nanosvgrast.hpp", {prefixdir = "nanosvg"})
target_end()
