#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

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
			(i == 1 || i == 2 ? NSVG_RGB(255, 0, 0) : NSVG_RGB(0, 0, 0)));
		if (i == 2)
			assert((image->shapes->stroke.color & 0xffffff) == NSVG_RGB(0, 0, 255));
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
				(i >= 6 ? NSVG_RGB(255, 0, 0) : NSVG_RGB(0, 0, 0)));
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
	float special[] = {NAN, INFINITY, -INFINITY, FLT_MAX, -FLT_MAX};
	float belowMax = nextafterf((float)INT_MAX, 0);
	float aboveMin = nextafterf((float)INT_MIN, 0);
	size_t i;
	NSVGimage* image;
	assert(nsvg__roundf_clamp(NAN) == 0);
	assert(nsvg__roundf_clamp(INFINITY) == INT_MAX);
	assert(nsvg__roundf_clamp(-INFINITY) == INT_MIN);
	assert(nsvg__roundf_clamp(FLT_MAX) == INT_MAX);
	assert(nsvg__roundf_clamp(-FLT_MAX) == INT_MIN);
	assert(nsvg__roundf_clamp((float)INT_MAX) == INT_MAX);
	assert(nsvg__roundf_clamp((float)INT_MIN) == INT_MIN);
	assert(nsvg__roundf_clamp(belowMax) == (int)belowMax);
	assert(nsvg__roundf_clamp(aboveMin) == (int)aboveMin);
	assert(nsvg__roundf_clamp(1.5f) == 2 && nsvg__roundf_clamp(-1.5f) == -2);
	assert(nsvg__iadd_sat(INT_MAX, 1) == INT_MAX);
	assert(nsvg__iadd_sat(INT_MIN, -1) == INT_MIN);
	assert(nsvg__iadd_sat(INT_MAX, INT_MAX) == INT_MAX);
	assert(nsvg__iadd_sat(INT_MIN, INT_MIN) == INT_MIN);
	assert(nsvg__iadd_sat(INT_MAX, INT_MIN) == -1);
	assert(nsvg__iadd_sat(10, -20) == -10);
	assert(nsvg__curveDivs(1, NSVG_PI, 0.25f) == 3);
	assert(nsvg__curveDivs(FLT_MAX, NSVG_PI, 0.25f) == 2);
	for (i = 0; i < sizeof(special)/sizeof(special[0]); i++) {
		NSVGparser* p = nsvg__createParser();
		float args[7] = {8, special[i], 0, 0, 1, 20, 20};
		float x = 0, y = 0;
		int j;
		assert(p != NULL);
		nsvg__moveTo(p, x, y);
		nsvg__pathArcTo(p, &x, &y, args, 0);
		assert(x == 20 && y == 20);
		for (j = 0; j < p->npts*2; j++) assert(isfinite(p->pts[j]));
		args[1] = 8;
		args[2] = special[i];
		args[5] = 30;
		nsvg__pathArcTo(p, &x, &y, args, 0);
		assert(x == 30 && y == 20);
		for (j = 0; j < p->npts*2; j++) assert(isfinite(p->pts[j]));
		args[5] = NAN;
		nsvg__pathArcTo(p, &x, &y, args, 0);
		assert(x == 30 && y == 20);
		nsvg__deleteParser(p);
		assert(nsvg__curveDivs(special[i], NSVG_PI, 0.25f) == 2);
		assert(nsvg__curveDivs(1, special[i], 0.25f) == 2);
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
	const float matrices[][6] = {{0,0,0,0,0,0}, {1,2,2,4,3,4}, {2,1,1,3,4,5}};
	const float identity[6] = {1,0,0,1,0,0};
	size_t i;
	for (i = 0; i < sizeof(matrices)/sizeof(matrices[0]); i++) {
		float input[6], output[6] = {42,42,42,42,42,42};
		int j;
		memcpy(input, matrices[i], sizeof(input));
		nsvg__xformInverse(output, input);
		assert(memcmp(input, matrices[i], sizeof(input)) == 0);
		if (i == 2) nsvg__xformMultiply(output, input);
		for (j = 0; j < 6; j++) assert(fabsf(output[j] - identity[j]) < 1e-6f);
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
	const struct { const char* text; float matrix[6]; } valid[] = {
		{"matrix(2 1 3 4 5 6)", {2,1,3,4,5,6}},
		{"translate(2)", {1,0,0,1,2,0}}, {"translate(2,3)", {1,0,0,1,2,3}},
		{"scale(2)", {2,0,0,2,0,0}}, {"scale(2,3)", {2,0,0,3,0,0}},
		{"rotate(90)", {0,1,-1,0,0,0}}, {"rotate(90,2,3)", {0,1,-1,0,5,1}},
		{"skewX(45)", {1,0,1,1,0,0}}, {"skewY(45)", {1,1,0,1,0,0}},
		{"translate (7,9) scale(2 3)", {2,0,0,3,7,9}},
		{"scale(.5,-2e-1)", {.5f,0,0,-.2f,0,0}}
	};
	size_t i;
	int j, mixed;
	for (i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
		for (mixed = 0; mixed < 2; mixed++) {
			char text[256];
			float actual[6];
			const float expected[][6] = {{1,0,0,1,0,0}, {2,0,0,3,7,9}};
			snprintf(text, sizeof(text), mixed ? "translate(7 9) %s scale(2 3)" : "%s", invalid[i]);
			nsvg__parseTransform(actual, text);
			for (j = 0; j < 6; j++) assert(fabsf(actual[j] - expected[mixed][j]) < 1e-6f);
		}
	}
	for (i = 0; i < sizeof(valid)/sizeof(valid[0]); i++) {
		float actual[6];
		nsvg__parseTransform(actual, valid[i].text);
		for (j = 0; j < 6; j++) assert(fabsf(actual[j] - valid[i].matrix[j]) < 1e-6f);
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

int main(void)
{
	test_css_recursion();
	test_css_bounds();
	test_dashes();
	test_numeric();
	test_inverse();
	test_transforms();
	puts("NanoSVG regression checks passed");
	return 0;
}
