#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

int main(void)
{
	test_css_recursion();
	test_css_bounds();
	test_dashes();
	puts("NanoSVG regression checks passed");
	return 0;
}
