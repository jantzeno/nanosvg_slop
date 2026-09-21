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
 * The SVG parser is based on Anti-Grain Geometry 2.4 SVG example
 * Copyright (C) 2002-2004 Maxim Shemanarev (McSeem) (http://www.antigrain.com/)
 *
 * Arc calculation code based on canvg (https://code.google.com/p/canvg/)
 *
 * Bounding box calculation based on http://blog.hackers-cafe.net/2009/06/how-to-calculate-bezier-curves-bounding.html
 *
 */

// Altered for NanoSVG 2: C API over the native C++23 implementation.
// Define NANOSVG_IMPLEMENTATION in one C++23 translation unit
// before the first NanoSVG include to implement both APIs.
#ifndef NANOSVG_H
#define NANOSVG_H

#if defined(NANOSVG_CPLUSPLUS)
#error "Remove NANOSVG_CPLUSPLUS; the C API always uses C linkage."
#endif

#ifdef __cplusplus
extern "C" {
#endif

// C compatibility API for the NanoSVG C++23 library.
// The parser returns double-precision cubic Bezier shapes.
//
// The library suits well for anything from rendering scalable icons in your editor application to prototyping a game.
//
// NanoSVG supports a wide range of SVG features, but something may be missing, feel free to create a pull request!
//
// The shapes in the SVG images are transformed by the viewBox and converted to specified units.
// That is, you should get the same looking data as your designed in your favorite app.
//
// NanoSVG can return the paths in few different units. For example if you want to render an image, you may choose
// to get the paths in pixels, or if you are feeding the data into a CNC-cutter, you may want to use millimeters.
//
// The units passed to NanoSVG should be one of: 'px', 'pt', 'pc' 'mm', 'cm', or 'in'.
// DPI (dots-per-inch) controls how the unit conversion is done.
//
// If you don't know or care about the units stuff, "px" and 96 should get you going.


/* Example Usage:
	// Load SVG
	NSVGimage* image;
	image = nsvgParseFromFile("test.svg", "px", 96);
	printf("size: %f x %f\n", image->width, image->height);
	// Use...
	for (NSVGshape *shape = image->shapes; shape != NULL; shape = shape->next) {
		for (NSVGpath *path = shape->paths; path != NULL; path = path->next) {
			for (int i = 0; i < path->npts-1; i += 3) {
				double* p = &path->pts[i*2];
				drawCubicBez(p[0],p[1], p[2],p[3], p[4],p[5], p[6],p[7]);
			}
		}
	}
	// Delete
	nsvgDelete(image);
*/

enum NSVGpaintType {
	NSVG_PAINT_UNDEF = -1,
	NSVG_PAINT_NONE = 0,
	NSVG_PAINT_COLOR = 1,
	NSVG_PAINT_LINEAR_GRADIENT = 2,
	NSVG_PAINT_RADIAL_GRADIENT = 3
};

enum NSVGspreadType {
	NSVG_SPREAD_PAD = 0,
	NSVG_SPREAD_REFLECT = 1,
	NSVG_SPREAD_REPEAT = 2
};

enum NSVGlineJoin {
	NSVG_JOIN_MITER = 0,
	NSVG_JOIN_ROUND = 1,
	NSVG_JOIN_BEVEL = 2
};

enum NSVGlineCap {
	NSVG_CAP_BUTT = 0,
	NSVG_CAP_ROUND = 1,
	NSVG_CAP_SQUARE = 2
};

enum NSVGfillRule {
	NSVG_FILLRULE_NONZERO = 0,
	NSVG_FILLRULE_EVENODD = 1
};

enum NSVGflags {
	NSVG_FLAGS_VISIBLE = 0x01
};

enum NSVGpaintOrder {
	NSVG_PAINT_FILL = 0x00,
	NSVG_PAINT_MARKERS = 0x01,
	NSVG_PAINT_STROKE = 0x02,
};

typedef struct NSVGgradientStop {
	unsigned int color;
	double offset;
} NSVGgradientStop;

typedef struct NSVGgradient {
	double xform[6];
	char spread;
	double fx, fy;
	int nstops;
	NSVGgradientStop* stops;
} NSVGgradient;

typedef struct NSVGpaint {
	signed char type;
	union {
		unsigned int color;
		NSVGgradient* gradient;
		double opacity;		// Legacy field; native parsing resolves paints before export.
	};
} NSVGpaint;

typedef struct NSVGpath
{
	double* pts;					// Cubic bezier points: x0,y0, [cpx1,cpx1,cpx2,cpy2,x1,y1], ...
	int npts;					// Total number of bezier points.
	char closed;				// Flag indicating if shapes should be treated as closed.
	double bounds[4];			// Tight bounding box of the shape [minx,miny,maxx,maxy].
	struct NSVGpath* next;		// Pointer to next path, or NULL if last element.
} NSVGpath;

typedef struct NSVGshape
{
	char id[64];				// Optional 'id' attr of the shape or its group
	NSVGpaint fill;				// Fill paint
	NSVGpaint stroke;			// Stroke paint
	double opacity;				// Opacity of the shape.
	double strokeWidth;			// Stroke width (scaled).
	double strokeDashOffset;		// Stroke dash offset (scaled).
	double strokeDashArray[8];	// Stroke dash array (scaled).
	char strokeDashCount;		// Number of dash values in dash array.
	char strokeLineJoin;		// Stroke join type.
	char strokeLineCap;			// Stroke cap type.
	double miterLimit;			// Miter limit
	char fillRule;				// Fill rule, see NSVGfillRule.
    unsigned char paintOrder;	// Encoded paint order (3×2-bit fields) see NSVGpaintOrder
	unsigned char flags;		// Logical or of NSVG_FLAGS_* flags
	double bounds[4];			// Tight bounding box of the shape [minx,miny,maxx,maxy].
	char fillGradient[64];		// Optional 'id' of fill gradient
	char strokeGradient[64];	// Optional 'id' of stroke gradient
	double xform[6];				// Root transformation for fill/stroke gradient
	NSVGpath* paths;			// Linked list of paths in the image.
	struct NSVGshape* next;		// Pointer to next shape, or NULL if last element.
} NSVGshape;

typedef struct NSVGimage
{
	double width;				// Width of the image.
	double height;				// Height of the image.
	NSVGshape* shapes;			// Linked list of shapes in the image.
} NSVGimage;

// Parses SVG file from a file, returns SVG image as paths.
NSVGimage* nsvgParseFromFile(const char* filename, const char* units, double dpi);

// Parses SVG file from a null terminated string, returns SVG image as paths.
// Mutable signature retained for C compatibility; the implementation copies input.
NSVGimage* nsvgParse(char* input, const char* units, double dpi);

// Duplicates a path.
NSVGpath* nsvgDuplicatePath(NSVGpath* p);

// Deletes an image.
void nsvgDelete(NSVGimage* image);

#ifdef __cplusplus
}

namespace nanosvg {
struct Image;
namespace c_api {
// Implementation detail: copy the current readable, acyclic C graph.
Image import_image(const NSVGimage& image);
}
}
#endif


#ifdef NANOSVG_IMPLEMENTATION

#ifndef __cplusplus
#error "NANOSVG_IMPLEMENTATION requires C++23; compile the implementation as C++."
#else

#include "nanosvg.hpp"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nanosvg::c_api {
namespace {
struct Free { void operator()(void* p) const noexcept { std::free(p); } };

template<class T> auto allocate(std::size_t count = 1) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
        throw std::length_error("C allocation too large");
    std::unique_ptr<T, Free> result;
    if (count) {
        result.reset(static_cast<T*>(std::calloc(count, sizeof(T))));
        if (!result) throw std::bad_alloc();
    }
    return result;
}

int checked_count(std::size_t count) {
    if (count > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("C count too large");
    return static_cast<int>(count);
}

void delete_paint(NSVGpaint& paint) noexcept {
    if ((paint.type == NSVG_PAINT_LINEAR_GRADIENT || paint.type == NSVG_PAINT_RADIAL_GRADIENT) && paint.gradient) {
        std::free(paint.gradient->stops);
        std::free(paint.gradient);
    }
}

struct PathDeleter {
    void operator()(NSVGpath* p) const noexcept {
        while (p) {
            auto* next = p->next;
            std::free(p->pts);
            std::free(p);
            p = next;
        }
    }
};
struct ShapeDeleter {
    void operator()(NSVGshape* p) const noexcept {
        while (p) {
            auto* next = p->next;
            delete_paint(p->fill);
            delete_paint(p->stroke);
            PathDeleter{}(p->paths);
            std::free(p);
            p = next;
        }
    }
};

template<std::size_t N> void export_string(char (&dst)[N], const std::string& src) {
    const auto count = std::min(src.size(), N - 1);
    std::copy_n(src.data(), count, dst);
    dst[count] = 0;
}
template<std::size_t N> std::string import_string(const char (&src)[N]) {
    return {src, std::find(src, src+N, '\0')};
}

void export_paint(const Paint& source, NSVGpaint& dest) {
    if (const auto* color = std::get_if<Color>(&source)) {
        dest.type = NSVG_PAINT_COLOR;
        dest.color = *color;
    } else if (const auto* gradient = std::get_if<Gradient>(&source)) {
        auto copy = allocate<NSVGgradient>();
        const auto count = checked_count(gradient->stops.size());
        auto stops = allocate<NSVGgradientStop>(gradient->stops.size());
        std::copy(gradient->xform.begin(), gradient->xform.end(), copy->xform);
        copy->spread = static_cast<char>(gradient->spread);
        copy->fx = gradient->fx;
        copy->fy = gradient->fy;
        copy->nstops = count;
        for (int i = 0; i < count; ++i) stops.get()[i] = {gradient->stops[i].color, gradient->stops[i].offset};
        copy->stops = stops.release();
        dest.type = static_cast<signed char>(gradient->kind);
        dest.gradient = copy.release();
    } else dest.type = NSVG_PAINT_NONE;
}

Paint import_paint(const NSVGpaint& source) {
    if (source.type == NSVG_PAINT_COLOR) return Color(source.color);
    if (source.type != NSVG_PAINT_LINEAR_GRADIENT && source.type != NSVG_PAINT_RADIAL_GRADIENT) return {};
    if (!source.gradient || source.gradient->nstops < 0 || (source.gradient->nstops && !source.gradient->stops))
        throw std::invalid_argument("invalid C gradient");
    const auto& src = *source.gradient;
    if (src.spread < NSVG_SPREAD_PAD || src.spread > NSVG_SPREAD_REPEAT)
        throw std::invalid_argument("invalid C gradient spread");
    Gradient gradient;
    gradient.kind = static_cast<GradientKind>(source.type);
    gradient.spread = static_cast<Spread>(src.spread);
    gradient.fx = src.fx;
    gradient.fy = src.fy;
    std::copy_n(src.xform, 6, gradient.xform.begin());
    gradient.stops.reserve(src.nstops);
    for (int i = 0; i < src.nstops; ++i) gradient.stops.push_back({src.stops[i].color, src.stops[i].offset});
    return gradient;
}

Path import_path(const NSVGpath& source) {
    if (source.npts < 0 || (source.npts && !source.pts)) throw std::invalid_argument("invalid C path");
    Path path;
    path.closed = source.closed != 0;
    std::copy_n(source.bounds, 4, path.bounds.begin());
    path.points.resize(source.npts);
    for (std::size_t i = 0; i < path.points.size(); ++i) path.points[i] = {source.pts[i*2], source.pts[i*2+1]};
    return path;
}

std::unique_ptr<NSVGpath, PathDeleter> export_path(const Path& source) {
    std::unique_ptr<NSVGpath, PathDeleter> path(allocate<NSVGpath>().release());
    path->npts = checked_count(source.points.size());
    path->closed = source.closed;
    std::copy(source.bounds.begin(), source.bounds.end(), path->bounds);
    if (source.points.size() > std::numeric_limits<std::size_t>::max() / 2)
        throw std::length_error("C path too large");
    auto points = allocate<double>(source.points.size()*2);
    for (std::size_t i = 0; i < source.points.size(); ++i) {
        points.get()[i*2] = source.points[i].x;
        points.get()[i*2+1] = source.points[i].y;
    }
    path->pts = points.release();
    return path;
}

NSVGimage* export_image(const Image& source) {
    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(allocate<NSVGimage>().release(), nsvgDelete);
    image->width = source.width;
    image->height = source.height;
    auto** tail = &image->shapes;
    for (const auto& src : source.shapes) {
        std::unique_ptr<NSVGshape, ShapeDeleter> shape(allocate<NSVGshape>().release());
        export_string(shape->id, src.id);
        export_string(shape->fillGradient, src.fillGradient);
        export_string(shape->strokeGradient, src.strokeGradient);
        export_paint(src.fill, shape->fill);
        export_paint(src.stroke, shape->stroke);
        shape->opacity = src.opacity;
        shape->strokeWidth = src.strokeWidth;
        shape->strokeDashOffset = src.strokeDashOffset;
        shape->strokeDashCount = static_cast<char>(std::min<std::size_t>(src.strokeDashArray.size(), 8));
        std::copy_n(src.strokeDashArray.begin(), shape->strokeDashCount, shape->strokeDashArray);
        shape->strokeLineJoin = static_cast<char>(src.strokeLineJoin);
        shape->strokeLineCap = static_cast<char>(src.strokeLineCap);
        shape->miterLimit = src.miterLimit;
        shape->fillRule = static_cast<char>(src.fillRule);
        for (int i = 0; i < 3; ++i) shape->paintOrder |= static_cast<unsigned char>(src.paintOrder[i]) << (2*i);
        shape->flags = src.visible ? NSVG_FLAGS_VISIBLE : 0;
        std::copy(src.bounds.begin(), src.bounds.end(), shape->bounds);
        std::copy(src.xform.begin(), src.xform.end(), shape->xform);
        auto** pathTail = &shape->paths;
        for (const auto& srcPath : src.paths) {
            *pathTail = export_path(srcPath).release();
            pathTail = &(*pathTail)->next;
        }
        *tail = shape.release();
        tail = &(*tail)->next;
    }
    return image.release();
}
} // namespace

Image import_image(const NSVGimage& source) {
    Image image;
    image.width = source.width;
    image.height = source.height;
    for (const auto* src = source.shapes; src; src = src->next) {
        if (src->strokeDashCount < 0 || src->strokeDashCount > 8) throw std::invalid_argument("invalid C dashes");
        if (src->strokeLineCap < NSVG_CAP_BUTT || src->strokeLineCap > NSVG_CAP_SQUARE ||
            src->strokeLineJoin < NSVG_JOIN_MITER || src->strokeLineJoin > NSVG_JOIN_BEVEL ||
            src->fillRule < NSVG_FILLRULE_NONZERO || src->fillRule > NSVG_FILLRULE_EVENODD)
            throw std::invalid_argument("invalid C stroke or fill rule");
        for (int i = 0; i < 3; ++i)
            if (((src->paintOrder >> (2*i)) & 3) > NSVG_PAINT_STROKE)
                throw std::invalid_argument("invalid C paint order");
        Shape shape;
        shape.id = import_string(src->id);
        shape.fillGradient = import_string(src->fillGradient);
        shape.strokeGradient = import_string(src->strokeGradient);
        shape.fill = import_paint(src->fill);
        shape.stroke = import_paint(src->stroke);
        shape.opacity = src->opacity;
        shape.strokeWidth = src->strokeWidth;
        shape.strokeDashOffset = src->strokeDashOffset;
        shape.strokeDashArray.assign(src->strokeDashArray, src->strokeDashArray + src->strokeDashCount);
        shape.strokeLineJoin = static_cast<LineJoin>(src->strokeLineJoin);
        shape.strokeLineCap = static_cast<LineCap>(src->strokeLineCap);
        shape.miterLimit = src->miterLimit;
        shape.fillRule = static_cast<FillRule>(src->fillRule);
        for (int i = 0; i < 3; ++i) shape.paintOrder[i] = static_cast<PaintOrder>((src->paintOrder >> (2*i)) & 3);
        shape.visible = (src->flags & NSVG_FLAGS_VISIBLE) != 0;
        std::copy_n(src->bounds, 4, shape.bounds.begin());
        std::copy_n(src->xform, 6, shape.xform.begin());
        for (auto* path = src->paths; path; path = path->next) shape.paths.push_back(import_path(*path));
        image.shapes.push_back(std::move(shape));
    }
    return image;
}
} // namespace nanosvg::c_api

extern "C" {
NSVGimage* nsvgParse(char* input, const char* units, double dpi) {
    if (!input || !units) return nullptr;
    try {
        auto result = nanosvg::parse(input, units, dpi);
        return result ? nanosvg::c_api::export_image(**result) : nullptr;
    } catch (...) { return nullptr; }
}
NSVGimage* nsvgParseFromFile(const char* filename, const char* units, double dpi) {
    if (!filename || !units) return nullptr;
    try {
        auto result = nanosvg::parse_file(filename, units, dpi);
        return result ? nanosvg::c_api::export_image(**result) : nullptr;
    } catch (...) { return nullptr; }
}
NSVGpath* nsvgDuplicatePath(NSVGpath* path) {
    if (!path) return nullptr;
    try { return nanosvg::c_api::export_path(nanosvg::c_api::import_path(*path)).release(); }
    catch (...) { return nullptr; }
}
void nsvgDelete(NSVGimage* image) {
    if (!image) return;
    nanosvg::c_api::ShapeDeleter{}(image->shapes);
    std::free(image);
}
}

#endif // __cplusplus
#endif // NANOSVG_IMPLEMENTATION
#endif // NANOSVG_H
