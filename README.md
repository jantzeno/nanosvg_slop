# NanoSVG 2

A native C++23 SVG parser and rasterizer derived from NanoSVG. Floating-point
geometry and calculations use `double`; native objects use smart pointers and
standard containers. The library is distributed as four headers: `nanosvg.hpp`
and `nanosvgrast.hpp` for native C++, and `nanosvg.h` and `nanosvgrast.h` for
the C compatibility API and its implementation. There are no separate library
source or private header files.

```text
C++ callers --------------------> Native parser and rasterizer
C callers -> C adapter ---------> Same native implementation
```

The parser produces cubic Bezier paths transformed into the requested output
units. Supported output units are `px`, `pt`, `pc`, `mm`, `cm`, and `in`; the
default is `px` at 96 DPI. The rasterizer produces straight-alpha RGBA bytes.

## C++23 usage

```cpp
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.hpp>
#include <vector>

int main() {
    auto image = nanosvg::parse_file("icon.svg");
    if (!image) return 1;
    auto rasterizer = nanosvg::create_rasterizer();
    if (!rasterizer) return 1;

    constexpr int width = 64, height = 64, stride = width * 4;
    std::vector<unsigned char> pixels(height * stride);
    auto result = (*rasterizer)->rasterize(**image, pixels, width, height, stride);
    return result ? 0 : 1;
}
```

Copy both `.hpp` files from `src` into your include path. Define the
implementation macros before the first NanoSVG include in exactly one C++23
translation unit; other source files include the headers without those macros.
Defining them after an earlier include is not supported. No library build or
private headers are needed. Parser-only consumers need just `nanosvg.hpp` and
`NANOSVG_IMPLEMENTATION`. The parser and rasterizer implementations can also
be compiled in separate translation units. Define `NANOSVG_ALL_COLOR_KEYWORDS`
alongside the parser implementation macro to enable the complete color table.

Use `nanosvg::parse(svgText)` to parse a `std::string_view` without modifying
its storage. Images are returned as `std::unique_ptr<Image>`. Shapes, paths,
points, and gradient stops are owned values in vectors; paints are a variant
of no paint, a packed color, or a gradient. A path has one initial point and
three points per cubic segment. Copying a `Path` deep-copies its points.
References and spans into an image must not outlive their owner or container
mutations that invalidate them. Packed `Color` values use ABGR bit order;
output buffers contain R, G, B, A bytes in that order.

Both parsing functions also accept `nanosvg::OutputUnit` (`px`, `pt`, `pc`,
`mm`, `cm`, or `in`), for example
`nanosvg::parse(svgText, nanosvg::OutputUnit::mm, 96.0)`. String unit arguments
and the existing defaults remain supported.

Each call uses a private parser that owns its input and working state. Token
views are temporary; returned strings, geometry, and paints own their storage.
File loading transfers its input buffer into the parser. Geometry and token
helpers return values, and completed objects move into the returned image.
The image remains freely editable: callers must maintain relationships such
as points and cached bounds when changing fields. Rendering validates geometry
and enum values before writing pixels and does not modify the image.

Parsing, file loading, rasterizer creation, and rendering return
`std::expected` with `Error::invalid_argument`, `io_error`,
`allocation_failure`, or `size_overflow`. Direct construction of a
`Rasterizer` and ordinary container copies can throw standard allocation
exceptions. Moved-from rasterizers reject rendering; moved-to objects remain
usable. Each rasterizer can be reused sequentially but is not safe for
concurrent calls.

Rendering preserves the image and row padding. Dimensions and stride must be
nonnegative; scale must be finite and positive, and translation must be finite.
The destination must hold `(height - 1) * stride + width * 4` bytes for nonempty
output. Zero width or height is a successful no-op. Invalid arguments leave
the destination untouched; allocation failures may leave partial output, but
the rasterizer remains reusable.

SVG parsing remains permissive. Empty input succeeds, and unsupported or
malformed SVG may produce an empty or partial image. A successful parse is
not a conformance or validity certificate. CSS and gradient-reference inheritance
remain incomplete; explicit `inherit` is supported for line caps, line joins,
fill rules, and paint order. Linear and radial gradients support `pad`, `repeat`,
and `reflect`. Radial gradients support off-center focal points, with omitted
`fx`/`fy` defaulting independently to `cx`/`cy`. Outside focal points are projected
onto the circle as in SVG 1.1; rays without a forward circle intersection use the
last stop for every spread mode. Double precision does not change the
rasterizer's tessellation or antialiasing policy.

## Build and install

For optional compiled libraries (including the C adapter), use CMake 3.25 or
newer and a C++23 compiler/standard library:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /path/to/prefix
```

Set `BUILD_SHARED_LIBS=ON` for shared libraries and
`NANOSVG_ALL_COLOR_KEYWORDS=ON` for the complete SVG color keyword table.

```cmake
find_package(NanoSVG 2 REQUIRED)
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE NanoSVG::nanosvgrast)
```

`NanoSVG::nanosvg` supports parser-only consumers. `NanoSVG::nanosvgrast`
propagates the parser dependency, include path, and C++23 requirement. The
same target names work with `add_subdirectory`. CMake and Xmake generate two
tiny `.cpp` files in their build directories to compile the native APIs and
C adapters from the headers. When linking these libraries, include the
headers without defining implementation macros. The four installed headers
live under `include/nanosvg` by default and also support single-header use.

Xmake is also supported:

```sh
xmake f -m release -k static
xmake
xmake install -o /path/to/prefix
# Shared libraries / extended color keywords:
xmake f -m release -k shared --all_color_keywords=y
```

Xmake consumers can use `includes("path/to/nanosvg")` and
`add_deps("nanosvgrast")`, or depend on `nanosvg` alone. Set the consuming
C++ target's language to `cxx23`.

## C API and migration from version 1

C callers include `nanosvg.h` / `nanosvgrast.h` and link the compiled libraries,
or supply one C++23 implementation translation unit containing:

```cpp
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
```

This implements both the native APIs and the C adapters. Copy all four
headers for combined parsing/rasterization, or just `nanosvg.h` and
`nanosvg.hpp` for parser-only use with `NANOSVG_IMPLEMENTATION`. The two
implementations can also be compiled separately, defining only the matching
macro before the first NanoSVG include in each translation unit. Keep
implementation macros out of C source files; C declarations remain usable
without a C++ compiler.

The function names, linked graph access, enum values, and deletion conventions
remain available. The adapter copies native parse results into an independently
owned C graph. Every C rasterization call copies the current C graph back to
native data, so valid caller edits are respected. C++ callers incur no adapter
conversion cost.

Version 2 breaks source and binary compatibility where floating-point types
are explicit. Rebuild every consumer and update `float*` point access, local
geometry arrays, callback/function-pointer signatures, and allocation sizes
to `double`. Shared libraries use ABI version 2, and CMake rejects version-1
package requests. `NSVGgradient::stops` is now an owned pointer, with `nstops`
retained; indexed access remains the same.

`NANOSVG_IMPLEMENTATION` and `NANOSVGRAST_IMPLEMENTATION` expand the native
C++23 implementations in the `.hpp` headers, and both native and C adapter
implementations in the `.h` headers. Remove them when linking the compiled
libraries. Remove the obsolete `NANOSVG_CPLUSPLUS` and `NANOSVGRAST_CPLUSPLUS`
macros. Compile C consumers as C and link with the C++ linker or exported
build targets so the C++ runtime is supplied. The C headers themselves remain
usable from C99 or older C++ standards.

`nsvgParse` retains its mutable-buffer signature, although the implementation
now copies input. `nsvgDelete` releases an image graph. `nsvgDuplicatePath`
returns one independent path with `next == NULL`; C callers may continue to
release its points and node with `free`. Deletion accepts null pointers.
C functions contain exceptions and retain null/void failure results; use the
native API for explicit error reporting. C graph lists must be acyclic and
all arrays must be readable for their declared counts. The C rasterizer cannot
verify the allocation size of a raw destination pointer; callers must supply
the required storage.

The OpenGL and PNG C examples now link the libraries instead of expanding
headers. The OpenGL example requires GLFW/OpenGL; the PNG example uses the
bundled `stb_image_write.h`.

## Tests

```sh
CC=clang CXX=clang++ sh tests/run.sh
CC=clang CXX=clang++ sh tests/all.sh
```

See [tests/README.md](tests/README.md) for sanitizer, packaging, allocation
failure, and coverage checks.

## License

[zlib license](LICENSE.txt). Original notices are retained; this is an altered
C++23 version of NanoSVG, not the original single-header C distribution.
