#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

// Compile the single-header implementations to exercise private helpers.
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "nanosvgrast.hpp"

static NSVGimage* parse(const char* svg)
{
	char* input = (char*)malloc(strlen(svg) + 1);
	NSVGimage* image;
	assert(input != NULL);
	strcpy(input, svg);
	image = nsvgParse(input, "px", 96);
	free(input);
	assert(image != NULL);
	return image;
}

static void test_css_recursion(void)
{
	const char* cases[] = {
		"<svg><style>.a{class:a;}</style><rect class=\"a\" width=\"1\" height=\"1\"/></svg>",
		"<svg><style>.a{class:b;fill:#ff0000;}.b{class:a;}</style><rect class=\"a\" width=\"1\" height=\"1\"/></svg>",
		"<svg><style>.a{fill:#ff0000;}.b{stroke:#0000ff;}</style><rect class=\"a b\" width=\"1\" height=\"1\"/></svg>",
		"<svg><style>.a{fill:#ff0000;}</style><rect style=\"class:a;\" width=\"1\" height=\"1\"/></svg>"
	};
	size_t i;
	for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		NSVGimage* image = parse(cases[i]);
		assert(image->shapes != NULL);
		assert(image->shapes->fill.type == NSVG_PAINT_COLOR);
		assert((image->shapes->fill.color & 0xffffff) ==
			(i == 1 || i == 2 ? nanosvg::detail::rgb(255, 0, 0) : nanosvg::detail::rgb(0, 0, 0)));
		if (i == 2)
			assert((image->shapes->stroke.color & 0xffffff) == nanosvg::detail::rgb(0, 0, 255));
		nsvgDelete(image);
	}
}

static void test_css_bounds(void)
{
	const char* declarations[] = {
		"", " ", "; ; ", "fill", "fill red", ":red", "fill: #ff0000",
		"fill: #ff0000;", "  fill  :  #ff0000  ;  ", ";bad;fill:#ff0000;bad"
	};
	size_t i;
	int inlineStyle;
	NSVGimage* image = parse("<style>.285713{ <path class=\"285713>");
	nsvgDelete(image);
	for (inlineStyle = 0; inlineStyle < 2; inlineStyle++) {
		for (i = 0; i < sizeof(declarations)/sizeof(declarations[0]); i++) {
			char svg[512];
			if (inlineStyle)
				snprintf(svg, sizeof(svg), "<svg><rect style=\"%s\" width=\"1\" height=\"1\"/></svg>", declarations[i]);
			else
				snprintf(svg, sizeof(svg), "<svg><style>.a{%s}</style><rect class=\"a\" width=\"1\" height=\"1\"/></svg>", declarations[i]);
			image = parse(svg);
			assert(image->shapes != NULL);
			assert((image->shapes->fill.color & 0xffffff) ==
				(i >= 6 ? nanosvg::detail::rgb(255, 0, 0) : nanosvg::detail::rgb(0, 0, 0)));
			nsvgDelete(image);
		}
	}
}

static void render(NSVGimage* image, unsigned char* pixels)
{
	NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
	assert(rasterizer != NULL);
	nsvgRasterize(rasterizer, image, 0, 0, 1, pixels, 64, 64, 64*4);
	nsvgDeleteRasterizer(rasterizer);
}

static void test_dashes(void)
{
	unsigned char pixels[64*64*4];
	const char* patterns[] = {"4 4", "0 4 4 0", "4"};
	int offsets[] = {0, 2, 4, -2, 10};
	size_t i, j;
	NSVGimage* image = parse("<svg width=\"10\" height=\"10\"><polyline points=\"0,0 99999999,0\" fill=\"none\" stroke=\"#000\" stroke-width=\"1\" stroke-dasharray=\"2 1\"/></svg>");
	render(image, pixels);
	assert(pixels[3] > 0);
	nsvgDelete(image);
	// At this origin, a positive dash length can round back to the same point.
	image = parse("<svg width=\"64\" height=\"64\"><path d=\"M100000000 1h100000000\" fill=\"none\" stroke=\"black\" stroke-dasharray=\"1 1\"/></svg>");
	render(image, pixels);
	nsvgDelete(image);
	for (i = 0; i < sizeof(patterns)/sizeof(patterns[0]); i++) {
		for (j = 0; j < sizeof(offsets)/sizeof(offsets[0]); j++) {
			char svg[512];
			int x;
			snprintf(svg, sizeof(svg), "<svg width=\"64\" height=\"64\"><path d=\"M0 4H32\" fill=\"none\" stroke=\"black\" stroke-width=\"2\" stroke-dasharray=\"%s\" stroke-dashoffset=\"%d\"/></svg>", patterns[i], offsets[j]);
			image = parse(svg);
			render(image, pixels);
			for (x = 0; x < 32; x++) {
				int phase = (x + offsets[j] + 16) % 8;
				int on = i == 1 ? phase >= 4 : phase < 4;
				assert(pixels[(4*64+x)*4+3] == (on ? 255 : 0));
			}
			nsvgDelete(image);
		}
	}
}

static void test_numeric(void)
{
	const char* cases[] = {
		"<svg width=\"64\" height=\"64\"><path d=\"M-30000000,0 L10,64 L20,0 Z\"/></svg>",
		"<svg width=\"64\" height=\"64\"><path d=\"M30000000,0 L-30000000,64 L20,0 Z\"/></svg>",
		"<svg width=\"64\" height=\"64\"><path d=\"M0,0 L1e999,20 L20,40 Z\"/></svg>",
		"<svg width=\"64\" height=\"64\"><path d=\"M0,0 L1e999,20 L20,40\" fill=\"none\" stroke=\"black\" stroke-linejoin=\"round\"/></svg>",
		"<svg width=\"64\" height=\"64\"><path d=\"M10,10 L50,50\" stroke=\"red\" stroke-width=\"10662107277\"/></svg>",
		"<svg width=\"64\" height=\"64\"><path d=\"M10,10 L50,50\" stroke=\"red\" stroke-width=\"1e999\" stroke-linecap=\"round\"/></svg>",
		"<svg width=\"64\" height=\"64\"><path d=\"M0,0 A8 57.1E2857 0 1 1 0 0.2\"/></svg>",
		"<svg width=\"64\" height=\"64\"><path d=\"M0,0 A1e30 1e30 0 0 1 20 20\"/></svg>",
		"<svg width=\"10\" height=\"10\"><polyline points=\"0,0 99999999,0\" fill=\"none\" stroke=\"#000\" stroke-width=\"1\"/></svg>"
	};
	unsigned char pixels[64*64*4];
	double special[] = {NAN, INFINITY, -INFINITY, DBL_MAX, -DBL_MAX};
	double belowMax = nextafter((double)INT_MAX, 0);
	double aboveMin = nextafter((double)INT_MIN, 0);
	size_t i;
	NSVGimage* image;
	assert(nanosvg::detail::nsvg__roundf_clamp(NAN) == 0);
	assert(nanosvg::detail::nsvg__roundf_clamp(INFINITY) == INT_MAX);
	assert(nanosvg::detail::nsvg__roundf_clamp(-INFINITY) == INT_MIN);
	assert(nanosvg::detail::nsvg__roundf_clamp(DBL_MAX) == INT_MAX);
	assert(nanosvg::detail::nsvg__roundf_clamp(-DBL_MAX) == INT_MIN);
	assert(nanosvg::detail::nsvg__roundf_clamp((double)INT_MAX) == INT_MAX);
	assert(nanosvg::detail::nsvg__roundf_clamp((double)INT_MIN) == INT_MIN);
	assert(nanosvg::detail::nsvg__roundf_clamp(belowMax) == INT_MAX);
	assert(nanosvg::detail::nsvg__roundf_clamp(aboveMin) == INT_MIN);
	assert(nanosvg::detail::nsvg__roundf_clamp(1.5) == 2 && nanosvg::detail::nsvg__roundf_clamp(-1.5) == -2);
	assert(nanosvg::detail::nsvg__iadd_sat(INT_MAX, 1) == INT_MAX);
	assert(nanosvg::detail::nsvg__iadd_sat(INT_MIN, -1) == INT_MIN);
	assert(nanosvg::detail::nsvg__iadd_sat(INT_MAX, INT_MAX) == INT_MAX);
	assert(nanosvg::detail::nsvg__iadd_sat(INT_MIN, INT_MIN) == INT_MIN);
	assert(nanosvg::detail::nsvg__iadd_sat(INT_MAX, INT_MIN) == -1);
	assert(nanosvg::detail::nsvg__iadd_sat(10, -20) == -10);
	assert(nanosvg::detail::nsvg__curveDivs(1, std::numbers::pi, 0.25) == 3);
	assert(nanosvg::detail::nsvg__curveDivs(DBL_MAX, std::numbers::pi, 0.25) == 2);
	for (i = 0; i < sizeof(special)/sizeof(special[0]); i++) {
		assert(nanosvg::detail::nsvg__curveDivs(special[i], std::numbers::pi, 0.25) == 2);
		assert(nanosvg::detail::nsvg__curveDivs(1, special[i], 0.25) == 2);
	}
	for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		image = parse(cases[i]);
		render(image, pixels);
		nsvgDelete(image);
	}
	image = parse("<svg width=\"64\" height=\"64\"><rect x=\"8\" y=\"8\" width=\"32\" height=\"32\" fill=\"#ff0000\"/></svg>");
	render(image, pixels);
	for (i = 0; i < 64*64; i++) {
		int inside = i%64 >= 8 && i%64 < 40 && i/64 >= 8 && i/64 < 40;
		assert(pixels[i*4+3] == (inside ? 255 : 0));
		if (inside) assert(pixels[i*4] == 255 && pixels[i*4+1] == 0 && pixels[i*4+2] == 0);
	}
	nsvgDelete(image);
}

static void test_inverse(void)
{
    const nanosvg::Transform matrices[] = {{0,0,0,0,0,0}, {1,2,2,4,3,4}, {2,1,1,3,4,5}};
    for (std::size_t i = 0; i < std::size(matrices); ++i) {
        const auto before = matrices[i];
        auto output = nanosvg::detail::inverse(matrices[i]);
        if (i == 2) output = nanosvg::detail::multiply(output, matrices[i]);
        assert(matrices[i] == before);
        for (int j = 0; j < 6; ++j) assert(std::abs(output[j]-nanosvg::detail::identity[j]) < 1e-6);
    }
}

static void test_transforms(void)
{
	const char* invalid[] = {
		"matrix()", "matrix(1 2 3 4 5)", "matrix(1 2 3 4 5 6 7)",
		"translate()", "translate(1 2 3)", "scale()", "scale(1 2 3)",
		"rotate()", "rotate(1 2)", "rotate(1 2 3 4)",
		"skewX()", "skewX(1 2)", "skewY()", "skewY(1 2)",
		"matrix", "translate", "scale", "rotate", "skewX", "skewY",
		"matrix(1 2 3 4 5 6", "translate(2", "scale(2", "rotate(2",
		"skewX(2", "skewY(2", "scale 2)", "scale(1e999)", "scale(.)"
	};
	const struct { const char* text; double matrix[6]; } valid[] = {
		{"matrix(2 1 3 4 5 6)", {2,1,3,4,5,6}},
		{"translate(2)", {1,0,0,1,2,0}}, {"translate(2,3)", {1,0,0,1,2,3}},
		{"scale(2)", {2,0,0,2,0,0}}, {"scale(2,3)", {2,0,0,3,0,0}},
		{"rotate(90)", {0,1,-1,0,0,0}}, {"rotate(90,2,3)", {0,1,-1,0,5,1}},
		{"skewX(45)", {1,0,1,1,0,0}}, {"skewY(45)", {1,1,0,1,0,0}},
		{"translate (7,9) scale(2 3)", {2,0,0,3,7,9}},
		{"scale(.5,-2e-1)", {.5,0,0,-.2,0,0}}
	};
	size_t i;
	int j, mixed;
	for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
		for (mixed = 0; mixed < 2; mixed++) {
			char text[256];
			const double expected[][6] = {{1,0,0,1,0,0}, {2,0,0,3,7,9}};
			snprintf(text, sizeof(text), mixed ? "translate(7 9) %s scale(2 3)" : "%s", invalid[i]);
			const auto actual = nanosvg::detail::parse_transform(text);
			for (j = 0; j < 6; j++) assert(fabs(actual[j] - expected[mixed][j]) < 1e-6);
		}
	}
	for (i = 0; i < sizeof(valid)/sizeof(valid[0]); i++) {
		const auto actual = nanosvg::detail::parse_transform(valid[i].text);
		for (j = 0; j < 6; j++) assert(fabs(actual[j] - valid[i].matrix[j]) < 1e-6);
	}
	{
		NSVGimage* image = parse("<svg width=\"64\" height=\"64\"><defs><linearGradient id=\"g\" gradientTransform=\"translate(2) scale()\"><stop stop-color=\"red\"/></linearGradient></defs><rect width=\"10\" height=\"10\" transform=\"translate(7 9) rotate(1 2) scale(2 3)\" fill=\"url(#g)\"/></svg>");
		NSVGshape* shape = image->shapes;
		assert(shape != NULL && shape->fill.type == NSVG_PAINT_LINEAR_GRADIENT);
		assert(shape->bounds[0] == 7 && shape->bounds[1] == 9);
		assert(shape->bounds[2] == 27 && shape->bounds[3] == 39);
		for (j = 0; j < 6; j++) assert(isfinite(shape->fill.gradient->xform[j]));
		nsvgDelete(image);
	}
}

static void test_arcs(void)
{
    const double radii[] = {195631, 1000, 1000, 10};
    const double ends[][2] = {{39.013, 7.101}, {1.8, 60}, {2.2, 60}, {10, 70}};
    for (int sweep = 0; sweep < 2; ++sweep) {
        for (std::size_t i = 0; i < std::size(radii); ++i) {
            for (bool line : {false, true}) {
                char svg[512];
                std::snprintf(svg, sizeof(svg), "<svg width='64' height='64'><path d='M0 60 a%.17g %.17g 0 0 %d %.17g %.17g%s'/></svg>",
                    radii[i], radii[i], sweep, ends[i][0], ends[i][1]-60, line ? " l3 4" : "");
                auto image = nanosvg::parse(svg);
                assert(image && (*image)->shapes.size() == 1);
                const auto& points = (*image)->shapes.front().paths.front().points;
                assert(std::abs(points.back().x-(ends[i][0]+(line ? 3 : 0))) < 1e-5);
                assert(std::abs(points.back().y-(ends[i][1]+(line ? 4 : 0))) < 1e-5);
                for (const auto& point : points) assert(std::isfinite(point.x) && std::isfinite(point.y));
                if (!line) {
                    if (i < 2) assert(points.size() == 4);
                    if (i == 1) assert(points[1].y == 60 && points[2].y == 60);
                    if (i >= 2) assert(points[1].y != 60 || points[2].y != 60);
                }
            }
        }
    }
    for (const char* value : {"1e999", "-1e999", "1.7976931348623157e308", "-1.7976931348623157e308"}) {
        char svg[512];
        std::snprintf(svg, sizeof(svg), "<svg width='64' height='64'><path d='M0 0 A8 %s 0 0 1 20 20 A8 8 %s 0 1 30 20 A8 8 0 0 1 1e999 20'/></svg>", value, value);
        auto image = nanosvg::parse(svg);
        assert(image && (*image)->shapes.size() == 1);
        const auto& points = (*image)->shapes.front().paths.front().points;
        assert(points.back().x == 30 && points.back().y == 20);
        for (const auto& point : points) assert(std::isfinite(point.x) && std::isfinite(point.y));
    }
}

static void test_gradients(void)
{
	int radial, transformed, userSpace, variant, j;
	for (radial = 0; radial < 2; radial++) {
		for (transformed = 0; transformed < 2; transformed++) {
			for (userSpace = 0; userSpace < 2; userSpace++) {
				NSVGimage* images[2];
				for (variant = 0; variant < 2; variant++) {
					char svg[1024];
					const char* coords;
					const char* tag = radial ? "radialGradient" : "linearGradient";
					if (radial)
						coords = !variant ? "cx='.5' cy='.5' r='.5' fx='.25' fy='.75'" :
							(userSpace ? "cx='.5px' cy='.5px' r='.5px' fx='.25px' fy='.75px'" : "cx='50%' cy='50%' r='50%' fx='25%' fy='75%'");
					else
						coords = !variant ? "x1='0' y1='0' x2='1' y2='0'" :
							(userSpace ? "x1='0px' y1='0px' x2='1px' y2='0px'" : "x1='0%' y1='0%' x2='100%' y2='0%'");
					snprintf(svg, sizeof(svg), "<svg width='256' height='256'><defs><%s id='g' gradientUnits='%s' %s><stop stop-color='red'/><stop offset='1' stop-color='blue'/></%s></defs><rect x='10' y='20' width='80' height='40' transform='%s' fill='url(#g)' stroke='url(#g)'/></svg>", tag, userSpace ? "userSpaceOnUse" : "objectBoundingBox", coords, tag, transformed ? "translate(3 4) scale(2 3)" : "");
					images[variant] = parse(svg);
					assert(images[variant]->shapes != NULL);
				}
				for (variant = 0; variant < 2; variant++) {
					NSVGshape* a = images[0]->shapes;
					NSVGshape* b = images[1]->shapes;
					NSVGpaint* pa = variant ? &a->stroke : &a->fill;
					NSVGpaint* pb = variant ? &b->stroke : &b->fill;
					double sx = transformed ? 2.0 : 1.0;
					assert(pa->type == (radial ? NSVG_PAINT_RADIAL_GRADIENT : NSVG_PAINT_LINEAR_GRADIENT));
					assert(pb->type == pa->type);
					for (j = 0; j < 6; j++) assert(fabs(pa->gradient->xform[j] - pb->gradient->xform[j]) < 1e-6);
					if (radial) {
						double radius = userSpace ? .5 : sqrt((80*80+40*40)/2.0)*.5;
						assert(fabs(pa->gradient->xform[0] - 1/(radius*sx)) < 1e-6);
						assert(pa->gradient->fx == pb->gradient->fx && pa->gradient->fy == pb->gradient->fy);
						assert(fabs(pa->gradient->fx - (userSpace ? -.25 : -20.0)/radius) < 1e-6);
						assert(fabs(pa->gradient->fy - (userSpace ? .25 : 10.0)/radius) < 1e-6);
					} else {
						double width = (userSpace ? 1.0 : 80.0)*sx;
						double origin = (userSpace ? 0.0 : 10.0)*sx + (transformed ? 3.0 : 0.0);
						assert(fabs(pa->gradient->xform[1] - 1/width) < 1e-6);
						assert(fabs(pa->gradient->xform[5] + origin/width) < 1e-6);
					}
				}
				nsvgDelete(images[0]);
				nsvgDelete(images[1]);
			}
		}
	}
}

static void test_radial_focus(void)
{
	const struct { const char* units; const char* coords; double fx, fy; } cases[] = {
		{"objectBoundingBox", "cx='.25' cy='.75' r='.5'", 0, 0},
		{"userSpaceOnUse", "cx='40' cy='30' r='20'", 0, 0},
		{"userSpaceOnUse", "cx='40' cy='30' r='20' fx='50'", .5, 0},
		{"userSpaceOnUse", "fy='25' cx='40' cy='30' r='20'", 0, -.25},
		{"userSpaceOnUse", "fx='0' cx='40' cy='30' r='20'", -2, 0},
		{"objectBoundingBox", "cx='50%' cy='50%' fx='50%' fy='50%' r='50%'", 0, 0},
		{"objectBoundingBox", "cx='.5' cy='.5' fx='.5' fy='.5' r='.5'", 0, 0},
		{"userSpaceOnUse", "cx='50%' cy='50%' fx='50%' fy='50%' r='50%'", 0, 0},
		{"userSpaceOnUse", "cx='40' cy='30' fx='50' fy='25' r='20'", .5, -.25},
		{"userSpaceOnUse", "cx='40px' cy='30px' fx='40px' fy='30px' r='20px'", 0, 0},
		{"objectBoundingBox", "cx='.5' cy='.5' fx='.25' fy='.75' r='0'", 0, 0},
		{"userSpaceOnUse", "cx='40' cy='30' fx='40' fy='30' r='0'", 0, 0},
		{"userSpaceOnUse", "cx='40' cy='30' fx='50' fy='25' r='-1'", 0, 0}
	};
	size_t i;
	for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		char svg[1024];
		NSVGimage* image;
		int stroke, j;
		snprintf(svg, sizeof(svg), "<svg width='160' height='80' viewBox='10 20 160 80'>"
			"<defs><radialGradient id='g' gradientUnits='%s' %s>"
			"<stop stop-color='red'/></radialGradient></defs>"
			"<rect x='10' y='20' width='80' height='40' fill='url(#g)' stroke='url(#g)'/></svg>",
			cases[i].units, cases[i].coords);
		image = parse(svg);
		assert(image->shapes != NULL);
		for (stroke = 0; stroke < 2; stroke++) {
			NSVGpaint* paint = stroke ? &image->shapes->stroke : &image->shapes->fill;
			assert(paint->type == NSVG_PAINT_RADIAL_GRADIENT && paint->gradient != NULL);
			assert(fabs(paint->gradient->fx - cases[i].fx) < 1e-6);
			assert(fabs(paint->gradient->fy - cases[i].fy) < 1e-6);
			for (j = 0; j < 6; j++) assert(isfinite(paint->gradient->xform[j]));
		}
		nsvgDelete(image);
	}
}

static void test_paint_inheritance() {
    using namespace nanosvg;
    const std::string inherited = "stroke-linecap:inherit;stroke-linejoin:inherit;fill-rule:inherit;paint-order:inherit";
    const std::string overrides = "stroke-linecap='square' stroke-linejoin='miter' fill-rule='nonzero' paint-order='normal' ";
    for (const std::string child : {std::string{},
            std::string("stroke-linecap=' inherit ' stroke-linejoin='inherit' fill-rule='inherit' paint-order='inherit'"),
            "style='" + inherited + "'", overrides + "style='" + inherited + "'", overrides + "class='i'"}) {
        for (bool group : {false, true}) {
            const std::string svg = "<svg><style>.i{" + inherited + "}</style>"
                "<g stroke-linecap='round' stroke-linejoin='bevel' fill-rule='evenodd' paint-order='stroke fill markers'>" +
                (group ? "<g " + child + "><rect width='2' height='2'/></g>" : "<rect width='2' height='2' " + child + "/>") +
                "<rect width='2' height='2'/></g></svg>";
            auto image = nanosvg::parse(svg);
            assert(image && (*image)->shapes.size() == 2);
            for (const auto& shape : (*image)->shapes) {
                assert(shape.strokeLineCap == LineCap::round && shape.strokeLineJoin == LineJoin::bevel);
                assert(shape.fillRule == FillRule::evenodd && shape.paintOrder[0] == PaintOrder::stroke);
            }
        }
    }
    auto root = nanosvg::parse("<svg " + overrides + "style='" + inherited + "'><rect id='inherit' width='2' height='2'/></svg>");
    assert(root && (*root)->shapes.size() == 1);
    const auto& shape = (*root)->shapes[0];
    assert(shape.strokeLineCap == LineCap::butt && shape.strokeLineJoin == LineJoin::miter);
    assert(shape.fillRule == FillRule::nonzero && shape.paintOrder[0] == PaintOrder::fill && shape.id == "inherit");
}

static void test_gradient_rendering() {
    using namespace nanosvg;
    // Multiple stops expose the spread/focus behavior that a single-color gradient hides.
    for (bool radial : {false, true}) for (bool stroke : {false, true}) for (bool transformed : {false, true}) {
        const std::string tag = radial ? "radialGradient" : "linearGradient";
        const std::string coords = radial ? "cx='24' cy='24' r='16' fx='32' fy='24'" : "x1='16' y1='0' x2='32' y2='0'";
        const std::string geometry = stroke ? "<path d='M0 24H64' stroke-width='12' fill='none' stroke='url(#g)'/>" :
            "<rect width='64' height='64' fill='url(#g)'/>";
        for (const char* spread : {"pad", "repeat", "reflect"}) {
            const std::string svg = "<svg width='160' height='160'><defs><" + tag +
                " id='g' gradientUnits='userSpaceOnUse' spreadMethod='" + spread + "' " + coords + ">"
                "<stop stop-color='red'/><stop offset='1' stop-color='blue'/></" + tag + "></defs>" +
                (transformed ? "<g transform='translate(4 6) scale(2)'>" + geometry + "</g>" : geometry) + "</svg>";
            auto image = nanosvg::parse(svg);
            assert(image);
            Rasterizer renderer;
            constexpr int stride = 160*4 + 7;
            std::vector<unsigned char> pixels(160*stride, 0xcd), cPixels(pixels);
            const double scale = transformed ? .5 : 1;
            const double tx = transformed ? 7 : 0, ty = transformed ? 9 : 0;
            assert(renderer.rasterize(**image, pixels, 160, 160, stride, tx, ty, scale));
            auto cImage = parse(svg.c_str());
            auto cRenderer = nsvgCreateRasterizer();
            assert(cRenderer);
            nsvgRasterize(cRenderer, cImage, tx, ty, scale, cPixels.data(), 160, 160, stride);
            assert(cPixels == pixels);
            for (int x : {8, 16, 24, 32, 36, 40, 44, 48, 56}) {
                const int px = x + (transformed ? 9 : 0), py = 24 + (transformed ? 12 : 0);
                double t = radial ? (x >= 32 ? (x - 32)/8.0 : (32 - x)/24.0) : (x - 16)/16.0;
                if (!strcmp(spread, "repeat")) t -= std::floor(t);
                else if (!strcmp(spread, "reflect")) {
                    t = std::fmod(std::abs(t), 2.0);
                    if (t > 1) t = 2 - t;
                } else t = std::clamp(t, 0.0, 1.0);
                const auto* pixel = &pixels[py*stride + px*4];
                assert(std::abs(pixel[0] - (1-t)*255) <= 3 && pixel[1] == 0);
                assert(std::abs(pixel[2] - t*255) <= 3 && pixel[3] == 255);
            }
            for (int y = 0; y < 160; ++y)
                for (int x = 640; x < stride; ++x) assert(pixels[y*stride + x] == 0xcd);
            nsvgDeleteRasterizer(cRenderer);
            nsvgDelete(cImage);
        }
    }
}

static void test_gradient_sampling_limits() {
    using namespace nanosvg;
    using namespace nanosvg::detail;
    // Check the gradient blend fast paths against the existing solid-paint path.
    CachedPaint solid{};
    solid.type = PaintKind::color;
    for (unsigned alpha = 0; alpha < 256; ++alpha) for (unsigned cover = 0; cover < 256; ++cover) {
        solid.colors[0] = (alpha << 24) | 0x00cb5b11;
        std::array<unsigned char, 4> expected{20, 40, 60, 80}, actual = expected;
        auto coverage = static_cast<unsigned char>(cover);
        nsvg__scanlineSolid(expected.data(), 1, &coverage, 0, 0, 0, 0, 1, &solid);
        nsvg__blendPixel(actual.data(), coverage, solid.colors[0]);
        assert(actual == expected);
    }
    assert(nsvg__gradientIndex(-.25, Spread::repeat) == 191);
    assert(nsvg__gradientIndex(-.25, Spread::reflect) == 63);
    assert(nsvg__gradientIndex(1, Spread::repeat) == 0);
    assert(nsvg__gradientIndex(1, Spread::reflect) == 255);
    for (Spread spread : {Spread::pad, Spread::reflect, Spread::repeat}) {
        assert(nsvg__gradientIndex(INFINITY, spread) == 255);
        assert(nsvg__gradientIndex(-INFINITY, spread) == 0);
        assert(nsvg__gradientIndex(NAN, spread) == 0);
    }
    Gradient gradient;
    gradient.kind = GradientKind::radial;
    gradient.stops = {{0xff0000ff, 0}, {0xffff0000, 1}};
    for (double focus : {0.0, .5, 1 - 1e-12, 1.0, 2.0, DBL_MAX}) {
        gradient.fx = focus;
        Paint paint = gradient;
        CachedPaint cache{};
        nsvg__initPaint(&cache, &paint, 1);
        assert(nsvg__radialDistance(cache.fx, cache.fy, &cache) == 0);
        assert(std::abs(nsvg__radialDistance(-1, 0, &cache) - 1) < 1e-12);
        assert(std::abs(nsvg__radialDistance(0, 0, &cache) - cache.fx/(1 + cache.fx)) < 1e-12);
        if (focus < 1) assert(std::abs(nsvg__radialDistance(1, 0, &cache) - 1) < 1e-12);
        else assert(std::isinf(nsvg__radialDistance(2, 0, &cache)));
    }
    gradient.fx = gradient.fy = DBL_MAX;
    Paint paint = gradient;
    CachedPaint cache{};
    nsvg__initPaint(&cache, &paint, 1);
    assert(std::abs(std::hypot(cache.fx, cache.fy) - 1) < 1e-12);
    gradient.fx = .3;
    gradient.fy = -.4;
    paint = gradient;
    nsvg__initPaint(&cache, &paint, 1);
    for (double angle : {0.0, .7, 1.5, 3.0, 5.0}) for (double t : {.2, 1.0, 1.5}) {
        const double x = (1-t)*gradient.fx + t*std::cos(angle);
        const double y = (1-t)*gradient.fy + t*std::sin(angle);
        assert(std::abs(nsvg__radialDistance(x, y, &cache) - t) < 1e-12);
    }

    // Many short periods expose accumulation drift at repeat boundaries.
    gradient.kind = GradientKind::linear;
    gradient.fx = gradient.fy = 0;
    gradient.spread = Spread::repeat;
    gradient.xform = {0, .1, 0, 0, 0, 0};
    paint = gradient;
    nsvg__initPaint(&cache, &paint, 1);
    std::vector<unsigned char> pixels(40000), coverage(10000, 255);
    nsvg__scanlineSolid(pixels.data(), 10000, coverage.data(), 0, 0, 0, 0, 1, &cache);
    for (int x = 0; x < 10000; x += 10) assert(pixels[x*4] == 255 && pixels[x*4+2] == 0);

    const char* svg = "<svg><defs><radialGradient id='g'><stop stop-color='red'/></radialGradient></defs>"
        "<rect width='2' height='2' fill='url(#g)'/></svg>";
    auto image = nanosvg::parse(svg);
    assert(image);
    Rasterizer renderer;
    std::array<unsigned char, 16> output;
    output.fill(0xcd);
    const auto untouched = output;
    auto& edited = std::get<Gradient>((*image)->shapes[0].fill);
    for (double value : {NAN, INFINITY, -INFINITY}) for (double* coord : {&edited.fx, &edited.fy}) {
        *coord = value;
        auto result = renderer.rasterize(**image, output, 2, 2, 8);
        assert(!result && result.error() == Error::invalid_argument && output == untouched);
        *coord = 0;
    }
    auto cImage = parse(svg);
    auto cRenderer = nsvgCreateRasterizer();
    assert(cRenderer);
    auto* cGradient = cImage->shapes->fill.gradient;
    for (double value : {NAN, INFINITY, -INFINITY}) for (double* coord : {&cGradient->fx, &cGradient->fy}) {
        *coord = value;
        nsvgRasterize(cRenderer, cImage, 0, 0, 1, output.data(), 2, 2, 8);
        assert(output == untouched);
        *coord = 0;
    }
    nsvgRasterize(cRenderer, cImage, 0, 0, 1, output.data(), 2, 2, 8);
    assert(output[0] == 255 && output[3] == 255);
    nsvgDeleteRasterizer(cRenderer);
    nsvgDelete(cImage);
}

static void test_gradient_opacity(void)
{
	const struct { double fill, stroke, stop, shape; int fillAlpha, strokeAlpha; } cases[] = {
		{0, 0, 1, 1, 0, 0}, {1, 1, 1, 1, 255, 255},
		{.25, .5, 1, 1, 63, 127}, {.25, .5, .5, .5, 31, 63},
		{1, 1, 0, 1, 0, 0}, {1, 1, 1, 0, 255, 255}
	};
	const char* invalid[] = {"#missing", "#empty", "#broken", "#cycle", "#", ""};
	size_t i;
	int kind, s, stroke, j;
	for (kind = 0; kind < 3; kind++) {
		for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
			char svg[2048];
			unsigned char pixels[64*64*4];
			const char* tag = kind == 2 ? "radialGradient" : "linearGradient";
			const char* paint = kind ? "url(#g)" : "red";
			NSVGimage* image;
			NSVGshape* shape;
			// Forward references and swapped opacities catch shared-stop mutation.
			snprintf(svg, sizeof(svg), "<svg width='64' height='64'>"
				"<g fill='%s' stroke='%s' stroke-width='4' opacity='%g'>"
				"<rect x='8' y='16' width='16' height='32' fill-opacity='%g' stroke-opacity='%g'/>"
				"<rect x='40' y='16' width='16' height='32' fill-opacity='%g' stroke-opacity='%g'/></g>"
				"<defs><%s id='g'><stop stop-color='red' stop-opacity='%g'/>"
				"<stop offset='1' stop-color='red' stop-opacity='%g'/></%s></defs></svg>",
				paint, paint, cases[i].shape, cases[i].fill, cases[i].stroke,
				cases[i].stroke, cases[i].fill, tag, cases[i].stop, cases[i].stop, tag);
			image = parse(svg);
			render(image, pixels);
			shape = image->shapes;
			for (s = 0; s < 2; s++, shape = shape->next) {
				assert(shape != NULL && shape->opacity == cases[i].shape);
				for (stroke = 0; stroke < 2; stroke++) {
					NSVGpaint* p = stroke ? &shape->stroke : &shape->fill;
					int alpha = s != stroke ? cases[i].strokeAlpha : cases[i].fillAlpha;
					int x = 8 + s*32 + (stroke ? -1 : 8);
					if (!kind)
						alpha = (int)((s != stroke ? cases[i].stroke : cases[i].fill)*255);
					assert(p->type == (kind == 0 ? NSVG_PAINT_COLOR :
						(kind == 1 ? NSVG_PAINT_LINEAR_GRADIENT : NSVG_PAINT_RADIAL_GRADIENT)));
					if (kind) {
						assert(p->gradient != NULL && p->gradient->nstops == 2);
						for (j = 0; j < p->gradient->nstops; j++) {
							assert((p->gradient->stops[j].color >> 24) == (unsigned int)alpha);
							assert((p->gradient->stops[j].color & 0xffffff) == nanosvg::detail::rgb(255,0,0));
						}
					} else {
						assert((p->color >> 24) == (unsigned int)alpha);
					}
					assert(pixels[(32*64+x)*4+3] == (int)(alpha*cases[i].shape));
				}
			}
			assert(shape == NULL);
			nsvgDelete(image);
		}
	}
	for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
		char svg[1024];
		unsigned char pixels[64*64*4];
		NSVGimage* image;
		snprintf(svg, sizeof(svg), "<svg width='64' height='64'><defs>"
			"<linearGradient id='empty'/><radialGradient id='broken' xlink:href='#missing'/>"
			"<linearGradient id='cycle' xlink:href='#cycle'/></defs>"
			"<rect width='32' height='32' fill='url(%s)' stroke='url(%s)' fill-opacity='.25' stroke-opacity='.5'/></svg>",
			invalid[i], invalid[i]);
		image = parse(svg);
		assert(image->shapes != NULL);
		assert(image->shapes->fill.type == NSVG_PAINT_NONE && image->shapes->stroke.type == NSVG_PAINT_NONE);
		render(image, pixels);
		for (j = 0; j < 64*64; j++) assert(pixels[j*4+3] == 0);
		nsvgDelete(image);
	}
}

static void test_visibility(void)
{
	const struct { const char* parent; const char* child; int visible; } cases[] = {
		{"", "", 1}, {"visibility='hidden'", "", 0},
		{"visibility='hidden'", "visibility='visible'", 1},
		{"visibility='hidden'", "visibility='inherit'", 0},
		{"visibility='hidden'", "visibility='invalid'", 0},
		{"visibility='visible'", "visibility='hidden'", 0},
		{"visibility='collapse'", "", 0},
		{"visibility='collapse'", "visibility='visible'", 1},
		{"class='hide'", "class='show'", 1},
		{"class='hide'", "", 0},
		{"style='visibility:hidden'", "style='visibility:visible'", 1},
		{"style='visibility:hidden'", "style='visibility:inherit'", 0},
		{"display='none'", "visibility='visible' display='inline'", 0},
		{"display='none'", "display='inline' visibility='visible'", 0},
		{"class='gone'", "class='show'", 0},
		{"style='display:none;visibility:hidden'", "style='display:inline;visibility:visible'", 0}
	};
	size_t i;
	for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		char svg[1024];
		unsigned char pixels[64*64*4];
		NSVGimage* image;
		snprintf(svg, sizeof(svg), "<svg width='64' height='64'><style>.hide{visibility:hidden;}.show{visibility:visible;}.gone{display:none;}</style><g %s><g><rect %s x='8' y='8' width='16' height='16'/></g></g><rect x='32' y='8' width='16' height='16'/></svg>", cases[i].parent, cases[i].child);
		image = parse(svg);
		assert(image->shapes != NULL && image->shapes->next != NULL);
		assert(image->shapes->flags == (cases[i].visible ? NSVG_FLAGS_VISIBLE : 0));
		assert(image->shapes->next->flags == NSVG_FLAGS_VISIBLE);
		render(image, pixels);
		assert(pixels[(16*64+16)*4+3] == (cases[i].visible ? 255 : 0));
		assert(pixels[(16*64+40)*4+3] == 255);
		nsvgDelete(image);
	}
}

static void test_value_helpers(void)
{
    using namespace nanosvg;
    using namespace nanosvg::detail;
    static_assert(!std::is_copy_constructible_v<Parser>);
    static_assert(!std::is_move_constructible_v<Parser>);
    const Transform transform{2, 0, 0, 3, 4, 5};
    const auto before = transform;
    const Point point{1, 2};
    const auto result = transform_point(point, transform);
    assert(result.x == 6 && result.y == 11);
    assert(point.x == 1 && point.y == 2 && transform == before);
    const std::array<Point, 4> curve{{{0, 0}, {0, 40}, {40, 40}, {40, 0}}};
    const auto original = curve;
    const auto bounds = curve_bounds(curve);
    assert((bounds == Bounds{0, 0, 40, 30}));
    for (std::size_t i = 0; i < curve.size(); ++i)
        assert(curve[i].x == original[i].x && curve[i].y == original[i].y);
    const std::string text = "12.5e999";
    const auto number = parse_number(std::string_view(text).substr(0, 4));
    assert(number.error == std::errc{} && number.value == 12.5 && number.consumed == 4);
    assert(text == "12.5e999");
    assert(parse_number(text).error == std::errc::result_out_of_range);
    assert(parse_number(".").error == std::errc::invalid_argument);
    assert(parse_number("2em").consumed == 1);
    assert(parse_coordinate_raw("2em").units == CoordinateUnit::em);
    assert(parse_coordinate_raw("2ex").units == CoordinateUnit::ex);
}

static void test_examples(void)
{
	const char* files[] = {"example/nano.svg", "example/drawing.svg", "example/23.svg"};
	size_t i;
	for (i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
		NSVGimage* image = nsvgParseFromFile(files[i], "px", 96);
		NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
		unsigned char pixels[64*64*4];
		int j, visible = 0;
		assert(image != NULL && image->shapes != NULL && rasterizer != NULL);
		assert(isfinite(image->width) && isfinite(image->height));
		assert(image->width > 0 && image->height > 0);
		nsvgRasterize(rasterizer, image, 0, 0, 64/fmax(image->width, image->height), pixels, 64, 64, 64*4);
		for (j = 0; j < 64*64; j++) visible |= pixels[j*4+3];
		assert(visible != 0);
		nsvgDeleteRasterizer(rasterizer);
		nsvgDelete(image);
	}
}

int main(void)
{
#define RUN(fn) do { puts(#fn); fflush(stdout); fn(); } while (0)
	RUN(test_css_recursion);
	RUN(test_css_bounds);
	RUN(test_dashes);
	RUN(test_numeric);
	RUN(test_inverse);
	RUN(test_transforms);
	RUN(test_arcs);
	RUN(test_gradients);
	RUN(test_radial_focus);
	RUN(test_paint_inheritance);
	RUN(test_gradient_rendering);
	RUN(test_gradient_sampling_limits);
	RUN(test_gradient_opacity);
	RUN(test_visibility);
	RUN(test_value_helpers);
	RUN(test_examples);
	puts("NanoSVG regression checks passed");
	return 0;
}
