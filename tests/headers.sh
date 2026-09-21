#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
repo=$(pwd)
: "${CC:=cc}"
: "${CXX:=c++}"
: "${CFLAGS:=-O1 -g}"
: "${CXXFLAGS:=-O1 -g}"
test "$(ls -A src | sort)" = 'nanosvg.h
nanosvg.hpp
nanosvgrast.h
nanosvgrast.hpp'
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# Only these two files are available to consumers: no source/private headers.
cp src/nanosvg.hpp src/nanosvgrast.hpp "$tmp/"
cat > "$tmp/parser.cpp" <<'EOF'
#include "nanosvg.hpp"
#include "nanosvg.hpp"
#include <cassert>
std::unique_ptr<nanosvg::Image> make_image() {
    auto image = nanosvg::parse("<svg width='2' height='2'><rect width='2' height='2' fill='red'/></svg>");
    assert(image && (*image)->width == 2 && (*image)->shapes.size() == 1);
    assert(nanosvg::parse_file("missing.svg").error() == nanosvg::Error::io_error);
    return std::move(*image);
}
#ifdef TEST_PARSER_ONLY
int main() { return make_image() ? 0 : 1; }
#endif
EOF
cat > "$tmp/raster.cpp" <<'EOF'
#include "nanosvgrast.hpp"
#include "nanosvgrast.hpp"
#include <cassert>
void render(const nanosvg::Image& image) {
    auto rasterizer = nanosvg::create_rasterizer();
    assert(rasterizer);
    std::array<unsigned char, 16> pixels{};
    assert((*rasterizer)->rasterize(image, pixels, 2, 2, 8));
    for (int i = 0; i < 16; i += 4) {
        assert(pixels[i] == (image.shapes.empty() ? 0 : 255));
        assert(pixels[i+1] == 0 && pixels[i+2] == 0);
        assert(pixels[i+3] == (image.shapes.empty() ? 0 : 255));
    }
}
#ifdef TEST_RASTER_ONLY
int main() { render(nanosvg::Image{}); }
#endif
EOF
cat > "$tmp/main.cpp" <<'EOF'
#include "nanosvgrast.hpp"
#ifdef NANOSVG_H
#error "Native headers must not depend on the C API"
#endif
std::unique_ptr<nanosvg::Image> make_image();
void render(const nanosvg::Image&);
int main() { render(*make_image()); }
EOF
cat > "$tmp/implementation.cpp" <<'EOF'
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.hpp"
#include "nanosvgrast.hpp"
EOF
cd "$tmp"
"$CXX" $CXXFLAGS -UNDEBUG -std=c++23 -DNANOSVG_IMPLEMENTATION -DTEST_PARSER_ONLY parser.cpp -o parser
"$CXX" $CXXFLAGS -UNDEBUG -std=c++23 -DNANOSVGRAST_IMPLEMENTATION -DTEST_RASTER_ONLY raster.cpp -o raster
"$CXX" $CXXFLAGS -UNDEBUG -std=c++23 implementation.cpp parser.cpp raster.cpp main.cpp -o combined
"$CXX" $CXXFLAGS -UNDEBUG -std=c++23 -DNANOSVG_IMPLEMENTATION -c parser.cpp -o parser.o
"$CXX" $CXXFLAGS -UNDEBUG -std=c++23 -DNANOSVGRAST_IMPLEMENTATION -c raster.cpp -o raster.o
"$CXX" $CXXFLAGS -UNDEBUG -std=c++23 main.cpp parser.o raster.o -o separate
for test in parser raster combined separate; do
    timeout 10 "./$test"
done
echo 'NanoSVG single headers passed: parser-only, raster-only, combined and separate implementations'

# Add the C adapters only after testing the native headers in isolation.
cp "$repo/src/nanosvg.h" "$repo/src/nanosvgrast.h" .
cat > c_consumer.c <<'EOF'
#include "nanosvg.h"
#include "nanosvg.h"
#ifndef TEST_PARSER_ONLY
#include "nanosvgrast.h"
#include "nanosvgrast.h"
#endif
#include <assert.h>
int main(void) {
    char svg[] = "<svg width='2' height='2'><rect width='2' height='2' fill='red'/></svg>";
    NSVGimage* image = nsvgParse(svg, "px", 96);
    assert(image && image->width == 2 && image->shapes);
#ifndef TEST_PARSER_ONLY
    unsigned char pixels[16] = {0};
    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    assert(rasterizer);
    nsvgRasterize(rasterizer, image, 0, 0, 1, pixels, 2, 2, 8);
    for (int i = 0; i < 16; i += 4) {
        assert(pixels[i] == 255 && pixels[i+3] == 255);
        assert(pixels[i+1] == 0 && pixels[i+2] == 0);
    }
    nsvgDeleteRasterizer(rasterizer);
#endif
    nsvgDelete(image);
    return 0;
}
EOF
cat > c_implementation.cpp <<'EOF'
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#ifdef TEST_NATIVE_FIRST
#include "nanosvgrast.hpp"
#include "nanosvgrast.h"
#else
#include "nanosvgrast.h"
#include "nanosvgrast.hpp"
#endif
#include "nanosvgrast.h"
#include "nanosvgrast.hpp"
EOF
"$CC" $CFLAGS -UNDEBUG -std=c99 -DTEST_PARSER_ONLY -c c_consumer.c -o c_parser_main.o
"$CC" $CFLAGS -UNDEBUG -std=c99 -c c_consumer.c -o c_main.o
"$CXX" $CXXFLAGS -UNDEBUG -std=c++11 -x c++ -fsyntax-only c_consumer.c
"$CXX" $CXXFLAGS -std=c++23 -DNANOSVG_IMPLEMENTATION -x c++ -c nanosvg.h -o c_parser.o
"$CXX" $CXXFLAGS -std=c++23 -DNANOSVGRAST_IMPLEMENTATION -x c++ -c nanosvgrast.h -o c_raster.o
"$CXX" $CXXFLAGS c_parser_main.o c_parser.o -o c_parser
"$CXX" $CXXFLAGS c_main.o c_parser.o c_raster.o -o c_separate
"$CXX" $CXXFLAGS -std=c++23 c_implementation.cpp c_main.o -o c_first
"$CXX" $CXXFLAGS -std=c++23 -DTEST_NATIVE_FIRST c_implementation.cpp c_main.o -o native_first
for test in c_parser c_separate c_first native_first; do
    timeout 10 "./$test"
done
for header in nanosvg nanosvgrast; do
    macro=$(printf "%s_IMPLEMENTATION" "$header" | tr '[:lower:]' '[:upper:]')
    if "$CC" $CFLAGS -std=c99 -D"$macro" -x c -fsyntax-only "$header.h" > diagnostic.log 2>&1; then
        echo 'C implementation unexpectedly compiled without C++23' >&2
        exit 1
    fi
    grep -q 'requires C++23' diagnostic.log
done
echo 'NanoSVG C headers passed: C99/C++11 declarations, C++23 implementations and mixed include order'
