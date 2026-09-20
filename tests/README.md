From the repository root, run the assertion-based regression suite:

```sh
CC=clang sh tests/run.sh
```

The runner builds in a temporary directory and uses a ten-second timeout.
It enables address, undefined-behavior, and float-cast-overflow sanitizers.
Leak detection defaults to off for environments that execute under tracing;
set `ASAN_OPTIONS=detect_leaks=1` to enable it elsewhere.

The checks cover the nine parser/rasterizer fixes and render all three
bundled SVG examples. Override `CC` and `CFLAGS` for other configurations:

```sh
CC=clang++ CFLAGS='-x c++ -std=c++11 -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all' sh tests/run.sh
CC=clang CFLAGS='-std=c99 -O1 -g -fsanitize=memory -fsanitize-memory-track-origins=2 -fPIE -pie' sh tests/run.sh
```
