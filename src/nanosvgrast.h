/*
 * Copyright (c) 2013-14 Mikko Mononen memon@inside.org
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software
 * in a product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * The polygon rasterization is heavily based on stb_truetype rasterizer
 * by Sean Barrett - http://nothings.org/
 *
 */

// Altered for NanoSVG 2: C API over the native C++23 implementation.
// Define NANOSVGRAST_IMPLEMENTATION in one C++23 translation unit
// before the first NanoSVG include to implement both APIs.
#ifndef NANOSVGRAST_H
#define NANOSVGRAST_H

#include "nanosvg.h"

#if defined(NANOSVGRAST_CPLUSPLUS)
#error "Remove NANOSVGRAST_CPLUSPLUS; the C API always uses C linkage."
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NSVGrasterizer NSVGrasterizer;

/* Example Usage:
	// Load SVG
	NSVGimage* image;
	image = nsvgParseFromFile("test.svg", "px", 96);

	// Create rasterizer (can be used to render multiple images).
	struct NSVGrasterizer* rast = nsvgCreateRasterizer();
	// Allocate memory for image
	unsigned char* img = malloc(w*h*4);
	// Rasterize
	nsvgRasterize(rast, image, 0,0,1, img, w, h, w*4);
*/

// Allocated compatibility handle; renders use independent working state.
NSVGrasterizer* nsvgCreateRasterizer(void);

// Rasterizes SVG image, returns RGBA image (non-premultiplied alpha)
//   r - pointer to rasterizer context
//   image - pointer to image to rasterize
//   tx,ty - image offset (applied after scaling)
//   scale - image scale
//   dst - pointer to destination image data, 4 bytes per pixel (RGBA)
//   w - width of the image to render
//   h - height of the image to render
//   stride - number of bytes per scaleline in the destination buffer
// Errors leave dst unchanged. Successful renders preserve row padding.
void nsvgRasterize(NSVGrasterizer* r,
				   NSVGimage* image, double tx, double ty, double scale,
				   unsigned char* dst, int w, int h, int stride);

// Deletes rasterizer context.
void nsvgDeleteRasterizer(NSVGrasterizer*);


#ifdef __cplusplus
}
#endif


#ifdef NANOSVGRAST_IMPLEMENTATION

#ifndef __cplusplus
#error "NANOSVGRAST_IMPLEMENTATION requires C++23; compile the implementation as C++."
#else

#include "nanosvgrast.hpp"
#include <cstring>

// Compatibility handle only; every render owns its own working state.
struct NSVGrasterizer {};

extern "C" {
NSVGrasterizer* nsvgCreateRasterizer(void) {
    try {
        return std::make_unique<NSVGrasterizer>().release();
    } catch (...) { return nullptr; }
}
void nsvgDeleteRasterizer(NSVGrasterizer* rasterizer) { delete rasterizer; }

void nsvgRasterize(NSVGrasterizer* rasterizer, NSVGimage* image, double tx, double ty, double scale,
                   unsigned char* dst, int width, int height, int stride) {
    if (!rasterizer || !image) return;
    const auto size = nanosvg::detail::raster_buffer_size(width, height, stride);
    if (!size || *size == 0 || !dst) return;
    try {
        // ponytail: copy per C call so caller edits are visible; a versioned C
        // image API would be needed before caching conversions safely.
        auto native = nanosvg::c_api::import_image(*image);
        auto rendered = nanosvg::rasterize(native, {width, height, {tx, ty}, scale});
        if (!rendered) return;
        // Commit only after all allocations and rendering succeed; preserve row padding.
        const auto rowPixels = static_cast<std::size_t>(width);
        for (int row = 0; row < height; ++row)
            std::memcpy(dst + static_cast<std::size_t>(row)*stride,
                        (*rendered)->pixels.data() + static_cast<std::size_t>(row)*rowPixels,
                        rowPixels*sizeof(nanosvg::Rgba8));
    } catch (...) { }
}
}

#endif // __cplusplus
#endif // NANOSVGRAST_IMPLEMENTATION
#endif // NANOSVGRAST_H
