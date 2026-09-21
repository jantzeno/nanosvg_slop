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

// Altered for NanoSVG 2: native C++23 ownership and double precision.
// Define NANOSVGRAST_IMPLEMENTATION in exactly one C++23 translation unit
// before the first NanoSVG include to compile its implementation.
// Define NANOSVG_IMPLEMENTATION there too, or compile the parser separately.
#ifndef NANOSVGRAST_HPP
#define NANOSVGRAST_HPP

#include "nanosvg.hpp"
#include <span>

namespace nanosvg {
namespace detail { struct RasterizerState; }

class Rasterizer {
public:
    Rasterizer();
    ~Rasterizer();
    Rasterizer(const Rasterizer&) = delete;
    Rasterizer& operator=(const Rasterizer&) = delete;
    Rasterizer(Rasterizer&&) noexcept;
    Rasterizer& operator=(Rasterizer&&) noexcept;

    // Writes straight-alpha RGBA, preserving row padding and image geometry.
    // Invalid arguments leave dst untouched. Allocation failures can leave
    // partial output; the rasterizer remains reusable. Not concurrently usable.
    [[nodiscard]] std::expected<void, Error> rasterize(
        const Image& image, std::span<unsigned char> dst,
        int width, int height, int stride,
        double tx = 0.0, double ty = 0.0, double scale = 1.0);

private:
    std::unique_ptr<detail::RasterizerState> state_;
};

[[nodiscard]] std::expected<std::unique_ptr<Rasterizer>, Error> create_rasterizer();
} // namespace nanosvg

#ifdef NANOSVGRAST_IMPLEMENTATION

#include <algorithm>
#include <cmath>
#include <cstring>
#include <climits>
#include <stdexcept>
#include <utility>

namespace nanosvg {
namespace detail {
using std::isfinite;
using std::isnan;
#define NSVG__SUBSAMPLES	5
#define NSVG__FIXSHIFT		10
#define NSVG__FIX			(1 << NSVG__FIXSHIFT)
#define NSVG__FIXMASK		(NSVG__FIX-1)

typedef struct Edge {
	double x0,y0, x1,y1;
	int dir;
	struct Edge* next;
} Edge;

typedef struct StrokePoint {
	double x, y;
	double dx, dy;
	double len;
	double dmx, dmy;
	unsigned char flags;
} StrokePoint;

typedef struct ActiveEdge {
	int x,dx;
	double ey;
	int dir;
	struct ActiveEdge *next;
} ActiveEdge;


enum class PaintKind { none, color, linear, radial };

typedef struct CachedPaint {
	PaintKind type;
	Spread spread;
	double xform[6];
	unsigned int colors[256];
} CachedPaint;

struct RasterizerState {
    double tessTol = 0.25, distTol = 0.01;
    std::vector<Edge> edges;
    std::vector<StrokePoint> points, points2;
    std::vector<ActiveEdge> activeEdges;
    ActiveEdge* freelist = nullptr; // Borrows from activeEdges during a scan.
    std::vector<unsigned char> scanline;
    unsigned char* bitmap = nullptr; // Borrows the current call's destination.
    int width = 0, height = 0;
    std::size_t stride = 0;
};

static int nsvg__ptEquals(double x1, double y1, double x2, double y2, double tol)
{
	double dx = x2 - x1;
	double dy = y2 - y1;
	return dx*dx + dy*dy < tol*tol;
}

static void nsvg__addPathPoint(RasterizerState* r, double x, double y, int flags) {
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    if (!r->points.empty() && nsvg__ptEquals(r->points.back().x, r->points.back().y, x, y, r->distTol)) {
        r->points.back().flags |= static_cast<unsigned char>(flags);
        return;
    }
    if (r->points.size() == INT_MAX) throw std::length_error("too many points");
    StrokePoint point{};
    point.x = x; point.y = y; point.flags = static_cast<unsigned char>(flags);
    r->points.push_back(point);
}

static void nsvg__appendPathPoint(RasterizerState* r, StrokePoint point) {
    if (r->points.size() == INT_MAX) throw std::length_error("too many points");
    r->points.push_back(point);
}

static void nsvg__duplicatePoints(RasterizerState* r) { r->points2 = r->points; }

static void nsvg__addEdge(RasterizerState* r, double x0, double y0, double x1, double y1) {
    if (y0 == y1 || !std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return;
    if (r->edges.size() == INT_MAX) throw std::length_error("too many edges");
    if (y0 < y1) r->edges.push_back({x0, y0, x1, y1, 1, nullptr});
    else r->edges.push_back({x1, y1, x0, y0, -1, nullptr});
}

static double nsvg__normalize(double *x, double* y)
{
	double d = sqrt((*x)*(*x) + (*y)*(*y));
	if (d > 1e-6) {
		double id = 1.0 / d;
		*x *= id;
		*y *= id;
	}
	return d;
}

static double nsvg__absf(double x) { return x < 0 ? -x : x; }
static double nsvg__roundf(double x) { return (x >= 0) ? floor(x + 0.5) : ceil(x - 0.5); }

static int nsvg__roundf_clamp(double x)
{
	double rounded;
	if (!isfinite(x)) return x > 0 ? INT_MAX : (x < 0 ? INT_MIN : 0);
	rounded = nsvg__roundf(x);
	if ((double)rounded >= (double)INT_MAX) return INT_MAX;
	if ((double)rounded <= (double)INT_MIN) return INT_MIN;
	return (int)rounded;
}

static int nsvg__iadd_sat(int a, int b)
{
	if (b > 0 && a > INT_MAX - b) return INT_MAX;
	if (b < 0 && a < INT_MIN - b) return INT_MIN;
	return a + b;
}

static void nsvg__flattenCubicBez(RasterizerState* r,
								  double x1, double y1, double x2, double y2,
								  double x3, double y3, double x4, double y4,
								  int level, int type)
{
	double x12,y12,x23,y23,x34,y34,x123,y123,x234,y234,x1234,y1234;
	double dx,dy,d2,d3;

	if (level > 10) return;

	x12 = (x1+x2)*0.5;
	y12 = (y1+y2)*0.5;
	x23 = (x2+x3)*0.5;
	y23 = (y2+y3)*0.5;
	x34 = (x3+x4)*0.5;
	y34 = (y3+y4)*0.5;
	x123 = (x12+x23)*0.5;
	y123 = (y12+y23)*0.5;

	dx = x4 - x1;
	dy = y4 - y1;
	d2 = nsvg__absf((x2 - x4) * dy - (y2 - y4) * dx);
	d3 = nsvg__absf((x3 - x4) * dy - (y3 - y4) * dx);

	if ((d2 + d3)*(d2 + d3) < r->tessTol * (dx*dx + dy*dy)) {
		nsvg__addPathPoint(r, x4, y4, type);
		return;
	}

	x234 = (x23+x34)*0.5;
	y234 = (y23+y34)*0.5;
	x1234 = (x123+x234)*0.5;
	y1234 = (y123+y234)*0.5;

	nsvg__flattenCubicBez(r, x1,y1, x12,y12, x123,y123, x1234,y1234, level+1, 0);
	nsvg__flattenCubicBez(r, x1234,y1234, x234,y234, x34,y34, x4,y4, level+1, type);
}

static void nsvg__flattenShape(RasterizerState* r, const Shape* shape, double scale)
{
	int i, j;

	for (const auto& pathValue : shape->paths) {
        const auto* path = &pathValue;
        if (path->points.empty()) continue;
		r->points.clear();
		// Flatten path
		nsvg__addPathPoint(r, path->points.front().x*scale, path->points.front().y*scale, 0);
		for (i = 0; i < static_cast<int>(path->points.size())-1; i += 3) {
			const auto* p = &path->points[i];
			nsvg__flattenCubicBez(r, p[0].x*scale,p[0].y*scale, p[1].x*scale,p[1].y*scale, p[2].x*scale,p[2].y*scale, p[3].x*scale,p[3].y*scale, 0, 0);
		}
		// Close path
		nsvg__addPathPoint(r, path->points.front().x*scale, path->points.front().y*scale, 0);
		// Build edges
		for (i = 0, j = static_cast<int>(r->points.size())-1; i < static_cast<int>(r->points.size()); j = i++)
			nsvg__addEdge(r, r->points[j].x, r->points[j].y, r->points[i].x, r->points[i].y);
	}
}

enum NSVGpointFlags
{
	NSVG_PT_CORNER = 0x01,
	NSVG_PT_BEVEL = 0x02,
	NSVG_PT_LEFT = 0x04
};

static void nsvg__initClosed(StrokePoint* left, StrokePoint* right, StrokePoint* p0, StrokePoint* p1, double lineWidth)
{
	double w = lineWidth * 0.5;
	double dx = p1->x - p0->x;
	double dy = p1->y - p0->y;
	double len = nsvg__normalize(&dx, &dy);
	double px = p0->x + dx*len*0.5, py = p0->y + dy*len*0.5;
	double dlx = dy, dly = -dx;
	double lx = px - dlx*w, ly = py - dly*w;
	double rx = px + dlx*w, ry = py + dly*w;
	left->x = lx; left->y = ly;
	right->x = rx; right->y = ry;
}

static void nsvg__buttCap(RasterizerState* r, StrokePoint* left, StrokePoint* right, StrokePoint* p, double dx, double dy, double lineWidth, int connect)
{
	double w = lineWidth * 0.5;
	double px = p->x, py = p->y;
	double dlx = dy, dly = -dx;
	double lx = px - dlx*w, ly = py - dly*w;
	double rx = px + dlx*w, ry = py + dly*w;

	nsvg__addEdge(r, lx, ly, rx, ry);

	if (connect) {
		nsvg__addEdge(r, left->x, left->y, lx, ly);
		nsvg__addEdge(r, rx, ry, right->x, right->y);
	}
	left->x = lx; left->y = ly;
	right->x = rx; right->y = ry;
}

static void nsvg__squareCap(RasterizerState* r, StrokePoint* left, StrokePoint* right, StrokePoint* p, double dx, double dy, double lineWidth, int connect)
{
	double w = lineWidth * 0.5;
	double px = p->x - dx*w, py = p->y - dy*w;
	double dlx = dy, dly = -dx;
	double lx = px - dlx*w, ly = py - dly*w;
	double rx = px + dlx*w, ry = py + dly*w;

	nsvg__addEdge(r, lx, ly, rx, ry);

	if (connect) {
		nsvg__addEdge(r, left->x, left->y, lx, ly);
		nsvg__addEdge(r, rx, ry, right->x, right->y);
	}
	left->x = lx; left->y = ly;
	right->x = rx; right->y = ry;
}

#ifndef NSVG_PI
#define NSVG_PI (3.14159265358979323846264338327)
#endif

static void nsvg__roundCap(RasterizerState* r, StrokePoint* left, StrokePoint* right, StrokePoint* p, double dx, double dy, double lineWidth, int ncap, int connect)
{
	int i;
	double w = lineWidth * 0.5;
	double px = p->x, py = p->y;
	double dlx = dy, dly = -dx;
	double lx = 0, ly = 0, rx = 0, ry = 0, prevx = 0, prevy = 0;

	for (i = 0; i < ncap; i++) {
		double a = (double)i/(double)(ncap-1)*NSVG_PI;
		double ax = cos(a) * w, ay = sin(a) * w;
		double x = px - dlx*ax - dx*ay;
		double y = py - dly*ax - dy*ay;

		if (i > 0)
			nsvg__addEdge(r, prevx, prevy, x, y);

		prevx = x;
		prevy = y;

		if (i == 0) {
			lx = x; ly = y;
		} else if (i == ncap-1) {
			rx = x; ry = y;
		}
	}

	if (connect) {
		nsvg__addEdge(r, left->x, left->y, lx, ly);
		nsvg__addEdge(r, rx, ry, right->x, right->y);
	}

	left->x = lx; left->y = ly;
	right->x = rx; right->y = ry;
}

static void nsvg__bevelJoin(RasterizerState* r, StrokePoint* left, StrokePoint* right, StrokePoint* p0, StrokePoint* p1, double lineWidth)
{
	double w = lineWidth * 0.5;
	double dlx0 = p0->dy, dly0 = -p0->dx;
	double dlx1 = p1->dy, dly1 = -p1->dx;
	double lx0 = p1->x - (dlx0 * w), ly0 = p1->y - (dly0 * w);
	double rx0 = p1->x + (dlx0 * w), ry0 = p1->y + (dly0 * w);
	double lx1 = p1->x - (dlx1 * w), ly1 = p1->y - (dly1 * w);
	double rx1 = p1->x + (dlx1 * w), ry1 = p1->y + (dly1 * w);

	nsvg__addEdge(r, lx0, ly0, left->x, left->y);
	nsvg__addEdge(r, lx1, ly1, lx0, ly0);

	nsvg__addEdge(r, right->x, right->y, rx0, ry0);
	nsvg__addEdge(r, rx0, ry0, rx1, ry1);

	left->x = lx1; left->y = ly1;
	right->x = rx1; right->y = ry1;
}

static void nsvg__miterJoin(RasterizerState* r, StrokePoint* left, StrokePoint* right, StrokePoint* p0, StrokePoint* p1, double lineWidth)
{
	double w = lineWidth * 0.5;
	double dlx0 = p0->dy, dly0 = -p0->dx;
	double dlx1 = p1->dy, dly1 = -p1->dx;
	double lx0, rx0, lx1, rx1;
	double ly0, ry0, ly1, ry1;

	if (p1->flags & NSVG_PT_LEFT) {
		lx0 = lx1 = p1->x - p1->dmx * w;
		ly0 = ly1 = p1->y - p1->dmy * w;
		nsvg__addEdge(r, lx1, ly1, left->x, left->y);

		rx0 = p1->x + (dlx0 * w);
		ry0 = p1->y + (dly0 * w);
		rx1 = p1->x + (dlx1 * w);
		ry1 = p1->y + (dly1 * w);
		nsvg__addEdge(r, right->x, right->y, rx0, ry0);
		nsvg__addEdge(r, rx0, ry0, rx1, ry1);
	} else {
		lx0 = p1->x - (dlx0 * w);
		ly0 = p1->y - (dly0 * w);
		lx1 = p1->x - (dlx1 * w);
		ly1 = p1->y - (dly1 * w);
		nsvg__addEdge(r, lx0, ly0, left->x, left->y);
		nsvg__addEdge(r, lx1, ly1, lx0, ly0);

		rx0 = rx1 = p1->x + p1->dmx * w;
		ry0 = ry1 = p1->y + p1->dmy * w;
		nsvg__addEdge(r, right->x, right->y, rx1, ry1);
	}

	left->x = lx1; left->y = ly1;
	right->x = rx1; right->y = ry1;
}

static void nsvg__roundJoin(RasterizerState* r, StrokePoint* left, StrokePoint* right, StrokePoint* p0, StrokePoint* p1, double lineWidth, int ncap)
{
	int i, n;
	double w = lineWidth * 0.5;
	double dlx0 = p0->dy, dly0 = -p0->dx;
	double dlx1 = p1->dy, dly1 = -p1->dx;
	double a0 = atan2(dly0, dlx0);
	double a1 = atan2(dly1, dlx1);
	double da = a1 - a0;
	double lx, ly, rx, ry;

	if (da < NSVG_PI) da += NSVG_PI*2;
	if (da > NSVG_PI) da -= NSVG_PI*2;

	n = nsvg__roundf_clamp(ceil((nsvg__absf(da) / NSVG_PI) * (double)ncap));
	if (n < 2) n = 2;
	if (n > ncap) n = ncap;

	lx = left->x;
	ly = left->y;
	rx = right->x;
	ry = right->y;

	for (i = 0; i < n; i++) {
		double u = (double)i/(double)(n-1);
		double a = a0 + u*da;
		double ax = cos(a) * w, ay = sin(a) * w;
		double lx1 = p1->x - ax, ly1 = p1->y - ay;
		double rx1 = p1->x + ax, ry1 = p1->y + ay;

		nsvg__addEdge(r, lx1, ly1, lx, ly);
		nsvg__addEdge(r, rx, ry, rx1, ry1);

		lx = lx1; ly = ly1;
		rx = rx1; ry = ry1;
	}

	left->x = lx; left->y = ly;
	right->x = rx; right->y = ry;
}

static void nsvg__straightJoin(RasterizerState* r, StrokePoint* left, StrokePoint* right, StrokePoint* p1, double lineWidth)
{
	double w = lineWidth * 0.5;
	double lx = p1->x - (p1->dmx * w), ly = p1->y - (p1->dmy * w);
	double rx = p1->x + (p1->dmx * w), ry = p1->y + (p1->dmy * w);

	nsvg__addEdge(r, lx, ly, left->x, left->y);
	nsvg__addEdge(r, right->x, right->y, rx, ry);

	left->x = lx; left->y = ly;
	right->x = rx; right->y = ry;
}

static int nsvg__curveDivs(double r, double arc, double tol)
{
	double da = acos(r / (r + tol)) * 2.0;
	double divs;
	// Huge radii can round the ratio to one; use the minimum subdivision.
	if (!(da > 0.0)) return 2;
	divs = ceil(arc / da);
	if (!isfinite(divs) || (double)divs >= (double)INT_MAX) return 2;
	return divs < 2 ? 2 : (int)divs;
}

static void nsvg__expandStroke(RasterizerState* r, StrokePoint* points, int npoints, int closed, LineJoin lineJoin, LineCap lineCap, double lineWidth)
{
	int ncap = nsvg__curveDivs(lineWidth*0.5, NSVG_PI, r->tessTol);	// Calculate divisions per half circle.
	StrokePoint left = {0,0,0,0,0,0,0,0}, right = {0,0,0,0,0,0,0,0}, firstLeft = {0,0,0,0,0,0,0,0}, firstRight = {0,0,0,0,0,0,0,0};
	StrokePoint* p0, *p1;
	int j, s, e;

	// Build stroke edges
	if (closed) {
		// Looping
		p0 = &points[npoints-1];
		p1 = &points[0];
		s = 0;
		e = npoints;
	} else {
		// Add cap
		p0 = &points[0];
		p1 = &points[1];
		s = 1;
		e = npoints-1;
	}

	if (closed) {
		nsvg__initClosed(&left, &right, p0, p1, lineWidth);
		firstLeft = left;
		firstRight = right;
	} else {
		// Add cap
		double dx = p1->x - p0->x;
		double dy = p1->y - p0->y;
		nsvg__normalize(&dx, &dy);
		if (lineCap == LineCap::butt)
			nsvg__buttCap(r, &left, &right, p0, dx, dy, lineWidth, 0);
		else if (lineCap == LineCap::square)
			nsvg__squareCap(r, &left, &right, p0, dx, dy, lineWidth, 0);
		else if (lineCap == LineCap::round)
			nsvg__roundCap(r, &left, &right, p0, dx, dy, lineWidth, ncap, 0);
	}

	for (j = s; j < e; ++j) {
		if (p1->flags & NSVG_PT_CORNER) {
			if (lineJoin == LineJoin::round)
				nsvg__roundJoin(r, &left, &right, p0, p1, lineWidth, ncap);
			else if (lineJoin == LineJoin::bevel || (p1->flags & NSVG_PT_BEVEL))
				nsvg__bevelJoin(r, &left, &right, p0, p1, lineWidth);
			else
				nsvg__miterJoin(r, &left, &right, p0, p1, lineWidth);
		} else {
			nsvg__straightJoin(r, &left, &right, p1, lineWidth);
		}
		p0 = p1++;
	}

	if (closed) {
		// Loop it
		nsvg__addEdge(r, firstLeft.x, firstLeft.y, left.x, left.y);
		nsvg__addEdge(r, right.x, right.y, firstRight.x, firstRight.y);
	} else {
		// Add cap
		double dx = p1->x - p0->x;
		double dy = p1->y - p0->y;
		nsvg__normalize(&dx, &dy);
		if (lineCap == LineCap::butt)
			nsvg__buttCap(r, &right, &left, p1, -dx, -dy, lineWidth, 1);
		else if (lineCap == LineCap::square)
			nsvg__squareCap(r, &right, &left, p1, -dx, -dy, lineWidth, 1);
		else if (lineCap == LineCap::round)
			nsvg__roundCap(r, &right, &left, p1, -dx, -dy, lineWidth, ncap, 1);
	}
}

static void nsvg__prepareStroke(RasterizerState* r, double miterLimit, LineJoin lineJoin)
{
	int i, j;
	StrokePoint* p0, *p1;

	p0 = &r->points[static_cast<int>(r->points.size())-1];
	p1 = &r->points[0];
	for (i = 0; i < static_cast<int>(r->points.size()); i++) {
		// Calculate segment direction and length
		p0->dx = p1->x - p0->x;
		p0->dy = p1->y - p0->y;
		p0->len = nsvg__normalize(&p0->dx, &p0->dy);
		// Advance
		p0 = p1++;
	}

	// calculate joins
	p0 = &r->points[static_cast<int>(r->points.size())-1];
	p1 = &r->points[0];
	for (j = 0; j < static_cast<int>(r->points.size()); j++) {
		double dlx0, dly0, dlx1, dly1, dmr2, cross;
		dlx0 = p0->dy;
		dly0 = -p0->dx;
		dlx1 = p1->dy;
		dly1 = -p1->dx;
		// Calculate extrusions
		p1->dmx = (dlx0 + dlx1) * 0.5;
		p1->dmy = (dly0 + dly1) * 0.5;
		dmr2 = p1->dmx*p1->dmx + p1->dmy*p1->dmy;
		if (dmr2 > 0.000001) {
			double s2 = 1.0 / dmr2;
			if (s2 > 600.0) {
				s2 = 600.0;
			}
			p1->dmx *= s2;
			p1->dmy *= s2;
		}

		// Clear flags, but keep the corner.
		p1->flags = (p1->flags & NSVG_PT_CORNER) ? NSVG_PT_CORNER : 0;

		// Keep track of left turns.
		cross = p1->dx * p0->dy - p0->dx * p1->dy;
		if (cross > 0.0)
			p1->flags |= NSVG_PT_LEFT;

		// Check to see if the corner needs to be beveled.
		if (p1->flags & NSVG_PT_CORNER) {
			if ((dmr2 * miterLimit*miterLimit) < 1.0 || lineJoin == LineJoin::bevel || lineJoin == LineJoin::round) {
				p1->flags |= NSVG_PT_BEVEL;
			}
		}

		p0 = p1++;
	}
}

static void nsvg__flattenShapeStroke(RasterizerState* r, const Shape* shape, double scale)
{
	int i, j, closed;
	StrokePoint* p0, *p1;
	double miterLimit = shape->miterLimit;
	LineJoin lineJoin = shape->strokeLineJoin;
	LineCap lineCap = shape->strokeLineCap;
	double lineWidth = shape->strokeWidth * scale;
	// ponytail: limit dash work per shape; omit the remainder on exhaustion.
	// Clip paths before dashing if larger patterns need to be rendered in full.
	int dashBudget = 10000;

	for (const auto& pathValue : shape->paths) {
        const auto* path = &pathValue;
        if (path->points.empty()) continue;
		// Flatten path
		r->points.clear();
		nsvg__addPathPoint(r, path->points.front().x*scale, path->points.front().y*scale, NSVG_PT_CORNER);
		for (i = 0; i < static_cast<int>(path->points.size())-1; i += 3) {
			const auto* p = &path->points[i];
			nsvg__flattenCubicBez(r, p[0].x*scale,p[0].y*scale, p[1].x*scale,p[1].y*scale, p[2].x*scale,p[2].y*scale, p[3].x*scale,p[3].y*scale, 0, NSVG_PT_CORNER);
		}
		if (static_cast<int>(r->points.size()) < 2)
			continue;

		closed = path->closed;

		// If the first and last points are the same, remove the last, mark as closed path.
		p0 = &r->points[static_cast<int>(r->points.size())-1];
		p1 = &r->points[0];
		if (nsvg__ptEquals(p0->x,p0->y, p1->x,p1->y, r->distTol)) {
			r->points.pop_back();
			p0 = &r->points[static_cast<int>(r->points.size())-1];
			closed = 1;
		}

		if (static_cast<int>(shape->strokeDashArray.size()) > 0) {
			int idash = 0, dashState = 1;
			double totalDist = 0, dashLen, allDashLen, dashOffset;
			StrokePoint cur;

			if (closed)
				nsvg__appendPathPoint(r, r->points[0]);

			// Duplicate points -> points2.
			nsvg__duplicatePoints(r);

			r->points.clear();
			cur = r->points2[0];
			nsvg__appendPathPoint(r, cur);

			// Figure out dash offset.
			allDashLen = 0;
			for (j = 0; j < static_cast<int>(shape->strokeDashArray.size()); j++)
				allDashLen += shape->strokeDashArray[j];
			if (static_cast<int>(shape->strokeDashArray.size()) & 1)
				allDashLen *= 2.0;
			if (!(allDashLen > 0.0) || !isfinite(allDashLen))
				continue;
			// Find location inside pattern
			dashOffset = fmod(shape->strokeDashOffset, allDashLen);
			if (!isfinite(dashOffset))
				continue;
			if (dashOffset < 0.0)
				dashOffset += allDashLen;

			while (dashOffset > shape->strokeDashArray[idash]) {
				if (--dashBudget < 0) return;
				dashOffset -= shape->strokeDashArray[idash];
				idash = (idash + 1) % static_cast<int>(shape->strokeDashArray.size());
				dashState = !dashState;
			}
			dashLen = (shape->strokeDashArray[idash] - dashOffset) * scale;

			for (j = 1; j < static_cast<int>(r->points2.size()); ) {
				double dx = r->points2[j].x - cur.x;
				double dy = r->points2[j].y - cur.y;
				double dist = sqrt(dx*dx + dy*dy);
				if (--dashBudget < 0 || !isfinite(dist)) return;

				if ((totalDist + dist) > dashLen) {
					// Calculate intermediate point
					double d = (dashLen - totalDist) / dist;
					double x = cur.x + dx * d;
					double y = cur.y + dy * d;
					// Zero-length entries may toggle the pattern without moving.
					if (dashLen > totalDist && x == cur.x && y == cur.y) return;
					nsvg__addPathPoint(r, x, y, NSVG_PT_CORNER);

					// Stroke
					if (static_cast<int>(r->points.size()) > 1 && dashState) {
						nsvg__prepareStroke(r, miterLimit, lineJoin);
						nsvg__expandStroke(r, r->points.data(), static_cast<int>(r->points.size()), 0, lineJoin, lineCap, lineWidth);
					}
					// Advance dash pattern
					dashState = !dashState;
					idash = (idash+1) % static_cast<int>(shape->strokeDashArray.size());
					dashLen = shape->strokeDashArray[idash] * scale;
					// Restart
					cur.x = x;
					cur.y = y;
					cur.flags = NSVG_PT_CORNER;
					totalDist = 0.0;
					r->points.clear();
					nsvg__appendPathPoint(r, cur);
				} else {
					totalDist += dist;
					cur = r->points2[j];
					nsvg__appendPathPoint(r, cur);
					j++;
				}
			}
			// Stroke any leftover path
			if (static_cast<int>(r->points.size()) > 1 && dashState) {
				nsvg__prepareStroke(r, miterLimit, lineJoin);
				nsvg__expandStroke(r, r->points.data(), static_cast<int>(r->points.size()), 0, lineJoin, lineCap, lineWidth);
			}
		} else {
			nsvg__prepareStroke(r, miterLimit, lineJoin);
			nsvg__expandStroke(r, r->points.data(), static_cast<int>(r->points.size()), closed, lineJoin, lineCap, lineWidth);
		}
	}
}




static ActiveEdge* nsvg__addActive(RasterizerState* r, Edge* e, double startPoint)
{
	 ActiveEdge* z;

	if (r->freelist != NULL) {
		// Restore from freelist.
		z = r->freelist;
		r->freelist = z->next;
	} else {
		// Alloc new edge.
		r->activeEdges.emplace_back();
        z = &r->activeEdges.back();
	}

	double dxdy = (e->x1 - e->x0) / (e->y1 - e->y0);
//	STBTT_assert(e->y0 <= start_point);
	// round dx down to avoid going too far
	z->dx = nsvg__roundf_clamp(NSVG__FIX * dxdy);
	z->x = nsvg__roundf_clamp(NSVG__FIX * (e->x0 + dxdy * (startPoint - e->y0)));
//	z->x -= off_x * FIX;
	z->ey = e->y1;
	z->next = 0;
	z->dir = e->dir;

	return z;
}

static void nsvg__freeActive(RasterizerState* r, ActiveEdge* z)
{
	z->next = r->freelist;
	r->freelist = z;
}

static void nsvg__fillScanline(unsigned char* scanline, int len, int x0, int x1, int maxWeight, int* xmin, int* xmax)
{
	int i = x0 >> NSVG__FIXSHIFT;
	int j = x1 >> NSVG__FIXSHIFT;
	if (i < *xmin) *xmin = i;
	if (j > *xmax) *xmax = j;
	if (i < len && j >= 0) {
		if (i == j) {
			// x0,x1 are the same pixel, so compute combined coverage
			scanline[i] = (unsigned char)(scanline[i] + ((x1 - x0) * maxWeight >> NSVG__FIXSHIFT));
		} else {
			if (i >= 0) // add antialiasing for x0
				scanline[i] = (unsigned char)(scanline[i] + (((NSVG__FIX - (x0 & NSVG__FIXMASK)) * maxWeight) >> NSVG__FIXSHIFT));
			else
				i = -1; // clip

			if (j < len) // add antialiasing for x1
				scanline[j] = (unsigned char)(scanline[j] + (((x1 & NSVG__FIXMASK) * maxWeight) >> NSVG__FIXSHIFT));
			else
				j = len; // clip

			for (++i; i < j; ++i) // fill pixels between x0 and x1
				scanline[i] = (unsigned char)(scanline[i] + maxWeight);
		}
	}
}

// note: this routine clips fills that extend off the edges... ideally this
// wouldn't happen, but it could happen if the truetype glyph bounding boxes
// are wrong, or if the user supplies a too-small bitmap
static void nsvg__fillActiveEdges(unsigned char* scanline, int len, ActiveEdge* e, int maxWeight, int* xmin, int* xmax, FillRule fillRule)
{
	// non-zero winding fill
	int x0 = 0, w = 0;

	if (fillRule == FillRule::nonzero) {
		// Non-zero
		while (e != NULL) {
			if (w == 0) {
				// if we're currently at zero, we need to record the edge start point
				x0 = e->x; w += e->dir;
			} else {
				int x1 = e->x; w += e->dir;
				// if we went to zero, we need to draw
				if (w == 0)
					nsvg__fillScanline(scanline, len, x0, x1, maxWeight, xmin, xmax);
			}
			e = e->next;
		}
	} else if (fillRule == FillRule::evenodd) {
		// Even-odd
		while (e != NULL) {
			if (w == 0) {
				// if we're currently at zero, we need to record the edge start point
				x0 = e->x; w = 1;
			} else {
				int x1 = e->x; w = 0;
				nsvg__fillScanline(scanline, len, x0, x1, maxWeight, xmin, xmax);
			}
			e = e->next;
		}
	}
}

static double nsvg__clampf(double a, double mn, double mx) {
	if (isnan(a))
		return mn;
	return a < mn ? mn : (a > mx ? mx : a);
}

static unsigned int nsvg__RGBA(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	return ((unsigned int)r) | ((unsigned int)g << 8) | ((unsigned int)b << 16) | ((unsigned int)a << 24);
}

static unsigned int nsvg__lerpRGBA(unsigned int c0, unsigned int c1, double u)
{
	int iu = (int)(nsvg__clampf(u, 0.0, 1.0) * 256.0);
	int r = (((c0) & 0xff)*(256-iu) + (((c1) & 0xff)*iu)) >> 8;
	int g = (((c0>>8) & 0xff)*(256-iu) + (((c1>>8) & 0xff)*iu)) >> 8;
	int b = (((c0>>16) & 0xff)*(256-iu) + (((c1>>16) & 0xff)*iu)) >> 8;
	int a = (((c0>>24) & 0xff)*(256-iu) + (((c1>>24) & 0xff)*iu)) >> 8;
	return nsvg__RGBA((unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a);
}

static unsigned int nsvg__applyOpacity(unsigned int c, double u)
{
	int iu = (int)(nsvg__clampf(u, 0.0, 1.0) * 256.0);
	int r = (c) & 0xff;
	int g = (c>>8) & 0xff;
	int b = (c>>16) & 0xff;
	int a = (((c>>24) & 0xff)*iu) >> 8;
	return nsvg__RGBA((unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a);
}

static inline int nsvg__div255(int x)
{
    return ((x+1) * 257) >> 16;
}

static void nsvg__scanlineSolid(unsigned char* dst, int count, unsigned char* cover, int x, int y,
								double tx, double ty, double scale, CachedPaint* cache)
{

	if (cache->type == PaintKind::color) {
		int i, cr, cg, cb, ca;
		cr = cache->colors[0] & 0xff;
		cg = (cache->colors[0] >> 8) & 0xff;
		cb = (cache->colors[0] >> 16) & 0xff;
		ca = (cache->colors[0] >> 24) & 0xff;

		for (i = 0; i < count; i++) {
			int r,g,b;
			int a = nsvg__div255((int)cover[0] * ca);
			int ia = 255 - a;
			// Premultiply
			r = nsvg__div255(cr * a);
			g = nsvg__div255(cg * a);
			b = nsvg__div255(cb * a);

			// Blend over
			r += nsvg__div255(ia * (int)dst[0]);
			g += nsvg__div255(ia * (int)dst[1]);
			b += nsvg__div255(ia * (int)dst[2]);
			a += nsvg__div255(ia * (int)dst[3]);

			dst[0] = (unsigned char)r;
			dst[1] = (unsigned char)g;
			dst[2] = (unsigned char)b;
			dst[3] = (unsigned char)a;

			cover++;
			dst += 4;
		}
	} else if (cache->type == PaintKind::linear) {
		// TODO: spread modes.
		// TODO: plenty of opportunities to optimize.
		double fx, fy, dx, gy;
		double* t = cache->xform;
		int i, cr, cg, cb, ca;
		unsigned int c;

		fx = ((double)x - tx) / scale;
		fy = ((double)y - ty) / scale;
		dx = 1.0 / scale;

		for (i = 0; i < count; i++) {
			int r,g,b,a,ia;
			gy = fx*t[1] + fy*t[3] + t[5];
			c = cache->colors[(int)nsvg__clampf(gy*255.0, 0, 255.0)];
			cr = (c) & 0xff;
			cg = (c >> 8) & 0xff;
			cb = (c >> 16) & 0xff;
			ca = (c >> 24) & 0xff;

			a = nsvg__div255((int)cover[0] * ca);
			ia = 255 - a;

			// Premultiply
			r = nsvg__div255(cr * a);
			g = nsvg__div255(cg * a);
			b = nsvg__div255(cb * a);

			// Blend over
			r += nsvg__div255(ia * (int)dst[0]);
			g += nsvg__div255(ia * (int)dst[1]);
			b += nsvg__div255(ia * (int)dst[2]);
			a += nsvg__div255(ia * (int)dst[3]);

			dst[0] = (unsigned char)r;
			dst[1] = (unsigned char)g;
			dst[2] = (unsigned char)b;
			dst[3] = (unsigned char)a;

			cover++;
			dst += 4;
			fx += dx;
		}
	} else if (cache->type == PaintKind::radial) {
		// TODO: spread modes.
		// TODO: plenty of opportunities to optimize.
		// TODO: focus (fx,fy)
		double fx, fy, dx, gx, gy, gd;
		double* t = cache->xform;
		int i, cr, cg, cb, ca;
		unsigned int c;

		fx = ((double)x - tx) / scale;
		fy = ((double)y - ty) / scale;
		dx = 1.0 / scale;

		for (i = 0; i < count; i++) {
			int r,g,b,a,ia;
			gx = fx*t[0] + fy*t[2] + t[4];
			gy = fx*t[1] + fy*t[3] + t[5];
			gd = sqrt(gx*gx + gy*gy);
			c = cache->colors[(int)nsvg__clampf(gd*255.0, 0, 255.0)];
			cr = (c) & 0xff;
			cg = (c >> 8) & 0xff;
			cb = (c >> 16) & 0xff;
			ca = (c >> 24) & 0xff;

			a = nsvg__div255((int)cover[0] * ca);
			ia = 255 - a;

			// Premultiply
			r = nsvg__div255(cr * a);
			g = nsvg__div255(cg * a);
			b = nsvg__div255(cb * a);

			// Blend over
			r += nsvg__div255(ia * (int)dst[0]);
			g += nsvg__div255(ia * (int)dst[1]);
			b += nsvg__div255(ia * (int)dst[2]);
			a += nsvg__div255(ia * (int)dst[3]);

			dst[0] = (unsigned char)r;
			dst[1] = (unsigned char)g;
			dst[2] = (unsigned char)b;
			dst[3] = (unsigned char)a;

			cover++;
			dst += 4;
			fx += dx;
		}
	}
}

static void nsvg__rasterizeSortedEdges(RasterizerState *r, double tx, double ty, double scale, CachedPaint* cache, FillRule fillRule)
{
	ActiveEdge *active = NULL;
	int y, s;
	int e = 0;
	int maxWeight = (255 / NSVG__SUBSAMPLES);  // weight per vertical scanline
	int xmin, xmax;

	for (y = 0; y < r->height; y++) {
		memset(r->scanline.data(), 0, r->width);
		xmin = r->width;
		xmax = 0;
		for (s = 0; s < NSVG__SUBSAMPLES; ++s) {
			// find center of pixel for this scanline
			double scany = ((double)y*NSVG__SUBSAMPLES + s) + 0.5;
			ActiveEdge **step = &active;

			// update all active edges;
			// remove all active edges that terminate before the center of this scanline
			while (*step) {
				ActiveEdge *z = *step;
				if (z->ey <= scany) {
					*step = z->next; // delete from list
//					NSVG__assert(z->valid);
					nsvg__freeActive(r, z);
				} else {
					z->x = nsvg__iadd_sat(z->x, z->dx); // advance to position for current scanline
					step = &((*step)->next); // advance through list
				}
			}

			// resort the list if needed
			for (;;) {
				int changed = 0;
				step = &active;
				while (*step && (*step)->next) {
					if ((*step)->x > (*step)->next->x) {
						ActiveEdge* t = *step;
						ActiveEdge* q = t->next;
						t->next = q->next;
						q->next = t;
						*step = q;
						changed = 1;
					}
					step = &(*step)->next;
				}
				if (!changed) break;
			}

			// insert all edges that start before the center of this scanline -- omit ones that also end on this scanline
			while (e < static_cast<int>(r->edges.size()) && r->edges[e].y0 <= scany) {
				if (r->edges[e].y1 > scany) {
					ActiveEdge* z = nsvg__addActive(r, &r->edges[e], scany);
					if (z == NULL) break;
					// find insertion point
					if (active == NULL) {
						active = z;
					} else if (z->x < active->x) {
						// insert at front
						z->next = active;
						active = z;
					} else {
						// find thing to insert AFTER
						ActiveEdge* p = active;
						while (p->next && p->next->x < z->x)
							p = p->next;
						// at this point, p->next->x is NOT < z->x
						z->next = p->next;
						p->next = z;
					}
				}
				e++;
			}

			// now process all active edges in non-zero fashion
			if (active != NULL)
				nsvg__fillActiveEdges(r->scanline.data(), r->width, active, maxWeight, &xmin, &xmax, fillRule);
		}
		// Blit
		if (xmin < 0) xmin = 0;
		if (xmax > r->width-1) xmax = r->width-1;
		if (xmin <= xmax) {
			nsvg__scanlineSolid(&r->bitmap[y * r->stride] + xmin*4, xmax-xmin+1, &r->scanline[xmin], xmin, y, tx,ty, scale, cache);
		}
	}

}

static void nsvg__unpremultiplyAlpha(unsigned char* image, int w, int h, std::ptrdiff_t stride)
{
	int x,y;

	// Unpremultiply
	for (y = 0; y < h; y++) {
		unsigned char *row = &image[y*stride];
		for (x = 0; x < w; x++) {
			int r = row[0], g = row[1], b = row[2], a = row[3];
			if (a != 0) {
				row[0] = (unsigned char)(r*255/a);
				row[1] = (unsigned char)(g*255/a);
				row[2] = (unsigned char)(b*255/a);
			}
			row += 4;
		}
	}

	// Defringe
	for (y = 0; y < h; y++) {
		unsigned char *row = &image[y*stride];
		for (x = 0; x < w; x++) {
			int r = 0, g = 0, b = 0, a = row[3], n = 0;
			if (a == 0) {
				if (x-1 > 0 && row[-1] != 0) {
					r += row[-4];
					g += row[-3];
					b += row[-2];
					n++;
				}
				if (x+1 < w && row[7] != 0) {
					r += row[4];
					g += row[5];
					b += row[6];
					n++;
				}
				if (y-1 > 0 && row[-stride+3] != 0) {
					r += row[-stride];
					g += row[-stride+1];
					b += row[-stride+2];
					n++;
				}
				if (y+1 < h && row[stride+3] != 0) {
					r += row[stride];
					g += row[stride+1];
					b += row[stride+2];
					n++;
				}
				if (n > 0) {
					row[0] = (unsigned char)(r/n);
					row[1] = (unsigned char)(g/n);
					row[2] = (unsigned char)(b/n);
				}
			}
			row += 4;
		}
	}
}


static void nsvg__initPaint(CachedPaint* cache, const Paint* paint, double opacity)
{
	int i, j;
	const Gradient* grad;

	cache->type = PaintKind::none;

	if (const auto* color = std::get_if<Color>(paint)) {
        cache->type = PaintKind::color;
		cache->colors[0] = nsvg__applyOpacity(*color, opacity);
		return;
	}

	grad = std::get_if<Gradient>(paint);
    if (!grad) return;
    cache->type = grad->kind == GradientKind::linear ? PaintKind::linear : PaintKind::radial;

	cache->spread = grad->spread;
	std::copy(grad->xform.begin(), grad->xform.end(), cache->xform);

	if (static_cast<int>(grad->stops.size()) == 0) {
		for (i = 0; i < 256; i++)
			cache->colors[i] = 0;
	} else if (static_cast<int>(grad->stops.size()) == 1) {
		unsigned int color = nsvg__applyOpacity(grad->stops[0].color, opacity);
		for (i = 0; i < 256; i++)
			cache->colors[i] = color;
	} else {
		unsigned int ca, cb = 0;
		double ua, ub, du, u;
		int ia, ib, count;

		ca = nsvg__applyOpacity(grad->stops[0].color, opacity);
		ua = nsvg__clampf(grad->stops[0].offset, 0, 1);
		ub = nsvg__clampf(grad->stops[static_cast<int>(grad->stops.size())-1].offset, ua, 1);
		ia = (int)(ua * 255.0);
		ib = (int)(ub * 255.0);
		for (i = 0; i < ia; i++) {
			cache->colors[i] = ca;
		}

		for (i = 0; i < static_cast<int>(grad->stops.size())-1; i++) {
			ca = nsvg__applyOpacity(grad->stops[i].color, opacity);
			cb = nsvg__applyOpacity(grad->stops[i+1].color, opacity);
			ua = nsvg__clampf(grad->stops[i].offset, 0, 1);
			ub = nsvg__clampf(grad->stops[i+1].offset, 0, 1);
			ia = (int)(ua * 255.0);
			ib = (int)(ub * 255.0);
			count = ib - ia;
			if (count <= 0) continue;
			u = 0;
			du = 1.0 / (double)count;
			for (j = 0; j < count; j++) {
				cache->colors[ia+j] = nsvg__lerpRGBA(ca,cb,u);
				u += du;
			}
		}

		for (i = ib; i < 256; i++)
			cache->colors[i] = cb;
	}

}
static void rasterize(RasterizerState* r, const Image& image, double tx, double ty, double scale,
                      unsigned char* dst, int w, int h, int stride) {
    r->scanline.resize(w);
    r->bitmap = dst;
    r->width = w; r->height = h; r->stride = static_cast<std::size_t>(stride);
    for (int y = 0; y < h; ++y) std::memset(dst + static_cast<std::size_t>(y)*stride, 0, static_cast<std::size_t>(w)*4);
    for (const auto& shape : image.shapes) {
        if (!shape.visible) continue;
        for (auto order : shape.paintOrder) {
            const Paint* paint = nullptr;
            FillRule rule = shape.fillRule;
            r->edges.clear();
            if (order == PaintOrder::fill && !std::holds_alternative<std::monostate>(shape.fill)) {
                paint = &shape.fill;
                nsvg__flattenShape(r, &shape, scale);
            } else if (order == PaintOrder::stroke && !std::holds_alternative<std::monostate>(shape.stroke) && shape.strokeWidth*scale > 0.01) {
                paint = &shape.stroke;
                rule = FillRule::nonzero;
                nsvg__flattenShapeStroke(r, &shape, scale);
            } else continue;
            for (auto& edge : r->edges) {
                edge.x0 += tx; edge.x1 += tx;
                edge.y0 = (ty + edge.y0)*NSVG__SUBSAMPLES;
                edge.y1 = (ty + edge.y1)*NSVG__SUBSAMPLES;
            }
            std::erase_if(r->edges, [](const auto& edge) {
                return !std::isfinite(edge.x0) || !std::isfinite(edge.x1) || !std::isfinite(edge.y0) || !std::isfinite(edge.y1);
            });
            std::sort(r->edges.begin(), r->edges.end(), [](const auto& a, const auto& b) { return a.y0 < b.y0; });
            r->activeEdges.clear();
            r->freelist = nullptr;
            // Active/free links borrow vector elements; never grow after linking.
            r->activeEdges.reserve(r->edges.size());
            CachedPaint cache{};
            nsvg__initPaint(&cache, paint, shape.opacity);
            nsvg__rasterizeSortedEdges(r, tx, ty, scale, &cache, rule);
        }
    }
    nsvg__unpremultiplyAlpha(dst, w, h, stride);
}


static bool valid_image(const Image& image) {
    for (const auto& shape : image.shapes) {
        if (shape.strokeLineCap < LineCap::butt || shape.strokeLineCap > LineCap::square ||
            shape.strokeLineJoin < LineJoin::miter || shape.strokeLineJoin > LineJoin::bevel ||
            shape.fillRule < FillRule::nonzero || shape.fillRule > FillRule::evenodd) return false;
        for (auto order : shape.paintOrder)
            if (order < PaintOrder::fill || order > PaintOrder::stroke) return false;
        if (!std::isfinite(shape.opacity) || !std::isfinite(shape.strokeWidth) ||
            !std::isfinite(shape.strokeDashOffset) || !std::isfinite(shape.miterLimit) ||
            shape.strokeDashArray.size() > INT_MAX) return false;
        for (double dash : shape.strokeDashArray) if (!std::isfinite(dash) || dash < 0) return false;
        for (const auto& path : shape.paths) {
            if (path.points.empty()) continue;
            if (path.points.size() < 4 || path.points.size() % 3 != 1 || path.points.size() > INT_MAX) return false;
            for (const auto& point : path.points)
                if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
        }
        for (const Paint* paint : {&shape.fill, &shape.stroke}) {
            if (paint->valueless_by_exception()) return false;
            if (const auto* grad = std::get_if<Gradient>(paint)) {
                if ((grad->kind != GradientKind::linear && grad->kind != GradientKind::radial) ||
                    grad->spread < Spread::pad || grad->spread > Spread::repeat) return false;
                if (grad->stops.size() > INT_MAX) return false;
                for (double value : grad->xform) if (!std::isfinite(value)) return false;
                for (const auto& stop : grad->stops) if (!std::isfinite(stop.offset)) return false;
            }
        }
    }
    return true;
}
} // namespace detail

Rasterizer::Rasterizer() : state_(std::make_unique<detail::RasterizerState>()) {}
Rasterizer::~Rasterizer() = default;
Rasterizer::Rasterizer(Rasterizer&&) noexcept = default;
Rasterizer& Rasterizer::operator=(Rasterizer&&) noexcept = default;

std::expected<std::unique_ptr<Rasterizer>, Error> create_rasterizer() {
    try { return std::make_unique<Rasterizer>(); }
    catch (const std::bad_alloc&) { return std::unexpected(Error::allocation_failure); }
}

std::expected<void, Error> Rasterizer::rasterize(const Image& image, std::span<unsigned char> dst,
    int width, int height, int stride, double tx, double ty, double scale) {
    const auto size = detail::raster_buffer_size(width, height, stride);
    if (!size) return std::unexpected(size.error());
    if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(scale) || scale <= 0 ||
        dst.size() < *size || !state_) return std::unexpected(Error::invalid_argument);
    if (*size == 0) return {};
    if (!detail::valid_image(image)) return std::unexpected(Error::invalid_argument);
    // Clear all borrowed call state even when a vector allocation throws.
    struct Reset {
        detail::RasterizerState& r;
        ~Reset() { r.bitmap = nullptr; r.width = r.height = 0; r.stride = 0; r.freelist = nullptr; }
    } reset{*state_};
    try {
        detail::rasterize(state_.get(), image, tx, ty, scale, dst.data(), width, height, stride);
        return {};
    } catch (const std::bad_alloc&) { return std::unexpected(Error::allocation_failure); }
      catch (const std::length_error&) { return std::unexpected(Error::size_overflow); }
}
} // namespace nanosvg

#endif // NANOSVGRAST_IMPLEMENTATION
#endif // NANOSVGRAST_HPP
