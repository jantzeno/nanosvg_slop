From the repository root, run the assertion-based regression suite:

```sh
CC=clang sh tests/run.sh
```

The runner builds in a temporary directory and uses a ten-second timeout.
It enables address, undefined-behavior, and float-cast-overflow sanitizers.
Leak detection defaults to off for environments that execute under tracing;
set `ASAN_OPTIONS=detect_leaks=1` to enable it elsewhere.

The checks cover parser/rasterizer fixes, including gradient fill/stroke
opacity, shared and missing gradient references, solid-color opacity,
and centered/off-center radial focus in both coordinate systems,
and render all three bundled SVG examples. Override `CC` and `CFLAGS` for
other configurations:

```sh
CC=clang++ CFLAGS='-x c++ -std=c++11 -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all' sh tests/run.sh
CC=clang CFLAGS='-std=c99 -O1 -g -fsanitize=memory -fsanitize-memory-track-origins=2 -fPIE -pie' sh tests/run.sh
```

Gradient fill/stroke opacity is baked into each paint's copied stops;
the rasterizer applies overall shape opacity separately. The parser temporarily
uses an `opacity` member of the existing `NSVGpaint` union, preserving the
paint and shape layouts on targets where `float` fits that union. Returned
paints continue to use only `color` or `gradient` according to their type.

Radial focus checks validate parsed coordinates relative to the center,
including finite fallback coordinates for nonpositive radii. The built-in
rasterizer still does not render off-center focal points.

Run the installation and external-consumer check with CMake 3.25 or newer and
CTest available:

```sh
CC=/usr/bin/clang sh tests/install.sh
```

It builds the default static and opt-in shared libraries with the default
library directory and explicit `CMAKE_INSTALL_LIBDIR=lib` and `lib64` overrides.
The two `lib64` cases also set `CMAKE_INSTALL_INCLUDEDIR=custom/include` and
verify the installed header locations. Generated `.c` files must be exact
copies of their source headers.

All installs are confined to temporary directories with `DESTDIR`; the check
verifies the install manifest stays under the chosen prefix. Each relocated
installation must provide its libraries and package files in the selected
directory. An external consumer must find the package, link both exported
targets, and render a gradient.

Two additional static/shared consumers use `add_subdirectory()` and link the
`nanosvg` and `nanosvgrast` targets without manually specifying include
directories. All eight consumers run the same rendering check. The minimum
CMake version is 3.25; the static-library default is unchanged.
