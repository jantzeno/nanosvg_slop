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

int main(void)
{
	test_css_recursion();
	test_css_bounds();
	puts("NanoSVG regression checks passed");
	return 0;
}
