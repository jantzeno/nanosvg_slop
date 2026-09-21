#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
repo=$(pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir "$tmp/consumer"
cat > "$tmp/consumer/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.25)
project(NanoSVGConsumer C)
set(target_prefix "")
if(NANOSVG_SOURCE_DIR)
    add_subdirectory("${NANOSVG_SOURCE_DIR}" nanosvg-build)
else()
    find_package(NanoSVG REQUIRED)
    set(target_prefix NanoSVG::)
endif()
foreach(name nanosvg nanosvgrast)
    if(NOT NANOSVG_SOURCE_DIR)
        get_target_property(location ${target_prefix}${name} IMPORTED_LOCATION_RELEASE)
        get_filename_component(directory "${location}" DIRECTORY)
        if(NOT directory STREQUAL EXPECTED_LIBDIR OR NOT EXISTS "${location}")
            message(FATAL_ERROR "Unexpected library location: ${location}")
        endif()
    endif()
    get_target_property(type ${target_prefix}${name} TYPE)
    if(NOT type STREQUAL EXPECTED_TYPE)
        message(FATAL_ERROR "Unexpected library type: ${type}")
    endif()
endforeach()
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE ${target_prefix}nanosvg ${target_prefix}nanosvgrast)
enable_testing()
add_test(NAME render COMMAND consumer)
set_tests_properties(render PROPERTIES TIMEOUT 10)
EOF
cat > "$tmp/consumer/main.c" <<'EOF'
#include <nanosvg.h>
#include <nanosvgrast.h>

int main(void)
{
    char svg[] = "<svg width='16' height='16'><defs><linearGradient id='g'>"
        "<stop stop-color='red'/></linearGradient></defs>"
        "<rect width='16' height='16' fill='url(#g)' fill-opacity='.25'/></svg>";
    unsigned char pixels[16*16*4];
    NSVGimage* image = nsvgParse(svg, "px", 96);
    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    int ok = 0;
    if (image && image->shapes && rasterizer) {
        nsvgRasterize(rasterizer, image, 0, 0, 1, pixels, 16, 16, 16*4);
        ok = pixels[(8*16+8)*4+3] == 63;
    }
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);
    return ok ? 0 : 1;
}
EOF

for layout in subdirectory default lib lib64; do
    for linkage in default shared; do
        build="$tmp/build-$layout-$linkage"
        prefix="$tmp/prefix-$layout-$linkage"
        mkdir "$build"
        set -- "-DCMAKE_INSTALL_PREFIX=$prefix" -DCMAKE_BUILD_TYPE=Release
        type=STATIC_LIBRARY
        if [ "$linkage" = shared ]; then
            set -- "$@" -DBUILD_SHARED_LIBS=ON
            type=SHARED_LIBRARY
        fi
        if [ "$layout" = subdirectory ]; then
            (cd "$build" && cmake "$tmp/consumer" "$@" \
                "-DNANOSVG_SOURCE_DIR=$repo" "-DEXPECTED_TYPE=$type")
            cmake --build "$build" --config Release
            (cd "$build" && ctest --output-on-failure -C Release)
            echo "NanoSVG subdirectory passed: $linkage"
            continue
        fi
        if [ "$layout" != default ]; then
            set -- "$@" "-DCMAKE_INSTALL_LIBDIR=$layout"
        fi
        if [ "$layout" = lib64 ]; then
            set -- "$@" -DCMAKE_INSTALL_INCLUDEDIR=custom/include
        fi
        (cd "$build" && cmake "$repo" "$@")
        cmp src/nanosvg.h "$build/nanosvg.c"
        cmp src/nanosvgrast.h "$build/nanosvgrast.c"
        libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:[^=]*=//p' "$build/CMakeCache.txt")
        if [ -z "$libdir" ]; then
            echo "CMAKE_INSTALL_LIBDIR was not initialized" >&2
            exit 1
        fi
        # DESTDIR confines even a regressed absolute destination to scratch space.
        DESTDIR="$tmp/stage" cmake --build "$build" --target install --config Release
        while IFS= read -r path; do
            case "$path" in
                "$prefix"/*) ;;
                *) echo "Install escaped prefix: $path" >&2; exit 1 ;;
            esac
        done < "$build/install_manifest.txt"
        installed="$tmp/installed-$layout-$linkage"
        mv "$tmp/stage$prefix" "$installed"
        includedir=$(sed -n 's/^CMAKE_INSTALL_INCLUDEDIR:[^=]*=//p' "$build/CMakeCache.txt")
        test -n "$includedir"
        test -f "$installed/$includedir/nanosvg/nanosvg.h"
        test -f "$installed/$includedir/nanosvg/nanosvgrast.h"
        test -f "$installed/$libdir/cmake/NanoSVG/NanoSVGConfig.cmake"
        test -f "$installed/$libdir/cmake/NanoSVG/NanoSVGConfigVersion.cmake"
        test -f "$installed/$libdir/cmake/NanoSVG/NanoSVGTargets.cmake"
        consumer="$tmp/consumer-$layout-$linkage"
        mkdir "$consumer"
        (cd "$consumer" && cmake "$tmp/consumer" \
            "-DCMAKE_PREFIX_PATH=$installed" \
            "-DEXPECTED_LIBDIR=$installed/$libdir" "-DEXPECTED_TYPE=$type" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON)
        cmake --build "$consumer" --config Release
        (cd "$consumer" && ctest --output-on-failure -C Release)
        echo "NanoSVG install passed: $layout ($libdir), $linkage"
    done
done
