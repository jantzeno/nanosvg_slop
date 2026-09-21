From the repository root, run the parser, rasterizer, and public API suites:

```sh
CC=clang sh tests/run.sh
```

Run every header configuration and the CMake consumer matrix:

```sh
CC=clang CXX=clang++ sh tests/all.sh
```

The scripts require a POSIX shell, `mktemp`, and `timeout`. The full run also
needs CMake 3.25 or newer, CTest, a C compiler, and a C++ compiler supporting
C++23. All builds, fixtures, and installations use temporary directories.
Each test executable has a ten-second timeout. Assertions remain active in
Release builds even when the compiler defines `NDEBUG`.

`run.sh` enables AddressSanitizer, UndefinedBehaviorSanitizer, and
float-cast-overflow checks. Leak detection defaults to off for environments
that execute under tracing; set `ASAN_OPTIONS=detect_leaks=1` to enable it
elsewhere. Override `CC` and `CFLAGS` for other configurations:

```sh
CC=clang++ CFLAGS='-x c++ -std=c++11 -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all' sh tests/run.sh
CC=clang CFLAGS='-std=c99 -O1 -g -fsanitize=memory -fsanitize-memory-track-origins=2 -fPIE -pie' sh tests/run.sh
```

`all.sh` runs both suites in five header configurations: C99, C99 with all
color keywords, C++11 with C linkage, C++23 with all color keywords and C
linkage, and C++23 with the optional `NANOSVG_CPLUSPLUS` and
`NANOSVGRAST_CPLUSPLUS` switches. Its `CFLAGS` override is shared across
languages; omit language-selection flags when using that runner. CMake
consumer builds exercise ordinary Debug/Release settings without sanitizers.

The coverage inventory is:

| Area | Required checks |
| --- | --- |
| Geometry | Every primitive; absolute and relative path commands; implicit repetition; smooth control points; quadratic conversion; arc flags, radii, and endpoints; closure; tight bounds; degenerate shapes |
| Coordinates | Physical and font units; 72/96/144 DPI; output units; percentages with and without a viewBox; all nine explicit alignments with meet/slice in both viewport orientations; nonzero origins; inferred dimensions; nested transforms |
| Styles | Common and extended colors; malformed color fallback; opacity clamping; CSS classes and inline styles; inheritance; independent display and visibility; identifiers |
| Gradients | Linear/radial paints; one to three stops; interpolation and endpoint padding; sorted stops; forward/shared/missing/cyclic references; spread metadata; stop/fill/stroke/shape opacity; all 256 alpha levels for solid/linear/radial fills and strokes; normalized radial focus |
| Rasterizer | Fill winding rules; all caps and joins; miter fallback; all paint-order permutations; straight-alpha blending; dashes and negative offsets; antialias coverage; translation, scaling, and clipping; renderer reuse; deterministic output; unchanged input paths |
| Memory and API | Mutable input lifetime; memory/file equivalence; empty and missing files; path deep copies and independent ownership; null deletion; padded strides and guarded buffers; malformed/truncated input; bounded strings; multiple shapes |
| Historical regressions | Numeric overflow and nonfinite values; singular transforms; bounded CSS recursion; malformed transforms; degenerate arcs/gradients/dashes; all three bundled SVG examples |
| Build and packaging | C and C++ callers; parser-only and rasterizer consumers; static/shared libraries; Debug/Release; source subdirectories; installed and relocated packages; default/lib/lib64 directories; custom includes; paths containing spaces; package version and exported dependency propagation |

Run the CMake portion alone with:

```sh
CC=clang CXX=clang++ sh tests/install.sh
```

This runs 16 configurations: Debug/Release × source-subdirectory/default/lib/lib64
× default-static/shared. Each runs three CTest executables (48 executions):
the public API suite as C, the same suite as C++, and a parser-only C caller.
The rasterizer consumers link only `nanosvgrast`, so its parser dependency and
include directories must propagate. Imported target names remain
`NanoSVG::nanosvg` and `NanoSVG::nanosvgrast`.

The `lib64` cases also use `CMAKE_INSTALL_INCLUDEDIR=custom/include`.
Generated `.c` files must exactly match their implementation headers. Installs
use `DESTDIR`, and every manifest entry must remain under the selected prefix.
Consumers use `find_package(NanoSVG 1.0 EXACT REQUIRED)` after relocation.

For the modern C++ rewrite, `functional.c` is the portable acceptance suite.
It calls only public APIs and can compile without either implementation macro:

```sh
scratch=$(mktemp -d)
cc -std=c99 -DNSVG_TEST_EXTERNAL -I/path/to/include/nanosvg \
    tests/functional.c -L/path/to/lib -lnanosvgrast -lnanosvg -lm \
    -o "$scratch/functional"
"$scratch/functional" "$scratch"
rm -rf "$scratch"
```

For a static C++ implementation, link that C-compiled object with the C++
linker or the exported CMake targets so the C++ runtime is supplied. Shared
libraries must be on the platform's runtime search path. CMake consumers
already compile C and C++ callers separately from the library.

The compatibility boundary is **source compatibility**, including the public
names, callable signatures, enum meanings, accessible fields, and ownership
rules. There are no fixed `sizeof`, alignment, or member-offset assertions;
callers must rebuild against the replacement headers. `nsvgParse` receives a
writable NUL-terminated buffer; returned images must outlive that buffer.
`nsvgDelete` releases the image graph. `nsvgDuplicatePath` produces a single
independent path whose points and path allocation the caller can release
with `free`. Rasterization writes non-premultiplied RGBA into caller-owned
storage, honors row stride, and preserves the image geometry. Do not expose
C++ exceptions, STL types, or C++ allocation ownership through this C API.

Geometry comparisons use numerical tolerances, and interpolated pixel tests
allow small rounding differences instead of freezing whole-image hashes.
Opaque interior colors, buffer guards, ownership, and exact integer geometry
remain strict checks. Preserve these behavioral tests during the rewrite.
`regression.c` additionally calls private `nsvg__` helpers; those checks belong
to the current implementation and must be adapted or retired when its
internals change. The header-only configuration and generated-source checks
also describe current packaging, not a required C++ implementation layout.

The new percentage and vertical-alignment cases are required regressions,
with no expected-failure bypass. Percentages fall back to the declared
viewport when viewBox dimensions are absent, and preserveAspectRatio
recognizes the SVG `YMin`, `YMid`, and `YMax` spellings.

Measure executed source coverage with matching Clang/LLVM tools:

```sh
CC=clang LLVM_PROFDATA=llvm-profdata LLVM_COV=llvm-cov sh tests/coverage.sh
```

This measures both suites in the default C99 header configuration and prints
line, branch, region, and function coverage for the two library headers.
It requires no test framework or persistent build directory. The initial
Clang/LLVM 22.1.8 baseline is:

| Source | Lines | Branches | Functions |
| --- | ---: | ---: | ---: |
| nanosvg.h | 96.76% | 84.81% | 100% |
| nanosvgrast.h | 98.37% | 89.55% | 100% |
| Combined | 97.25% | 85.87% | 100% |

The consumer matrix was verified with CMake 4.3.0 on Linux; CMake 3.25 itself
and other operating systems have not been exercised here. Coverage is a
measure of exercised code, not proof of correctness for every possible SVG.
The suite has broad deterministic cases; it is not full SVG conformance,
allocation-failure injection, fuzzing, or a cross-platform CI service.
The parser's partial CSS/gradient inheritance and the rasterizer's missing
repeat/reflect spread and off-center focal rendering remain outside the
promised feature set. Tests check spread/focus metadata without claiming
those rendering features work.
