# NanoSVG 2 checks

Run the native C++23 parser/rasterizer, public C API, and historical regressions:

```sh
CC=clang CXX=clang++ sh tests/run.sh
```

The runner checks standalone C++23 headers, builds the optional libraries,
compiles the public C suite as C99, and uses AddressSanitizer,
UndefinedBehaviorSanitizer, and float-cast-overflow
checks by default. Assertions remain enabled in Release builds. It uses only
compiler tools, a POSIX shell, `mktemp`, and `timeout`; each executable has a
ten-second timeout. All build products and generated fixtures use temporary
directories. Leak detection defaults off for traced environments; enable it
with `ASAN_OPTIONS=detect_leaks=1` where supported. Override `CFLAGS` and
`CXXFLAGS` for alternate instrumentation; language standards are set by the
runner.

On Linux, `failures.cpp` also overrides C++ allocation and wraps C `calloc`
and `free` with the linker's `--wrap` option. It fails each allocation in native parsing,
C graph export, native rendering, and C rendering, checks error results, and
verifies renderer reuse. Allocation counts must return to zero after cleanup,
including when LeakSanitizer cannot run under tracing.

```sh
CC=clang CXX=clang++ sh tests/all.sh
```

The full run repeats the suites with all color keywords, runs the CMake
consumer matrix, and runs Xmake checks when Xmake is installed.

| Area | Coverage |
| --- | --- |
| Geometry | Primitives; absolute/relative path commands; smooth and quadratic curves; arcs; closure; tight bounds; degeneracies |
| Coordinates | Physical/font units; DPI; output units; viewBox percentages and alignment; inferred dimensions; nested transforms |
| Paint | Colors; opacity; class/inline styles; inheritance; visibility; paint order; linear/radial gradients and references |
| Rendering | Fill rules; caps/joins; miter fallback; dashes; antialiasing; blending; translation/scaling/clipping; reuse; padded strides; unchanged geometry |
| Native API | Double precision beyond float range/resolution; long decimals; variant gradients; unique ownership; deep path copies; string-view lifetime; file/error results; invalid buffers; moved rasterizers |
| C adapter | Independent C graph ownership; malloc/free-compatible path copies; C/native output agreement; caller edits to geometry and gradient stops; long graph destruction |
| Failures | Malformed/truncated input; numeric overflow and nonfinite values; singular transforms; CSS/gradient cycles; allocation failure and recovery |

`functional.c` exercises public C behavior through the adapter. Its numeric
fields and helpers now use double. `native.cpp` exercises the native API and
checks adapter parity. `regression.cpp` additionally enables the native header
implementations to test pure numerical and token helpers; those helpers are
not a supported interface. Parser state remains private, with arc regressions
exercised through the public API. The three existing SVG examples remain
smoke-test inputs.

Native checks cover typed and string output units, numeric tokens longer than
63 characters, CSS gradient references longer than 511 characters, finite
stroke widths under large transforms, and owned results after input destruction
and parser container growth. Caller edits remain supported; invalid enum values
are rejected before writing to the destination.

`headers.sh` copies only the two native headers to a temporary directory and
checks parser-only, raster-only, combined, and separate implementation builds.
It then adds the two C headers to check C99 callers, C++11 declarations,
combined/separate C++23 implementations, and rejection of implementation
macros in C. It checks multiple consumer translation units, repeated includes,
and both C/native include orders with implementation macros defined before
the first NanoSVG include.

## Packaging

```sh
CC=clang CXX=clang++ sh tests/install.sh
CC=clang CXX=clang++ sh tests/xmake.sh
```

The CMake matrix covers 16 configurations: Debug/Release × source-subdirectory,
default installation, explicit lib, or explicit lib64 × static/shared.
Each runs five CTest consumers (80 executions): C API from C, C API from C++,
C parser-only, native C++ API, and native parser-only. This checks dependency
and C++23 propagation, version 2.0.0, rejection of version 1, paths containing
spaces, relocation, DESTDIR confinement, and custom include directories.
All four installed headers support single-header use; the C adapter
implementations require C++23. Builds generate implementation translation
units in their build directories. Checks verify that `src` and the installed
header directory contain exactly the four public headers.

Xmake checks Debug/static and Release/shared builds and installed C/native
consumers in a temporary project. It skips with a message if Xmake is absent.
These scripts currently exercise Linux; other operating systems require their
own validation.

## Coverage

```sh
CC=clang CXX=clang++ LLVM_PROFDATA=llvm-profdata LLVM_COV=llvm-cov sh tests/coverage.sh
```

Use matching Clang/LLVM versions. This reports native parser/rasterizer and
C adapter coverage from all three behavioral suites. The old version-1 header
coverage percentages are not a baseline for the rewritten implementation.

Geometry comparisons use tolerances; interpolated pixels permit small
rounding differences. Integer geometry, opaque interior colors, buffer guards,
and ownership remain strict. This is broad deterministic regression coverage,
not full SVG conformance or fuzzing. Gradient regressions cover repeat/reflect
pixels, negative positions, long scanlines, off-center and boundary focal points,
omitted focal coordinates, invalid caller edits, and C/native output agreement.
Explicit inheritance checks cover line caps, joins, fill rules, and paint order,
including child overrides followed by `inherit`. Full CSS/gradient-reference
inheritance remains outside the feature set.
