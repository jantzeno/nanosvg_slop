#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
repo=$(pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir "$tmp/consumer"
cat > "$tmp/consumer/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.25)
project(NanoSVGConsumer C CXX)
set(target_prefix "")
if(NANOSVG_SOURCE_DIR)
    add_subdirectory("${NANOSVG_SOURCE_DIR}" nanosvg-build)
else()
    find_package(NanoSVG 2.0.0 EXACT REQUIRED)
    set(target_prefix NanoSVG::)
endif()
foreach(name nanosvg nanosvgrast)
    if(NOT NANOSVG_SOURCE_DIR)
        string(TOUPPER "${CMAKE_BUILD_TYPE}" configuration)
        get_target_property(location ${target_prefix}${name} IMPORTED_LOCATION_${configuration})
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
enable_testing()
foreach(language c cpp)
    configure_file("${NSVG_TEST_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/functional.${language}" COPYONLY)
    add_executable(consumer_${language} "${CMAKE_CURRENT_BINARY_DIR}/functional.${language}")
    # This must propagate both the parser link dependency and its includes.
    target_link_libraries(consumer_${language} PRIVATE ${target_prefix}nanosvgrast)
    set_target_properties(consumer_${language} PROPERTIES C_STANDARD 99 CXX_STANDARD 23)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/scratch-${language}")
    add_test(NAME api_${language} COMMAND consumer_${language} "${CMAKE_CURRENT_BINARY_DIR}/scratch-${language}")
    set_tests_properties(api_${language} PROPERTIES TIMEOUT 10)
endforeach()
add_executable(parser_only main.c)
target_link_libraries(parser_only PRIVATE ${target_prefix}nanosvg)
add_test(NAME parser_only COMMAND parser_only)
set_tests_properties(parser_only PROPERTIES TIMEOUT 10)
get_filename_component(test_dir "${NSVG_TEST_SOURCE}" DIRECTORY)
add_executable(native "${test_dir}/native.cpp")
target_link_libraries(native PRIVATE ${target_prefix}nanosvgrast)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/scratch-native")
add_test(NAME native COMMAND native "${CMAKE_CURRENT_BINARY_DIR}/scratch-native")
set_tests_properties(native PROPERTIES TIMEOUT 10)
add_executable(native_parser main.cpp)
target_link_libraries(native_parser PRIVATE ${target_prefix}nanosvg)
add_test(NAME native_parser COMMAND native_parser)
set_tests_properties(native_parser PROPERTIES TIMEOUT 10)
EOF
cat > "$tmp/consumer/main.c" <<'EOF'
#include <nanosvg.h>
#include <nanosvg.h>

int main(void)
{
    char svg[] = "<svg width='16' height='16'><defs><linearGradient id='g'>"
        "<stop stop-color='red'/></linearGradient></defs>"
        "<rect width='16' height='16' fill='url(#g)' fill-opacity='.25'/></svg>";
    NSVGimage* image = nsvgParse(svg, "px", 96);
    int ok = image && image->shapes && image->shapes->fill.type == NSVG_PAINT_LINEAR_GRADIENT &&
        (image->shapes->fill.gradient->stops[0].color >> 24) == 63;
    nsvgDelete(image);
    return ok ? 0 : 1;
}
EOF

cat > "$tmp/consumer/main.cpp" <<'EOF'
#include <nanosvg.hpp>
#ifdef NANOSVG_H
#error "Native parser depends on C API"
#endif
int main() {
    auto image = nanosvg::parse("<svg width='16777217' height='1'/>");
    return image && (*image)->width == 16777217.0 ? 0 : 1;
}
EOF

for config in Debug Release; do
for layout in subdirectory default lib lib64; do
    for linkage in default shared; do
        build="$tmp/build-$config-$layout-$linkage"
        prefix="$tmp/prefix $config $layout $linkage"
        mkdir "$build"
        set -- "-DCMAKE_INSTALL_PREFIX=$prefix" "-DCMAKE_BUILD_TYPE=$config"
        type=STATIC_LIBRARY
        if [ "$linkage" = shared ]; then
            set -- "$@" -DBUILD_SHARED_LIBS=ON
            type=SHARED_LIBRARY
        fi
        if [ "$layout" = subdirectory ]; then
            (cd "$build" && cmake "$tmp/consumer" "$@" \
                "-DNANOSVG_SOURCE_DIR=$repo" "-DEXPECTED_TYPE=$type" \
                "-DNSVG_TEST_SOURCE=$repo/tests/functional.c")
            cmake --build "$build" --config "$config"
            (cd "$build" && ctest --output-on-failure -C "$config")
            echo "NanoSVG subdirectory passed: $config, $linkage (C API, native API, parser-only)"
            continue
        fi
        if [ "$layout" != default ]; then
            set -- "$@" "-DCMAKE_INSTALL_LIBDIR=$layout"
        fi
        if [ "$layout" = lib64 ]; then
            set -- "$@" -DCMAKE_INSTALL_INCLUDEDIR=custom/include
        fi
        (cd "$build" && cmake "$repo" "$@")
        libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:[^=]*=//p' "$build/CMakeCache.txt")
        if [ -z "$libdir" ]; then
            echo "CMAKE_INSTALL_LIBDIR was not initialized" >&2
            exit 1
        fi
        # DESTDIR confines even a regressed absolute destination to scratch space.
        DESTDIR="$tmp/stage" cmake --build "$build" --target install --config "$config"
        while IFS= read -r path; do
            case "$path" in
                "$prefix"/*) ;;
                *) echo "Install escaped prefix: $path" >&2; exit 1 ;;
            esac
        done < "$build/install_manifest.txt"
        installed="$tmp/installed $config $layout $linkage"
        mv "$tmp/stage$prefix" "$installed"
        includedir=$(sed -n 's/^CMAKE_INSTALL_INCLUDEDIR:[^=]*=//p' "$build/CMakeCache.txt")
        test -n "$includedir"
        test -f "$installed/$includedir/nanosvg/nanosvg.h"
        test -f "$installed/$includedir/nanosvg/nanosvgrast.h"
        test -f "$installed/$includedir/nanosvg/nanosvg.hpp"
        test -f "$installed/$includedir/nanosvg/nanosvgrast.hpp"
        test "$(ls -A "$installed/$includedir/nanosvg" | sort)" = 'nanosvg.h
nanosvg.hpp
nanosvgrast.h
nanosvgrast.hpp'
        cat > "$tmp/check-version.cmake" <<EOF
set(PACKAGE_FIND_VERSION 1.0)
set(PACKAGE_FIND_VERSION_MAJOR 1)
include("$installed/$libdir/cmake/NanoSVG/NanoSVGConfigVersion.cmake")
if(PACKAGE_VERSION_COMPATIBLE)
    message(FATAL_ERROR "Version 1 must not match the double-precision ABI")
endif()
EOF
        cmake -P "$tmp/check-version.cmake"
        test -f "$installed/$libdir/cmake/NanoSVG/NanoSVGConfig.cmake"
        test -f "$installed/$libdir/cmake/NanoSVG/NanoSVGConfigVersion.cmake"
        test -f "$installed/$libdir/cmake/NanoSVG/NanoSVGTargets.cmake"
        consumer="$tmp/consumer-$config-$layout-$linkage"
        mkdir "$consumer"
        (cd "$consumer" && cmake "$tmp/consumer" \
            "-DCMAKE_PREFIX_PATH=$installed" \
            "-DEXPECTED_LIBDIR=$installed/$libdir" "-DEXPECTED_TYPE=$type" \
            "-DCMAKE_BUILD_TYPE=$config" "-DNSVG_TEST_SOURCE=$repo/tests/functional.c" \
            -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON)
        cmake --build "$consumer" --config "$config"
        (cd "$consumer" && ctest --output-on-failure -C "$config")
        echo "NanoSVG install passed: $config, $layout ($libdir), $linkage (C API, native API, parser-only)"
    done
done
done
test "$(ls -A src | sort)" = 'nanosvg.h
nanosvg.hpp
nanosvgrast.h
nanosvgrast.hpp'
