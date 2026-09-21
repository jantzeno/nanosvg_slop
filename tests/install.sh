#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
repo=$(pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir "$tmp/consumer"
cat > "$tmp/consumer/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.10)
project(NanoSVGConsumer C)
find_package(NanoSVG REQUIRED)
foreach(name nanosvg nanosvgrast)
    get_target_property(location NanoSVG::${name} IMPORTED_LOCATION_RELEASE)
    get_filename_component(directory "${location}" DIRECTORY)
    get_target_property(type NanoSVG::${name} TYPE)
    if(NOT directory STREQUAL EXPECTED_LIBDIR OR NOT EXISTS "${location}")
        message(FATAL_ERROR "Unexpected library location: ${location}")
    endif()
    if(NOT type STREQUAL EXPECTED_TYPE)
        message(FATAL_ERROR "Unexpected library type: ${type}")
    endif()
endforeach()
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE NanoSVG::nanosvg NanoSVG::nanosvgrast)
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

for layout in default lib lib64; do
    for linkage in default shared; do
        build="$tmp/build-$layout-$linkage"
        prefix="$tmp/prefix-$layout-$linkage"
        mkdir "$build"
        set -- "-DCMAKE_INSTALL_PREFIX=$prefix" -DCMAKE_BUILD_TYPE=Release
        if [ "$layout" != default ]; then
            set -- "$@" "-DCMAKE_INSTALL_LIBDIR=$layout"
        fi
        type=STATIC_LIBRARY
        if [ "$linkage" = shared ]; then
            set -- "$@" -DBUILD_SHARED_LIBS=ON
            type=SHARED_LIBRARY
        fi
        (cd "$build" && cmake "$repo" "$@")
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
