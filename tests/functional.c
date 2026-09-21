/* Public C API contract tests, also linked against installed libraries. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NSVG_TEST_EXTERNAL
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#endif
#include "nanosvg.h"
#include "nanosvgrast.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define RUN(fn) do { puts(#fn); fflush(stdout); fn(); } while (0)

static const char* scratch;
static char context[8192];
static void near_value(float actual, float expected, float tolerance)
{
	if (!(fabsf(actual - expected) <= tolerance)) {
		fprintf(stderr, "expected %.9g, got %.9g (tolerance %.9g)\n", expected, actual, tolerance);
		fprintf(stderr, "SVG: %s\n", context);
		abort();
	}
}

static NSVGimage* parse_units(const char* svg, const char* units, float dpi)
{
	char* input = (char*)malloc(strlen(svg) + 1);
	NSVGimage* image;
	assert(input != NULL);
	snprintf(context, sizeof(context), "%s", svg);
	strcpy(input, svg);
	image = nsvgParse(input, units, dpi);
	free(input);
	assert(image != NULL);
	return image;
}

static NSVGimage* parse(const char* svg)
{
	return parse_units(svg, "px", 96);
}

static NSVGimage* element(const char* body)
{
	char svg[8192];
	int n = snprintf(svg, sizeof(svg), "<svg width='256' height='256'>%s</svg>", body);
	assert(n >= 0 && (size_t)n < sizeof(svg));
	return parse(svg);
}

static void bounds(const float* actual, float x0, float y0, float x1, float y1)
{
	near_value(actual[0], x0, 1e-3f);
	near_value(actual[1], y0, 1e-3f);
	near_value(actual[2], x1, 1e-3f);
	near_value(actual[3], y1, 1e-3f);
}

static void valid_paths(NSVGimage* image)
{
	NSVGshape* shape;
	for (shape = image->shapes; shape; shape = shape->next) {
		NSVGpath* path;
		assert(shape->paths != NULL);
		assert(shape->fill.type >= NSVG_PAINT_NONE && shape->fill.type <= NSVG_PAINT_RADIAL_GRADIENT);
		assert(shape->stroke.type >= NSVG_PAINT_NONE && shape->stroke.type <= NSVG_PAINT_RADIAL_GRADIENT);
		for (path = shape->paths; path; path = path->next) {
			int j;
			assert(path->pts != NULL && path->npts >= 4 && path->npts % 3 == 1);
			for (j = 0; j < path->npts*2; j++) assert(isfinite(path->pts[j]));
			for (j = 0; j < 4; j++) assert(isfinite(path->bounds[j]));
			assert(path->bounds[0] <= path->bounds[2] && path->bounds[1] <= path->bounds[3]);
		}
	}
}

static void same_paths(NSVGimage* a, NSVGimage* b)
{
	NSVGpath *pa, *pb;
	assert(a->shapes && b->shapes);
	pa = a->shapes->paths;
	pb = b->shapes->paths;
	for (; pa && pb; pa = pa->next, pb = pb->next) {
		int i;
		assert(pa->npts == pb->npts && pa->closed == pb->closed);
		for (i = 0; i < pa->npts*2; i++) near_value(pa->pts[i], pb->pts[i], 1e-3f);
		for (i = 0; i < 4; i++) near_value(pa->bounds[i], pb->bounds[i], 1e-3f);
	}
	assert(pa == NULL && pb == NULL);
}

static void render(NSVGimage* image, unsigned char* pixels)
{
	NSVGrasterizer* r = nsvgCreateRasterizer();
	assert(r != NULL);
	nsvgRasterize(r, image, 0, 0, 1, pixels, 64, 64, 64*4);
	nsvgDeleteRasterizer(r);
}

static void rgba(const unsigned char* pixels, int x, int y, int red, int green, int blue, int alpha, int tolerance)
{
	const unsigned char* p = pixels + (y*64+x)*4;
	near_value(p[0], (float)red, (float)tolerance);
	near_value(p[1], (float)green, (float)tolerance);
	near_value(p[2], (float)blue, (float)tolerance);
	near_value(p[3], (float)alpha, (float)tolerance);
}

static void test_defaults(void)
{
	const char* empty[] = {"", " ", "<svg></svg>", "<?xml version='1.0'?><svg/>", "<!-- empty -->"};
	size_t i;
	NSVGimage* image;
	NSVGshape* shape;
	for (i = 0; i < COUNT(empty); i++) {
		image = parse(empty[i]);
		assert(image->shapes == NULL && image->width == 0 && image->height == 0);
		nsvgDelete(image);
	}
	image = element("<rect id='rect' x='8' y='12' width='16' height='20'/>");
	shape = image->shapes;
	assert(shape && !shape->next && strcmp(shape->id, "rect") == 0);
	assert(shape->fill.type == NSVG_PAINT_COLOR && shape->fill.color == 0xff000000u);
	assert(shape->stroke.type == NSVG_PAINT_NONE && shape->opacity == 1);
	assert(shape->strokeWidth == 1 && shape->strokeDashCount == 0);
	assert(shape->strokeLineCap == NSVG_CAP_BUTT && shape->strokeLineJoin == NSVG_JOIN_MITER);
	assert(shape->miterLimit == 4 && shape->fillRule == NSVG_FILLRULE_NONZERO);
	assert(shape->flags == NSVG_FLAGS_VISIBLE);
	bounds(shape->bounds, 8, 12, 24, 32);
	valid_paths(image);
	nsvgDelete(image);
	nsvgDelete(NULL);
	nsvgDeleteRasterizer(NULL);
	assert(nsvgDuplicatePath(NULL) == NULL);
}

static void test_primitives(void)
{
	const struct { const char* svg; float box[4]; int closed; } cases[] = {
		{"<rect x='8' y='12' width='16' height='20'/>", {8,12,24,32}, 1},
		{"<rect x='8' y='12' width='16' height='20' rx='3'/>", {8,12,24,32}, 1},
		{"<rect x='8' y='12' width='16' height='20' ry='50'/>", {8,12,24,32}, 1},
		{"<circle cx='32' cy='24' r='10'/>", {22,14,42,34}, 1},
		{"<ellipse cx='32' cy='24' rx='12' ry='6'/>", {20,18,44,30}, 1},
		{"<line x1='8' y1='12' x2='24' y2='32'/>", {8,12,24,32}, 0},
		{"<polyline points='8,12 24,12 24,32'/>", {8,12,24,32}, 0},
		{"<polygon points='8,12 24,12 24,32'/>", {8,12,24,32}, 1},
		{"<path d='M8 12H24V32H8Z'/>", {8,12,24,32}, 1}
	};
	const char* degenerate[] = {"<rect width='0' height='5'/>", "<rect width='5' height='0'/>",
		"<circle r='0'/>", "<ellipse rx='5' ry='0'/>", "<polyline points='1,2'/>", "<path d='M1 2'/>", "<path/>"};
	size_t i;
	for (i = 0; i < COUNT(cases); i++) {
		NSVGimage* image = element(cases[i].svg);
		NSVGpath* path;
		assert(image->shapes && !image->shapes->next);
		path = image->shapes->paths;
		assert(path && !path->next && path->closed == cases[i].closed);
		bounds(image->shapes->bounds, cases[i].box[0], cases[i].box[1], cases[i].box[2], cases[i].box[3]);
		if (path->closed) {
			near_value(path->pts[0], path->pts[path->npts*2-2], 1e-5f);
			near_value(path->pts[1], path->pts[path->npts*2-1], 1e-5f);
		}
		valid_paths(image);
		nsvgDelete(image);
	}
	for (i = 0; i < COUNT(degenerate); i++) {
		NSVGimage* image = element(degenerate[i]);
		assert(!image->shapes);
		nsvgDelete(image);
	}
}

static void test_path_commands(void)
{
	const char* pairs[][2] = {
		{"M10 20L30 20L30 30L10 30Z", "m10 20h20v10h-20z"},
		{"M10 20L30 20L30 30", "M10,20 30,20 30,30"},
		{"M10 20L30 20L30 30", "m10 20 20 0 0 10"},
		{"M10 10C20 0 30 0 40 10C50 20 60 20 70 10", "m10 10c10 -10 20 -10 30 0s20 10 30 0"},
		{"M10 10C20 0 30 0 40 10C50 20 60 20 70 10", "M10 10C20 0 30 0 40 10S60 20 70 10"},
		{"M10 10Q20 0 30 10Q40 20 50 10", "m10 10q10 -10 20 0t20 0"},
		{"M10 10Q20 0 30 10Q40 20 50 10", "M10 10Q20 0 30 10T50 10"},
		{"M0 0Q15 30 30 0", "M0 0C10 20 20 20 30 0"},
		{"M0 0L10 10C10 10 20 20 30 10", "M0 0L10 10S20 20 30 10"},
		{"M0 0L10 10Q10 10 30 10", "M0 0L10 10T30 10"},
		{"M10 20L30 40", "M1e1,+2E1L.3e2 40."},
		{"M10 10L20 10Z M30 30L40 40", "m10 10l10 0z m20 20l10 10"},
		{"M10 10L30 20", "M10 10A0 8 0 0 1 30 20"}
	};
	size_t i;
	int large, sweep;
	for (i = 0; i < COUNT(pairs); i++) {
		char body[1024];
		NSVGimage *a, *b;
		snprintf(body, sizeof(body), "<path d='%s'/>", pairs[i][0]);
		a = element(body);
		snprintf(body, sizeof(body), "<path d='%s'/>", pairs[i][1]);
		b = element(body);
		valid_paths(a);
		valid_paths(b);
		same_paths(a, b);
		nsvgDelete(a);
		nsvgDelete(b);
	}
	for (large = 0; large < 2; large++) for (sweep = 0; sweep < 2; sweep++) {
		char body[256];
		NSVGimage *a, *b;
		NSVGpath* path;
		snprintf(body, sizeof(body), "<path d='M10 20A25 15 30 %d %d 40 35'/>", large, sweep);
		a = element(body);
		snprintf(body, sizeof(body), "<path d='m10 20a-25 -15 30 %d%d30 15'/>", large, sweep);
		b = element(body);
		same_paths(a, b);
		valid_paths(a);
		path = a->shapes->paths;
		near_value(path->pts[path->npts*2-2], 40, 1e-3f);
		near_value(path->pts[path->npts*2-1], 35, 1e-3f);
		nsvgDelete(a);
		nsvgDelete(b);
	}
	{
		NSVGimage* image = element("<path d='M0 0C0 40 40 40 40 0'/>");
		bounds(image->shapes->bounds, 0, 0, 40, 30);
		nsvgDelete(image);
		image = element("<path d='M0 0Q20 40 40 0'/>");
		bounds(image->shapes->bounds, 0, 0, 40, 20);
		nsvgDelete(image);
	}
}

static void test_units(void)
{
	const char* units[] = {"px", "pt", "pc", "mm", "cm", "in", "em", "ex"};
	const float dpi[] = {72, 96, 144};
	size_t i, d;
	for (d = 0; d < COUNT(dpi); d++) {
		float factors[] = {1, dpi[d]/72, dpi[d]/6, dpi[d]/25.4f, dpi[d]/2.54f, dpi[d], 10, 5.2f};
		for (i = 0; i < COUNT(units); i++) {
			char svg[512];
			NSVGimage* image;
			snprintf(svg, sizeof(svg), "<svg width='1024' height='1024'><g font-size='10'>"
				"<rect x='1%s' y='2%s' width='3%s' height='4%s'/></g></svg>", units[i], units[i], units[i], units[i]);
			image = parse_units(svg, "px", dpi[d]);
			assert(image->shapes);
			bounds(image->shapes->bounds, factors[i], 2*factors[i], 4*factors[i], 6*factors[i]);
			nsvgDelete(image);
			if (i < 6) {
				image = parse_units("<svg width='1024' height='1024'><rect x='96' y='192' width='96' height='96'/></svg>", units[i], dpi[d]);
				bounds(image->shapes->bounds, 96/factors[i], 192/factors[i], 192/factors[i], 288/factors[i]);
				nsvgDelete(image);
			}
		}
	}
	{
		NSVGimage* image = parse("<svg width='200' height='100' viewBox='0 0 200 100'><rect x='10%' y='20%' width='50%' height='50%'/></svg>");
		bounds(image->shapes->bounds, 20, 20, 120, 70);
		nsvgDelete(image);
		image = parse("<svg width='1in' height='25.4mm'><rect width='1in' height='72pt'/></svg>");
		near_value(image->width, 96, 1e-4f);
		near_value(image->height, 96, 1e-4f);
		bounds(image->shapes->bounds, 0, 0, 96, 96);
		nsvgDelete(image);
	}
}

static void test_viewbox(void)
{
	const char* xs[] = {"xMin", "xMid", "xMax"};
	const char* ys[] = {"YMin", "YMid", "YMax"};
	int x, y, slice, portrait;
	for (x = 0; x < 3; x++) for (y = 0; y < 3; y++)
	for (slice = 0; slice < 2; slice++) for (portrait = 0; portrait < 2; portrait++) {
		char svg[512];
		float side = slice ? 200.0f : 100.0f;
		float left = ((portrait ? 100 : 200) - side)*x/2;
		float top = ((portrait ? 200 : 100) - side)*y/2;
		NSVGimage* image;
		snprintf(svg, sizeof(svg), "<svg width='%d' height='%d' viewBox='10 20 100 100' preserveAspectRatio='%s%s %s'>"
			"<rect x='10' y='20' width='100' height='100'/></svg>",
			portrait ? 100 : 200, portrait ? 200 : 100, xs[x], ys[y], slice ? "slice" : "meet");
		image = parse(svg);
		assert(image->shapes);
		valid_paths(image);
		bounds(image->shapes->bounds, left, top, left+side, top+side);
		nsvgDelete(image);
	}
	{
		NSVGimage* image = parse("<svg width='200' height='100' viewBox='10 20 100 100' preserveAspectRatio='none'>"
			"<rect x='10' y='20' width='100' height='100'/></svg>");
		bounds(image->shapes->bounds, 0, 0, 200, 100);
		nsvgDelete(image);
		image = parse("<svg><rect x='10' y='20' width='30' height='40'/></svg>");
		assert(image->width == 30 && image->height == 40);
		bounds(image->shapes->bounds, 0, 0, 30, 40);
		nsvgDelete(image);
		image = parse("<svg viewBox='10 20 100 200'><rect x='10' y='20' width='30' height='40'/></svg>");
		assert(image->width == 100 && image->height == 200);
		bounds(image->shapes->bounds, 0, 0, 30, 40);
		nsvgDelete(image);
	}
}

static void test_percentages_without_viewbox(void)
{
	NSVGimage* image = parse("<svg width='200' height='100'><rect x='10%' y='20%' width='50%' height='50%'/></svg>");
	const char* bodies[] = {
		"<rect x='10%' y='20%' width='50%' height='50%' rx='5%' ry='10%' stroke='red' stroke-width='10%' stroke-dasharray='10%,20%' stroke-dashoffset='5%'/>",
		"<circle cx='50%' cy='50%' r='10%'/>",
		"<ellipse cx='50%' cy='50%' rx='10%' ry='20%'/>",
		"<line x1='10%' y1='20%' x2='80%' y2='90%'/>",
		"<defs><linearGradient id='g' gradientUnits='userSpaceOnUse' x1='10%' y1='20%' x2='90%' y2='80%'><stop/><stop offset='1' stop-color='red'/></linearGradient></defs><rect width='100%' height='100%' fill='url(#g)'/>",
		"<defs><radialGradient id='g' gradientUnits='userSpaceOnUse' cx='50%' cy='50%' r='30%' fx='25%' fy='75%'><stop/><stop offset='1' stop-color='red'/></radialGradient></defs><rect width='100%' height='100%' fill='url(#g)'/>"
	};
	size_t i;
	assert(image->shapes);
	bounds(image->shapes->bounds, 20, 20, 120, 70);
	nsvgDelete(image);
	for (i = 0; i < COUNT(bodies); i++) {
		char svg[2048];
		NSVGimage* explicitView;
		NSVGshape *a, *b;
		int j;
		snprintf(svg, sizeof(svg), "<svg width='200' height='100'>%s</svg>", bodies[i]);
		image = parse(svg);
		snprintf(svg, sizeof(svg), "<svg width='200' height='100' viewBox='0 0 200 100'>%s</svg>", bodies[i]);
		explicitView = parse(svg);
		same_paths(image, explicitView);
		a = image->shapes;
		b = explicitView->shapes;
		near_value(a->strokeWidth, b->strokeWidth, 1e-4f);
		near_value(a->strokeDashOffset, b->strokeDashOffset, 1e-4f);
		assert(a->strokeDashCount == b->strokeDashCount);
		for (j = 0; j < a->strokeDashCount; j++) near_value(a->strokeDashArray[j], b->strokeDashArray[j], 1e-4f);
		if (i == 0) near_value(a->strokeWidth, sqrtf(25000.0f)*.1f, 1e-4f);
		if (i == 1) {
			float radius = sqrtf(25000.0f)*.1f;
			bounds(a->bounds, 100-radius, 50-radius, 100+radius, 50+radius);
		}
		if (a->fill.type == NSVG_PAINT_LINEAR_GRADIENT || a->fill.type == NSVG_PAINT_RADIAL_GRADIENT) {
			assert(a->fill.type == b->fill.type);
			for (j = 0; j < 6; j++) near_value(a->fill.gradient->xform[j], b->fill.gradient->xform[j], 1e-4f);
			if (a->fill.type == NSVG_PAINT_RADIAL_GRADIENT) {
				near_value(a->fill.gradient->fx, b->fill.gradient->fx, 1e-4f);
				near_value(a->fill.gradient->fy, b->fill.gradient->fy, 1e-4f);
			}
		}
		nsvgDelete(explicitView);
		nsvgDelete(image);
	}
}

static void test_colors(void)
{
	const struct { const char* text; unsigned int rgb; } cases[] = {
		{"red", 0x0000ff}, {"green", 0x008000}, {"blue", 0xff0000},
		{"black", 0}, {"white", 0xffffff}, {"yellow", 0x00ffff},
		{"cyan", 0xffff00}, {"magenta", 0xff00ff}, {"gray", 0x808080}, {"grey", 0x808080},
		{"#123", 0x332211}, {"#12aBcD", 0xcdab12},
		{"rgb(255,128,0)", 0x0080ff}, {"rgb(300,256,128)", 0x80ffff},
		{"rgb(100%,0%,50%)", 0x8000ff}, {"rgb( 0%, +100%, 0% )", 0x00ff00},
		{"unknown", 0x808080}, {"#x", 0x808080}, {"rgb(1)", 0x808080}
	};
	const char* opacity[] = {"-1", "0", ".25", "1", "2"};
	const unsigned int alpha[] = {0, 0, 63, 255, 255};
	size_t i, j;
	for (i = 0; i < COUNT(cases); i++) for (j = 0; j < COUNT(opacity); j++) {
		char body[512];
		NSVGimage* image;
		snprintf(body, sizeof(body), "<rect width='16' height='16' fill='%s' stroke='%s' fill-opacity='%s' stroke-opacity='%s'/>",
			cases[i].text, cases[i].text, opacity[j], opacity[j]);
		image = element(body);
		assert(image->shapes->fill.color == (cases[i].rgb | alpha[j]<<24));
		assert(image->shapes->stroke.color == image->shapes->fill.color);
		nsvgDelete(image);
	}
#ifdef NANOSVG_ALL_COLOR_KEYWORDS
	{
		NSVGimage* image = element("<rect width='16' height='16' fill='aliceblue' stroke='darkslategray'/>");
		assert(image->shapes->fill.color == 0xfffff8f0u);
		assert(image->shapes->stroke.color == 0xff4f4f2fu);
		nsvgDelete(image);
	}
#endif
}

static void test_styles_and_transforms(void)
{
	NSVGimage* image = element("<style>.paint{fill:#00ff00;stroke-width:3;}.edge{stroke:blue;}</style>"
		"<g fill='red' stroke='blue' stroke-width='4' transform='translate(3 4)'>"
		"<g transform='scale(2)'><rect id='first' x='5' y='6' width='10' height='12' "
		"class='paint edge' style='fill-opacity:.5;stroke-dasharray:2,3;stroke-dashoffset:1;stroke-miterlimit:2'/></g>"
		"<rect id='second' width='10' height='10'/></g><rect id='third' width='10' height='10'/>");
	NSVGshape* s = image->shapes;
	assert(s && strcmp(s->id, "first") == 0);
	bounds(s->bounds, 13, 16, 33, 40);
	assert(s->fill.color == 0x7f00ff00u && s->stroke.color == 0xffff0000u);
	assert(s->strokeWidth == 6 && s->strokeDashOffset == 2 && s->miterLimit == 2);
	assert(s->strokeDashCount == 2 && s->strokeDashArray[0] == 4 && s->strokeDashArray[1] == 6);
	s = s->next;
	assert(s && strcmp(s->id, "second") == 0 && s->fill.color == 0xff0000ffu && s->strokeWidth == 4);
	bounds(s->bounds, 3, 4, 13, 14);
	s = s->next;
	assert(s && strcmp(s->id, "third") == 0 && s->fill.color == 0xff000000u);
	assert(!s->next && s->stroke.type == NSVG_PAINT_NONE);
	nsvgDelete(image);
	image = element("<style>.paint{fill:red;}</style><rect width='8' height='8' class='painter paintx'/>");
	assert(image->shapes->fill.color == 0xff000000u);
	nsvgDelete(image);
}

static void test_gradient_data(void)
{
	const char* spreads[] = {"pad", "reflect", "repeat"};
	const int spreadValues[] = {NSVG_SPREAD_PAD, NSVG_SPREAD_REFLECT, NSVG_SPREAD_REPEAT};
	int radial;
	size_t i;
	for (radial = 0; radial < 2; radial++) for (i = 0; i < COUNT(spreads); i++) {
		char body[2048];
		const char* tag = radial ? "radialGradient" : "linearGradient";
		NSVGimage* image;
		NSVGgradient *fill, *stroke;
		snprintf(body, sizeof(body), "<rect width='64' height='64' fill='url(#copy)' stroke='url(#copy)'/>"
			"<defs><%s id='copy' xlink:href='#middle' spreadMethod='%s' %s/>"
			"<%s id='middle' xlink:href='#base'/><%s id='base'>"
			"<stop offset='1' stop-color='blue'/><stop offset='0' stop-color='red'/>"
			"<stop offset='50%%' style='stop-color:#00ff00;stop-opacity:.5'/></%s></defs>",
			tag, spreads[i], radial ? "cx='.5' cy='.5' fx='.5' fy='.5'" : "", tag, tag, tag);
		image = element(body);
		assert(image->shapes && image->shapes->fill.type == (radial ? NSVG_PAINT_RADIAL_GRADIENT : NSVG_PAINT_LINEAR_GRADIENT));
		assert(image->shapes->stroke.type == image->shapes->fill.type);
		fill = image->shapes->fill.gradient;
		stroke = image->shapes->stroke.gradient;
		assert(fill && stroke && fill != stroke && fill->nstops == 3 && stroke->nstops == 3);
		assert(fill->spread == spreadValues[i] && stroke->spread == spreadValues[i]);
		assert(fill->stops[0].offset == 0 && fill->stops[0].color == 0xff0000ffu);
		assert(fill->stops[1].offset == .5f && fill->stops[1].color == 0x7f00ff00u);
		assert(fill->stops[2].offset == 1 && fill->stops[2].color == 0xffff0000u);
		if (radial) assert(fill->fx == 0 && fill->fy == 0);
		fill->stops[0].color = 0;
		assert(stroke->stops[0].color == 0xff0000ffu);
		nsvgDelete(image);
	}
	{
		NSVGimage* image = element("<defs><linearGradient id='a' xlink:href='#b'/>"
			"<linearGradient id='b' xlink:href='#a'/></defs>"
			"<rect width='16' height='16' fill='url(#a)' stroke='url(#b)'/>");
		assert(image->shapes->fill.type == NSVG_PAINT_NONE && image->shapes->stroke.type == NSVG_PAINT_NONE);
		nsvgDelete(image);
	}
}

static void test_gradient_pixels(void)
{
	int radial, stops;
	for (radial = 0; radial < 2; radial++) for (stops = 1; stops <= 3; stops++) {
		char body[2048];
		unsigned char pixels[64*64*4];
		const char* tag = radial ? "radialGradient" : "linearGradient";
		NSVGimage* image;
		snprintf(body, sizeof(body), "<defs><%s id='g' gradientUnits='userSpaceOnUse' %s>"
			"<stop stop-color='red'/>%s%s</%s></defs><rect width='64' height='64' fill='url(#g)'/>",
			tag, radial ? "cx='32' cy='32' fx='32' fy='32' r='16'" : "x1='16' y1='0' x2='48' y2='0'",
			stops == 3 ? "<stop offset='.5' stop-color='#00ff00'/>" : "",
			stops > 1 ? "<stop offset='1' stop-color='blue'/>" : "", tag);
		image = element(body);
		render(image, pixels);
		rgba(pixels, radial ? 32 : 16, 32, 255, 0, 0, 255, 1);
		rgba(pixels, 56, 32, stops == 1 ? 255 : 0, 0, stops == 1 ? 0 : 255, 255, 1);
		if (stops == 2) rgba(pixels, radial ? 40 : 32, 32, 128, 0, 127, 255, 2);
		if (stops == 3) rgba(pixels, radial ? 40 : 32, 32, 0, 255, 0, 255, 2);
		nsvgDelete(image);
	}
	{
		unsigned char pixels[64*64*4];
		NSVGimage* image = element("<defs><linearGradient id='g' gradientUnits='userSpaceOnUse' x1='0' x2='64'>"
			"<stop offset='.25' stop-color='red'/><stop offset='.75' stop-color='blue'/></linearGradient></defs>"
			"<rect width='64' height='64' fill='url(#g)'/>");
		render(image, pixels);
		rgba(pixels, 0, 32, 255, 0, 0, 255, 0);
		rgba(pixels, 60, 32, 0, 0, 255, 255, 0);
		nsvgDelete(image);
	}
}

static void test_gradient_opacity_and_focus(void)
{
	int userSpace;
	for (userSpace = 0; userSpace < 2; userSpace++) {
		char body[2048];
		unsigned char pixels[64*64*4];
		NSVGimage* image;
		NSVGshape* shape;
		int stroke, n;
		snprintf(body, sizeof(body), "<g opacity='.5' fill='url(#g)' stroke='url(#g)' stroke-width='2'>"
			"<rect x='16' y='8' width='32' height='32' fill-opacity='.25' stroke-opacity='.5'/>"
			"<rect x='80' y='8' width='32' height='32' fill-opacity='.5' stroke-opacity='.25'/></g>"
			"<defs><radialGradient id='g' gradientUnits='%s' %s><stop stop-color='red' stop-opacity='.5'/>"
			"</radialGradient></defs>", userSpace ? "userSpaceOnUse" : "objectBoundingBox",
			userSpace ? "cx='32' cy='24' r='16' fx='40' fy='16'" : "cx='.5' cy='.5' r='.5' fx='.75' fy='.25'");
		image = element(body);
		shape = image->shapes;
		for (n = 0; n < 2; n++, shape = shape->next) {
			assert(shape && shape->opacity == .5f);
			for (stroke = 0; stroke < 2; stroke++) {
				NSVGpaint* paint = stroke ? &shape->stroke : &shape->fill;
				assert(paint->type == NSVG_PAINT_RADIAL_GRADIENT);
				near_value(paint->gradient->fx, .5f, 1e-5f);
				near_value(paint->gradient->fy, -.5f, 1e-5f);
				assert((paint->gradient->stops[0].color >> 24) == (n != stroke ? 63u : 31u));
			}
		}
		render(image, pixels);
		assert(pixels[(24*64+32)*4+3] == 15 && pixels[(24*64+15)*4+3] == 31);
		nsvgDelete(image);
	}
}

static void test_opacity_range(void)
{
	int kind, alpha;
	for (kind = 0; kind < 3; kind++) for (alpha = 0; alpha <= 255; alpha++) {
		char body[1024];
		unsigned char pixels[64*64*4];
		const char* tag = kind == 2 ? "radialGradient" : "linearGradient";
		const char* paint = kind ? "url(#g)" : "red";
		NSVGimage* image;
		snprintf(body, sizeof(body), "<defs><%s id='g'><stop stop-color='red'/></%s></defs>"
			"<rect x='8' y='8' width='40' height='40' fill='%s' stroke='%s' stroke-width='4' "
			"fill-opacity='%.9g' stroke-opacity='%.9g'/>", tag, tag, paint, paint, alpha/255.0, alpha/255.0);
		image = element(body);
		render(image, pixels);
		near_value(pixels[(24*64+24)*4+3], (float)alpha, 1);
		near_value(pixels[(24*64+7)*4+3], (float)alpha, 1);
		nsvgDelete(image);
	}
}

static void test_fill_rules(void)
{
	int evenodd, reverse;
	for (evenodd = 0; evenodd < 2; evenodd++) for (reverse = 0; reverse < 2; reverse++) {
		char body[512];
		unsigned char pixels[64*64*4];
		NSVGimage* image;
		snprintf(body, sizeof(body), "<path fill-rule='%s' d='M8 8H56V56H8Z %s'/>",
			evenodd ? "evenodd" : "nonzero", reverse ? "M20 20V44H44V20Z" : "M20 20H44V44H20Z");
		image = element(body);
		assert(image->shapes->fillRule == (evenodd ? NSVG_FILLRULE_EVENODD : NSVG_FILLRULE_NONZERO));
		render(image, pixels);
		assert(pixels[(12*64+12)*4+3] == 255);
		assert(pixels[(32*64+32)*4+3] == (evenodd || reverse ? 0 : 255));
		assert(pixels[3] == 0);
		nsvgDelete(image);
	}
}

static void test_caps_and_joins(void)
{
	const char* caps[] = {"butt", "round", "square"};
	const int capValues[] = {NSVG_CAP_BUTT, NSVG_CAP_ROUND, NSVG_CAP_SQUARE};
	const char* joins[] = {"miter", "round", "bevel"};
	const int joinValues[] = {NSVG_JOIN_MITER, NSVG_JOIN_ROUND, NSVG_JOIN_BEVEL};
	unsigned long areas[3] = {0, 0, 0};
	size_t i;
	for (i = 0; i < COUNT(caps); i++) {
		char body[512];
		unsigned char pixels[64*64*4];
		NSVGimage* image;
		snprintf(body, sizeof(body), "<path d='M16 32H48' fill='none' stroke='red' stroke-width='8' stroke-linecap='%s'/>", caps[i]);
		image = element(body);
		assert(image->shapes->strokeLineCap == capValues[i]);
		render(image, pixels);
		assert(pixels[(32*64+32)*4+3] == 255);
		assert(pixels[(32*64+13)*4+3] == (i == 0 ? 0 : 255));
		assert(pixels[(32*64+8)*4+3] == 0);
		if (i == 2) assert(pixels[(28*64+12)*4+3] == 255);
		if (i == 1) assert(pixels[(28*64+12)*4+3] < 128);
		nsvgDelete(image);
	}
	for (i = 0; i < COUNT(joins); i++) {
		char body[512];
		unsigned char pixels[64*64*4];
		NSVGimage* image;
		int j;
		snprintf(body, sizeof(body), "<path d='M16 48L32 16L48 48' fill='none' stroke='red' "
			"stroke-width='12' stroke-linejoin='%s' stroke-miterlimit='10'/>", joins[i]);
		image = element(body);
		assert(image->shapes->strokeLineJoin == joinValues[i]);
		render(image, pixels);
		for (j = 0; j < 64*64; j++) areas[i] += pixels[j*4+3];
		assert(pixels[(8*64+32)*4+3] == (i == 0 ? 255 : 0));
		if (i == 0) {
			image->shapes->miterLimit = 1;
			render(image, pixels);
			assert(pixels[(8*64+32)*4+3] == 0);
		}
		nsvgDelete(image);
	}
	assert(areas[0] > areas[1] && areas[1] > areas[2]);
}

static void test_paint_order_and_blending(void)
{
	const char* orders[] = {"normal", "fill stroke markers", "fill markers stroke", "markers fill stroke",
		"markers stroke fill", "stroke fill markers", "stroke markers fill"};
	size_t i;
	for (i = 0; i < COUNT(orders); i++) {
		char body[512];
		unsigned char pixels[64*64*4];
		NSVGimage* image;
		snprintf(body, sizeof(body), "<rect x='16' y='16' width='32' height='32' fill='red' "
			"stroke='blue' stroke-width='8' paint-order='%s'/>", orders[i]);
		image = element(body);
		render(image, pixels);
		rgba(pixels, 18, 32, i < 4 ? 0 : 255, 0, i < 4 ? 255 : 0, 255, 0);
		rgba(pixels, 14, 32, 0, 0, 255, 255, 0);
		rgba(pixels, 32, 32, 255, 0, 0, 255, 0);
		nsvgDelete(image);
	}
	{
		unsigned char pixels[64*64*4];
		NSVGimage* image = element("<rect width='64' height='64' fill='red'/>"
			"<rect x='8' y='8' width='32' height='32' fill='blue' fill-opacity='.5'/>");
		render(image, pixels);
		rgba(pixels, 16, 16, 128, 0, 127, 255, 1);
		rgba(pixels, 48, 48, 255, 0, 0, 255, 0);
		image->shapes->opacity = .5f;
		render(image, pixels);
		rgba(pixels, 16, 16, 85, 0, 170, 191, 2);
		nsvgDelete(image);
	}
}

static void test_raster_buffers(void)
{
	const int sizes[][2] = {{1,1}, {7,3}, {64,64}, {2,65}, {0,0}};
	NSVGrasterizer* r = nsvgCreateRasterizer();
	NSVGimage* image = element("<rect x='-8' y='-8' width='128' height='128' fill='red'/>");
	NSVGimage* empty = element("");
	size_t i;
	assert(r != NULL);
	for (i = 0; i < COUNT(sizes); i++) {
		int w = sizes[i][0], h = sizes[i][1], stride = w*4+7, x, y, j;
		size_t size = (size_t)stride*h+64;
		unsigned char* storage = (unsigned char*)malloc(size);
		unsigned char* pixels;
		assert(storage != NULL);
		memset(storage, 0xa5, size);
		pixels = storage+32;
		nsvgRasterize(r, image, 0, 0, 1, pixels, w, h, stride);
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				assert(pixels[y*stride+x*4] == 255 && pixels[y*stride+x*4+3] == 255);
				assert(pixels[y*stride+x*4+1] == 0 && pixels[y*stride+x*4+2] == 0);
			}
			for (x = w*4; x < stride; x++) assert(pixels[y*stride+x] == 0xa5);
		}
		nsvgRasterize(r, empty, 0, 0, 1, pixels, w, h, stride);
		for (y = 0; y < h; y++) {
			for (x = 0; x < w*4; x++) assert(pixels[y*stride+x] == 0);
			for (x = w*4; x < stride; x++) assert(pixels[y*stride+x] == 0xa5);
		}
		for (j = 0; j < 32; j++) assert(storage[j] == 0xa5 && storage[size-1-j] == 0xa5);
		free(storage);
	}
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
	nsvgDelete(empty);
}

static void test_raster_transform_and_reuse(void)
{
	const struct { float tx, ty, scale; int box[4]; } cases[] = {
		{0, 0, 1, {8,8,24,24}}, {4, 6, 2, {20,22,52,54}},
		{-20, -20, 2, {0,0,28,28}}, {0, 0, .5f, {4,4,12,12}}, {100, 100, 1, {0,0,0,0}}
	};
	NSVGimage* image = element("<rect x='8' y='8' width='16' height='16' fill='blue'/>");
	NSVGrasterizer* r = nsvgCreateRasterizer();
	NSVGpath* path = image->shapes->paths;
	float* original = (float*)malloc(path->npts*2*sizeof(float));
	size_t i;
	assert(r && original);
	memcpy(original, path->pts, path->npts*2*sizeof(float));
	for (i = 0; i < COUNT(cases); i++) {
		unsigned char a[64*64*4], b[64*64*4];
		int x, y;
		nsvgRasterize(r, image, cases[i].tx, cases[i].ty, cases[i].scale, a, 64, 64, 64*4);
		nsvgRasterize(r, image, cases[i].tx, cases[i].ty, cases[i].scale, b, 64, 64, 64*4);
		assert(memcmp(a, b, sizeof(a)) == 0);
		for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) {
			int inside = x >= cases[i].box[0] && y >= cases[i].box[1] && x < cases[i].box[2] && y < cases[i].box[3];
			assert(a[(y*64+x)*4+3] == (inside ? 255 : 0));
			if (inside) rgba(a, x, y, 0, 0, 255, 255, 0);
		}
		assert(memcmp(original, path->pts, path->npts*2*sizeof(float)) == 0);
	}
	free(original);
	nsvgDeleteRasterizer(r);
	nsvgDelete(image);
}

static void test_antialias(void)
{
	unsigned char pixels[64*64*4];
	unsigned long sum = 0;
	int i;
	NSVGimage* image = element("<rect x='8.25' y='8.25' width='16' height='16' fill='red'/>");
	render(image, pixels);
	assert(pixels[(8*64+8)*4+3] > 0 && pixels[(8*64+8)*4+3] < 255);
	assert(pixels[(16*64+16)*4+3] == 255 && pixels[(26*64+26)*4+3] == 0);
	for (i = 0; i < 64*64; i++) sum += pixels[i*4+3];
	near_value((float)sum, 16*16*255, 2*255);
	nsvgDelete(image);
}

static void test_dashes_and_visibility(void)
{
	const char* patterns[] = {"4 4", "4", "0 4 4 0"};
	int i, offset;
	for (i = 0; i < 3; i++) for (offset = -8; offset <= 8; offset++) {
		char body[512];
		unsigned char pixels[64*64*4];
		NSVGimage* image;
		int x;
		snprintf(body, sizeof(body), "<path d='M0 16H64' fill='none' stroke='red' stroke-width='2' "
			"stroke-dasharray='%s' stroke-dashoffset='%d'/>", patterns[i], offset);
		image = element(body);
		render(image, pixels);
		for (x = 0; x < 64; x++) {
			int phase = (x+offset+16)%8;
			assert(pixels[(16*64+x)*4+3] == ((i == 2 ? phase >= 4 : phase < 4) ? 255 : 0));
		}
		nsvgDelete(image);
	}
	{
		const char* states[] = {"visibility='hidden'", "visibility='collapse'", "display='none'"};
		for (i = 0; i < 3; i++) {
			char body[1024];
			unsigned char pixels[64*64*4];
			NSVGimage* image;
			snprintf(body, sizeof(body), "<g %s><rect x='8' y='8' width='16' height='16'/>"
				"<rect x='32' y='8' width='16' height='16' visibility='visible' display='inline'/></g>"
				"<rect x='8' y='32' width='16' height='16'/>", states[i]);
			image = element(body);
			render(image, pixels);
			assert(image->shapes->flags == 0);
			assert(pixels[(16*64+16)*4+3] == 0);
			assert(pixels[(16*64+40)*4+3] == (i == 2 ? 0 : 255));
			assert(pixels[(40*64+16)*4+3] == 255);
			nsvgDelete(image);
		}
	}
}

static void test_files_and_ownership(void)
{
	char filename[4096];
	const char* svg = "<svg width='64' height='64'><path id='owned' fill='red' d='M8 8H32V32H8Z M40 40L48 48'/></svg>";
	NSVGimage *memory = parse(svg), *file;
	NSVGpath *source, *copy;
	FILE* fp;
	int n = snprintf(filename, sizeof(filename), "%s/input.svg", scratch);
	assert(n >= 0 && (size_t)n < sizeof(filename));
	fp = fopen(filename, "wb");
	assert(fp && fwrite(svg, 1, strlen(svg), fp) == strlen(svg));
	assert(fclose(fp) == 0);
	file = nsvgParseFromFile(filename, "px", 96);
	assert(file && file->shapes && strcmp(file->shapes->id, "owned") == 0);
	assert(file->width == memory->width && file->height == memory->height);
	same_paths(file, memory);
	source = file->shapes->paths;
	copy = nsvgDuplicatePath(source);
	assert(copy && copy != source && copy->pts != source->pts && copy->next == NULL);
	assert(copy->npts == source->npts && copy->closed == source->closed);
	assert(memcmp(copy->bounds, source->bounds, sizeof(copy->bounds)) == 0);
	assert(memcmp(copy->pts, source->pts, source->npts*2*sizeof(float)) == 0);
	copy->pts[0] += 10;
	assert(copy->pts[0] != source->pts[0]);
	nsvgDelete(file);
	assert(isfinite(copy->pts[copy->npts*2-1]));
	free(copy->pts);
	free(copy);
	nsvgDelete(memory);
	fp = fopen(filename, "wb");
	assert(fp && fclose(fp) == 0);
	file = nsvgParseFromFile(filename, "px", 96);
	assert(file && !file->shapes);
	nsvgDelete(file);
	assert(remove(filename) == 0);
	assert(nsvgParseFromFile(filename, "px", 96) == NULL);
}

static void test_malformed_and_limits(void)
{
	const char* cases[] = {"<", "<svg", "<svg><rect width='", "<svg><path d='M'/></svg>",
		"<svg width='64' height='64'><path d='L1 2M3 4L5'/></svg>",
		"<svg width='64' height='64'><path d='M1 2A3 4 0 0'/></svg>",
		"<svg><rect broken width='5' height='5'/></svg>",
		"<svg><unknown/><defs><rect width='5' height='5'/></defs></svg>",
		"<svg width='64' height='64'><path d='M1 2L3 4 garbage'/></svg>",
		"<svg><style>.a{class:b;}.b{class:a;}</style><rect class='a' width='8' height='8'/></svg>",
		"<svg><style>.a{;bad;fill:red;bad}</style><rect class='a' width='8' height='8'/></svg>"};
	size_t i;
	for (i = 0; i < COUNT(cases); i++) {
		unsigned char pixels[64*64*4];
		NSVGimage* image = parse(cases[i]);
		valid_paths(image);
		render(image, pixels);
		nsvgDelete(image);
	}
	{
		char svg[16384], id[1024];
		NSVGimage* image;
		NSVGshape* shape;
		int j, count = 0, used;
		memset(id, 'a', sizeof(id)-1);
		id[sizeof(id)-1] = 0;
		snprintf(svg, sizeof(svg), "<svg width='64' height='64'><rect id='%s' width='16' height='16'/></svg>", id);
		image = parse(svg);
		assert(image->shapes && strlen(image->shapes->id) > 0);
		assert(strlen(image->shapes->id) < sizeof(image->shapes->id));
		assert(strncmp(image->shapes->id, id, strlen(image->shapes->id)) == 0);
		nsvgDelete(image);
		used = snprintf(svg, sizeof(svg), "<svg width='64' height='64'>");
		for (j = 0; j < 128; j++) used += snprintf(svg+used, sizeof(svg)-(size_t)used,
			"<rect x='%d' y='%d' width='1' height='1'/>", j%32, j/32);
		assert((size_t)used+7 < sizeof(svg));
		strcpy(svg+used, "</svg>");
		image = parse(svg);
		for (shape = image->shapes; shape; shape = shape->next) count++;
		assert(count == 128);
		valid_paths(image);
		nsvgDelete(image);
	}
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s SCRATCH_DIRECTORY\n", argv[0]);
		return 2;
	}
	scratch = argv[1];
	RUN(test_defaults);
	RUN(test_primitives);
	RUN(test_path_commands);
	RUN(test_units);
	RUN(test_viewbox);
	RUN(test_percentages_without_viewbox);
	RUN(test_colors);
	RUN(test_styles_and_transforms);
	RUN(test_gradient_data);
	RUN(test_gradient_pixels);
	RUN(test_gradient_opacity_and_focus);
	RUN(test_opacity_range);
	RUN(test_fill_rules);
	RUN(test_caps_and_joins);
	RUN(test_paint_order_and_blending);
	RUN(test_raster_buffers);
	RUN(test_raster_transform_and_reuse);
	RUN(test_antialias);
	RUN(test_dashes_and_visibility);
	RUN(test_files_and_ownership);
	RUN(test_malformed_and_limits);
	puts("NanoSVG public API checks passed");
	return 0;
}
