// Linux linker allocation injection exercises native and C-graph cleanup.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "nanosvgrast.hpp"
#include "nanosvgrast.h"
#include <cassert>
#include <cstdlib>
#include <new>
#include <cstdio>

static int fail_after = -1;
static bool failed = false;
static std::size_t live_allocations = 0;
static bool should_fail() {
    if (fail_after < 0) return false;
    if (fail_after-- == 0) { failed = true; return true; }
    return false;
}
void* operator new(std::size_t n) {
    if (should_fail()) throw std::bad_alloc();
    if (auto* p = std::malloc(n ? n : 1)) { ++live_allocations; return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
extern "C" void* __real_calloc(std::size_t, std::size_t);
extern "C" void __real_free(void*);
extern "C" void* __wrap_calloc(std::size_t n, std::size_t size) {
    if (should_fail()) return nullptr;
    auto* p = __real_calloc(n, size);
    if (p) ++live_allocations;
    return p;
}
extern "C" void __wrap_free(void* p) {
    if (p) { assert(live_allocations > 0); --live_allocations; }
    __real_free(p);
}

static constexpr auto svg = "<svg width='32' height='32'><style>.a{stroke:blue;stroke-width:2}</style>"
    "<defs><linearGradient id='g'><stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs>"
    "<rect class='a' x='2' y='2' width='28' height='28' fill='url(#g)'/></svg>";

int main() {
    int failures = 0;
    {
    for (int i = 0; ; ++i) {
        assert(i < 1000);
        fail_after = i; failed = false;
        auto result = nanosvg::parse(svg);
        fail_after = -1;
        if (!failed) { assert(result); break; }
        assert(!result && result.error() == nanosvg::Error::allocation_failure);
        ++failures;
    }
    for (int i = 0; ; ++i) {
        assert(i < 1000);
        std::string input(svg);
        fail_after = i; failed = false;
        auto* result = nsvgParse(input.data(), "px", 96);
        fail_after = -1;
        if (!failed) { assert(result); nsvgDelete(result); break; }
        assert(!result);
        ++failures;
    }
    auto image = nanosvg::parse(svg);
    assert(image);
    for (int i = 0; ; ++i) {
        assert(i < 1000);
        auto renderer = nanosvg::create_rasterizer();
        assert(renderer);
        unsigned char pixels[32*32*4];
        fail_after = i; failed = false;
        auto result = (*renderer)->rasterize(**image, pixels, 32, 32, 128);
        fail_after = -1;
        if (!failed) { assert(result); break; }
        assert(!result && result.error() == nanosvg::Error::allocation_failure);
        assert((*renderer)->rasterize(**image, pixels, 32, 32, 128));
        ++failures;
    }
    std::string input(svg);
    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> cImage(nsvgParse(input.data(), "px", 96), nsvgDelete);
    assert(cImage);
    for (int i = 0; ; ++i) {
        assert(i < 1000);
        std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> renderer(nsvgCreateRasterizer(), nsvgDeleteRasterizer);
        assert(renderer);
        unsigned char pixels[32*32*4]{};
        fail_after = i; failed = false;
        nsvgRasterize(renderer.get(), cImage.get(), 0, 0, 1, pixels, 32, 32, 128);
        fail_after = -1;
        if (!failed) break;
        nsvgRasterize(renderer.get(), cImage.get(), 0, 0, 1, pixels, 32, 32, 128);
        assert(pixels[(16*32+16)*4+3] == 255);
        ++failures;
    }
    }
    assert(live_allocations == 0);
    assert(failures > 20);
    std::printf("NanoSVG allocation cleanup passed (%d injected failures)\n", failures);
}
