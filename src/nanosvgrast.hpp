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
        double offsetX = 0.0, double offsetY = 0.0, double scale = 1.0);

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
	double startX,startY, endX,endY;
	int dir;
	struct Edge* next;
} Edge;

typedef struct StrokePoint {
	double posX, posY;
	double dirX, dirY;
	double len;
	double dmx, dmy;
	unsigned char flags;
} StrokePoint;

typedef struct ActiveEdge {
	int posX,stepX;
	double endY;
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

static int nsvg__ptEquals(double firstX, double firstY, double secondX, double secondY, double tol)
{
	double deltaX = secondX - firstX;
	double deltaY = secondY - firstY;
	return deltaX*deltaX + deltaY*deltaY < tol*tol;
}

static void nsvg__addPathPoint(RasterizerState* raster, double posX, double posY, int flags) {
    if (!std::isfinite(posX) || !std::isfinite(posY)) return;
    if (!raster->points.empty() && nsvg__ptEquals(raster->points.back().posX, raster->points.back().posY, posX, posY, raster->distTol)) {
        raster->points.back().flags |= static_cast<unsigned char>(flags);
        return;
    }
    if (raster->points.size() == INT_MAX) throw std::length_error("too many points");
    StrokePoint point{};
    point.posX = posX; point.posY = posY; point.flags = static_cast<unsigned char>(flags);
    raster->points.push_back(point);
}

static void nsvg__appendPathPoint(RasterizerState* raster, StrokePoint point) {
    if (raster->points.size() == INT_MAX) throw std::length_error("too many points");
    raster->points.push_back(point);
}

static void nsvg__duplicatePoints(RasterizerState* raster) { raster->points2 = raster->points; }

static void nsvg__addEdge(RasterizerState* raster, double startX, double startY, double endX, double endY) {
    if (startY == endY || !std::isfinite(startX) || !std::isfinite(startY) || !std::isfinite(endX) || !std::isfinite(endY)) return;
    if (raster->edges.size() == INT_MAX) throw std::length_error("too many edges");
    if (startY < endY) raster->edges.push_back({startX, startY, endX, endY, 1, nullptr});
    else raster->edges.push_back({endX, endY, startX, startY, -1, nullptr});
}

static double nsvg__normalize(double *dirX, double* dirY)
{
	double length = sqrt((*dirX)*(*dirX) + (*dirY)*(*dirY));
	if (length > 1e-6) {
		double invLength = 1.0 / length;
		*dirX *= invLength;
		*dirY *= invLength;
	}
	return length;
}

static double nsvg__absf(double value) { return value < 0 ? -value : value; }
static double nsvg__roundf(double value) { return (value >= 0) ? floor(value + 0.5) : ceil(value - 0.5); }

static int nsvg__roundf_clamp(double value)
{
	double rounded;
	if (!isfinite(value)) return value > 0 ? INT_MAX : (value < 0 ? INT_MIN : 0);
	rounded = nsvg__roundf(value);
	if ((double)rounded >= (double)INT_MAX) return INT_MAX;
	if ((double)rounded <= (double)INT_MIN) return INT_MIN;
	return (int)rounded;
}

static int nsvg__iadd_sat(int lhs, int rhs)
{
	if (rhs > 0 && lhs > INT_MAX - rhs) return INT_MAX;
	if (rhs < 0 && lhs < INT_MIN - rhs) return INT_MIN;
	return lhs + rhs;
}

static void nsvg__flattenCubicBez(RasterizerState* raster,
								  double startX, double startY, double controlX1, double controlY1,
								  double controlX2, double controlY2, double endX, double endY,
								  int level, int type)
{
	double x12,y12,x23,y23,x34,y34,x123,y123,x234,y234,x1234,y1234;
	double deltaX,deltaY,deviation1,deviation2;

	if (level > 10) return;

	x12 = (startX+controlX1)*0.5;
	y12 = (startY+controlY1)*0.5;
	x23 = (controlX1+controlX2)*0.5;
	y23 = (controlY1+controlY2)*0.5;
	x34 = (controlX2+endX)*0.5;
	y34 = (controlY2+endY)*0.5;
	x123 = (x12+x23)*0.5;
	y123 = (y12+y23)*0.5;

	deltaX = endX - startX;
	deltaY = endY - startY;
	deviation1 = nsvg__absf((controlX1 - endX) * deltaY - (controlY1 - endY) * deltaX);
	deviation2 = nsvg__absf((controlX2 - endX) * deltaY - (controlY2 - endY) * deltaX);

	if ((deviation1 + deviation2)*(deviation1 + deviation2) < raster->tessTol * (deltaX*deltaX + deltaY*deltaY)) {
		nsvg__addPathPoint(raster, endX, endY, type);
		return;
	}

	x234 = (x23+x34)*0.5;
	y234 = (y23+y34)*0.5;
	x1234 = (x123+x234)*0.5;
	y1234 = (y123+y234)*0.5;

	nsvg__flattenCubicBez(raster, startX,startY, x12,y12, x123,y123, x1234,y1234, level+1, 0);
	nsvg__flattenCubicBez(raster, x1234,y1234, x234,y234, x34,y34, endX,endY, level+1, type);
}

static void nsvg__flattenShape(RasterizerState* raster, const Shape* shape, double scale)
{
	int pointIndex, prevIndex;

	for (const auto& pathValue : shape->paths) {
        const auto* path = &pathValue;
        if (path->points.empty()) continue;
		raster->points.clear();
		// Flatten path
		nsvg__addPathPoint(raster, path->points.front().x*scale, path->points.front().y*scale, 0);
		for (pointIndex = 0; pointIndex < static_cast<int>(path->points.size())-1; pointIndex += 3) {
			const auto* curve = &path->points[pointIndex];
			nsvg__flattenCubicBez(raster, curve[0].x*scale,curve[0].y*scale, curve[1].x*scale,
				curve[1].y*scale, curve[2].x*scale,curve[2].y*scale, curve[3].x*scale,curve[3].y*scale, 0, 0);
		}
		// Close path
		nsvg__addPathPoint(raster, path->points.front().x*scale, path->points.front().y*scale, 0);
		// Build edges
		for (pointIndex = 0, prevIndex = static_cast<int>(raster->points.size())-1; pointIndex < static_cast<int>(raster->points.size()); prevIndex = pointIndex++)
			nsvg__addEdge(raster, raster->points[prevIndex].posX, raster->points[prevIndex].posY,
				raster->points[pointIndex].posX, raster->points[pointIndex].posY);
	}
}

enum NSVGpointFlags
{
	NSVG_PT_CORNER = 0x01,
	NSVG_PT_BEVEL = 0x02,
	NSVG_PT_LEFT = 0x04
};

static void nsvg__initClosed(StrokePoint* left, StrokePoint* right, StrokePoint* prevPoint, StrokePoint* point, double lineWidth)
{
	double halfWidth = lineWidth * 0.5;
	double dirX = point->posX - prevPoint->posX;
	double dirY = point->posY - prevPoint->posY;
	double len = nsvg__normalize(&dirX, &dirY);
	double midX = prevPoint->posX + dirX*len*0.5, midY = prevPoint->posY + dirY*len*0.5;
	double dlx = dirY, dly = -dirX;
	double leftX = midX - dlx*halfWidth, leftY = midY - dly*halfWidth;
	double rightX = midX + dlx*halfWidth, rightY = midY + dly*halfWidth;
	left->posX = leftX; left->posY = leftY;
	right->posX = rightX; right->posY = rightY;
}

static void nsvg__buttCap(RasterizerState* raster, StrokePoint* left, StrokePoint* right,
	StrokePoint* point, double dirX, double dirY, double lineWidth, int connect)
{
	double halfWidth = lineWidth * 0.5;
	double centerX = point->posX, centerY = point->posY;
	double dlx = dirY, dly = -dirX;
	double leftX = centerX - dlx*halfWidth, leftY = centerY - dly*halfWidth;
	double rightX = centerX + dlx*halfWidth, rightY = centerY + dly*halfWidth;

	nsvg__addEdge(raster, leftX, leftY, rightX, rightY);

	if (connect) {
		nsvg__addEdge(raster, left->posX, left->posY, leftX, leftY);
		nsvg__addEdge(raster, rightX, rightY, right->posX, right->posY);
	}
	left->posX = leftX; left->posY = leftY;
	right->posX = rightX; right->posY = rightY;
}

static void nsvg__squareCap(RasterizerState* raster, StrokePoint* left, StrokePoint* right,
	StrokePoint* point, double dirX, double dirY, double lineWidth, int connect)
{
	double halfWidth = lineWidth * 0.5;
	double centerX = point->posX - dirX*halfWidth, centerY = point->posY - dirY*halfWidth;
	double dlx = dirY, dly = -dirX;
	double leftX = centerX - dlx*halfWidth, leftY = centerY - dly*halfWidth;
	double rightX = centerX + dlx*halfWidth, rightY = centerY + dly*halfWidth;

	nsvg__addEdge(raster, leftX, leftY, rightX, rightY);

	if (connect) {
		nsvg__addEdge(raster, left->posX, left->posY, leftX, leftY);
		nsvg__addEdge(raster, rightX, rightY, right->posX, right->posY);
	}
	left->posX = leftX; left->posY = leftY;
	right->posX = rightX; right->posY = rightY;
}

#ifndef NSVG_PI
#define NSVG_PI (3.14159265358979323846264338327)
#endif

static void nsvg__roundCap(RasterizerState* raster, StrokePoint* left, StrokePoint* right,
	StrokePoint* point, double dirX, double dirY, double lineWidth, int ncap, int connect)
{
	int step;
	double halfWidth = lineWidth * 0.5;
	double centerX = point->posX, centerY = point->posY;
	double dlx = dirY, dly = -dirX;
	double leftX = 0, leftY = 0, rightX = 0, rightY = 0, prevx = 0, prevy = 0;

	for (step = 0; step < ncap; step++) {
		double angle = (double)step/(double)(ncap-1)*NSVG_PI;
		double offsetX = cos(angle) * halfWidth, offsetY = sin(angle) * halfWidth;
		double posX = centerX - dlx*offsetX - dirX*offsetY;
		double posY = centerY - dly*offsetX - dirY*offsetY;

		if (step > 0)
			nsvg__addEdge(raster, prevx, prevy, posX, posY);

		prevx = posX;
		prevy = posY;

		if (step == 0) {
			leftX = posX; leftY = posY;
		} else if (step == ncap-1) {
			rightX = posX; rightY = posY;
		}
	}

	if (connect) {
		nsvg__addEdge(raster, left->posX, left->posY, leftX, leftY);
		nsvg__addEdge(raster, rightX, rightY, right->posX, right->posY);
	}

	left->posX = leftX; left->posY = leftY;
	right->posX = rightX; right->posY = rightY;
}

static void nsvg__bevelJoin(RasterizerState* raster, StrokePoint* left, StrokePoint* right,
	StrokePoint* prevPoint, StrokePoint* point, double lineWidth)
{
	double halfWidth = lineWidth * 0.5;
	double dlx0 = prevPoint->dirY, dly0 = -prevPoint->dirX;
	double dlx1 = point->dirY, dly1 = -point->dirX;
	double lx0 = point->posX - (dlx0 * halfWidth), ly0 = point->posY - (dly0 * halfWidth);
	double rx0 = point->posX + (dlx0 * halfWidth), ry0 = point->posY + (dly0 * halfWidth);
	double lx1 = point->posX - (dlx1 * halfWidth), ly1 = point->posY - (dly1 * halfWidth);
	double rx1 = point->posX + (dlx1 * halfWidth), ry1 = point->posY + (dly1 * halfWidth);

	nsvg__addEdge(raster, lx0, ly0, left->posX, left->posY);
	nsvg__addEdge(raster, lx1, ly1, lx0, ly0);

	nsvg__addEdge(raster, right->posX, right->posY, rx0, ry0);
	nsvg__addEdge(raster, rx0, ry0, rx1, ry1);

	left->posX = lx1; left->posY = ly1;
	right->posX = rx1; right->posY = ry1;
}

static void nsvg__miterJoin(RasterizerState* raster, StrokePoint* left, StrokePoint* right,
	StrokePoint* prevPoint, StrokePoint* point, double lineWidth)
{
	double halfWidth = lineWidth * 0.5;
	double dlx0 = prevPoint->dirY, dly0 = -prevPoint->dirX;
	double dlx1 = point->dirY, dly1 = -point->dirX;
	double lx0, rx0, lx1, rx1;
	double ly0, ry0, ly1, ry1;

	if (point->flags & NSVG_PT_LEFT) {
		lx0 = lx1 = point->posX - point->dmx * halfWidth;
		ly0 = ly1 = point->posY - point->dmy * halfWidth;
		nsvg__addEdge(raster, lx1, ly1, left->posX, left->posY);

		rx0 = point->posX + (dlx0 * halfWidth);
		ry0 = point->posY + (dly0 * halfWidth);
		rx1 = point->posX + (dlx1 * halfWidth);
		ry1 = point->posY + (dly1 * halfWidth);
		nsvg__addEdge(raster, right->posX, right->posY, rx0, ry0);
		nsvg__addEdge(raster, rx0, ry0, rx1, ry1);
	} else {
		lx0 = point->posX - (dlx0 * halfWidth);
		ly0 = point->posY - (dly0 * halfWidth);
		lx1 = point->posX - (dlx1 * halfWidth);
		ly1 = point->posY - (dly1 * halfWidth);
		nsvg__addEdge(raster, lx0, ly0, left->posX, left->posY);
		nsvg__addEdge(raster, lx1, ly1, lx0, ly0);

		rx0 = rx1 = point->posX + point->dmx * halfWidth;
		ry0 = ry1 = point->posY + point->dmy * halfWidth;
		nsvg__addEdge(raster, right->posX, right->posY, rx1, ry1);
	}

	left->posX = lx1; left->posY = ly1;
	right->posX = rx1; right->posY = ry1;
}

static void nsvg__roundJoin(RasterizerState* raster, StrokePoint* left, StrokePoint* right,
	StrokePoint* prevPoint, StrokePoint* point, double lineWidth, int ncap)
{
	int step, steps;
	double halfWidth = lineWidth * 0.5;
	double dlx0 = prevPoint->dirY, dly0 = -prevPoint->dirX;
	double dlx1 = point->dirY, dly1 = -point->dirX;
	double startAngle = atan2(dly0, dlx0);
	double endAngle = atan2(dly1, dlx1);
	double sweepAngle = endAngle - startAngle;
	double leftX, leftY, rightX, rightY;

	if (sweepAngle < NSVG_PI) sweepAngle += NSVG_PI*2;
	if (sweepAngle > NSVG_PI) sweepAngle -= NSVG_PI*2;

	steps = nsvg__roundf_clamp(ceil((nsvg__absf(sweepAngle) / NSVG_PI) * (double)ncap));
	if (steps < 2) steps = 2;
	if (steps > ncap) steps = ncap;

	leftX = left->posX;
	leftY = left->posY;
	rightX = right->posX;
	rightY = right->posY;

	for (step = 0; step < steps; step++) {
		double ratio = (double)step/(double)(steps-1);
		double angle = startAngle + ratio*sweepAngle;
		double offsetX = cos(angle) * halfWidth, offsetY = sin(angle) * halfWidth;
		double lx1 = point->posX - offsetX, ly1 = point->posY - offsetY;
		double rx1 = point->posX + offsetX, ry1 = point->posY + offsetY;

		nsvg__addEdge(raster, lx1, ly1, leftX, leftY);
		nsvg__addEdge(raster, rightX, rightY, rx1, ry1);

		leftX = lx1; leftY = ly1;
		rightX = rx1; rightY = ry1;
	}

	left->posX = leftX; left->posY = leftY;
	right->posX = rightX; right->posY = rightY;
}

static void nsvg__straightJoin(RasterizerState* raster, StrokePoint* left, StrokePoint* right, StrokePoint* point, double lineWidth)
{
	double halfWidth = lineWidth * 0.5;
	double leftX = point->posX - (point->dmx * halfWidth), leftY = point->posY - (point->dmy * halfWidth);
	double rightX = point->posX + (point->dmx * halfWidth), rightY = point->posY + (point->dmy * halfWidth);

	nsvg__addEdge(raster, leftX, leftY, left->posX, left->posY);
	nsvg__addEdge(raster, right->posX, right->posY, rightX, rightY);

	left->posX = leftX; left->posY = leftY;
	right->posX = rightX; right->posY = rightY;
}

static int nsvg__curveDivs(double radius, double arc, double tol)
{
	double angleStep = acos(radius / (radius + tol)) * 2.0;
	double divs;
	// Huge radii can round the ratio to one; use the minimum subdivision.
	if (!(angleStep > 0.0)) return 2;
	divs = ceil(arc / angleStep);
	if (!isfinite(divs) || (double)divs >= (double)INT_MAX) return 2;
	return divs < 2 ? 2 : (int)divs;
}

static void nsvg__expandStroke(RasterizerState* raster, StrokePoint* points, int npoints, int closed,
	LineJoin lineJoin, LineCap lineCap, double lineWidth)
{
	int ncap = nsvg__curveDivs(lineWidth*0.5, NSVG_PI, raster->tessTol);	// Calculate divisions per half circle.
	StrokePoint left = {0,0,0,0,0,0,0,0}, right = {0,0,0,0,0,0,0,0}, firstLeft = {0,0,0,0,0,0,0,0}, firstRight = {0,0,0,0,0,0,0,0};
	StrokePoint* prevPoint, *point;
	int pointIndex, startIndex, endIndex;

	// Build stroke edges
	if (closed) {
		// Looping
		prevPoint = &points[npoints-1];
		point = &points[0];
		startIndex = 0;
		endIndex = npoints;
	} else {
		// Add cap
		prevPoint = &points[0];
		point = &points[1];
		startIndex = 1;
		endIndex = npoints-1;
	}

	if (closed) {
		nsvg__initClosed(&left, &right, prevPoint, point, lineWidth);
		firstLeft = left;
		firstRight = right;
	} else {
		// Add cap
		double dirX = point->posX - prevPoint->posX;
		double dirY = point->posY - prevPoint->posY;
		nsvg__normalize(&dirX, &dirY);
		if (lineCap == LineCap::butt)
			nsvg__buttCap(raster, &left, &right, prevPoint, dirX, dirY, lineWidth, 0);
		else if (lineCap == LineCap::square)
			nsvg__squareCap(raster, &left, &right, prevPoint, dirX, dirY, lineWidth, 0);
		else if (lineCap == LineCap::round)
			nsvg__roundCap(raster, &left, &right, prevPoint, dirX, dirY, lineWidth, ncap, 0);
	}

	for (pointIndex = startIndex; pointIndex < endIndex; ++pointIndex) {
		if (point->flags & NSVG_PT_CORNER) {
			if (lineJoin == LineJoin::round)
				nsvg__roundJoin(raster, &left, &right, prevPoint, point, lineWidth, ncap);
			else if (lineJoin == LineJoin::bevel || (point->flags & NSVG_PT_BEVEL))
				nsvg__bevelJoin(raster, &left, &right, prevPoint, point, lineWidth);
			else
				nsvg__miterJoin(raster, &left, &right, prevPoint, point, lineWidth);
		} else {
			nsvg__straightJoin(raster, &left, &right, point, lineWidth);
		}
		prevPoint = point++;
	}

	if (closed) {
		// Loop it
		nsvg__addEdge(raster, firstLeft.posX, firstLeft.posY, left.posX, left.posY);
		nsvg__addEdge(raster, right.posX, right.posY, firstRight.posX, firstRight.posY);
	} else {
		// Add cap
		double dirX = point->posX - prevPoint->posX;
		double dirY = point->posY - prevPoint->posY;
		nsvg__normalize(&dirX, &dirY);
		if (lineCap == LineCap::butt)
			nsvg__buttCap(raster, &right, &left, point, -dirX, -dirY, lineWidth, 1);
		else if (lineCap == LineCap::square)
			nsvg__squareCap(raster, &right, &left, point, -dirX, -dirY, lineWidth, 1);
		else if (lineCap == LineCap::round)
			nsvg__roundCap(raster, &right, &left, point, -dirX, -dirY, lineWidth, ncap, 1);
	}
}

static void nsvg__prepareStroke(RasterizerState* raster, double miterLimit, LineJoin lineJoin)
{
	int segment, joinIndex;
	StrokePoint* prevPoint, *point;

	prevPoint = &raster->points[static_cast<int>(raster->points.size())-1];
	point = &raster->points[0];
	for (segment = 0; segment < static_cast<int>(raster->points.size()); segment++) {
		// Calculate segment direction and length
		prevPoint->dirX = point->posX - prevPoint->posX;
		prevPoint->dirY = point->posY - prevPoint->posY;
		prevPoint->len = nsvg__normalize(&prevPoint->dirX, &prevPoint->dirY);
		// Advance
		prevPoint = point++;
	}

	// calculate joins
	prevPoint = &raster->points[static_cast<int>(raster->points.size())-1];
	point = &raster->points[0];
	for (joinIndex = 0; joinIndex < static_cast<int>(raster->points.size()); joinIndex++) {
		double dlx0, dly0, dlx1, dly1, dmr2, cross;
		dlx0 = prevPoint->dirY;
		dly0 = -prevPoint->dirX;
		dlx1 = point->dirY;
		dly1 = -point->dirX;
		// Calculate extrusions
		point->dmx = (dlx0 + dlx1) * 0.5;
		point->dmy = (dly0 + dly1) * 0.5;
		dmr2 = point->dmx*point->dmx + point->dmy*point->dmy;
		if (dmr2 > 0.000001) {
			double miterScale = 1.0 / dmr2;
			if (miterScale > 600.0) {
				miterScale = 600.0;
			}
			point->dmx *= miterScale;
			point->dmy *= miterScale;
		}

		// Clear flags, but keep the corner.
		point->flags = (point->flags & NSVG_PT_CORNER) ? NSVG_PT_CORNER : 0;

		// Keep track of left turns.
		cross = point->dirX * prevPoint->dirY - prevPoint->dirX * point->dirY;
		if (cross > 0.0)
			point->flags |= NSVG_PT_LEFT;

		// Check to see if the corner needs to be beveled.
		if (point->flags & NSVG_PT_CORNER) {
			if ((dmr2 * miterLimit*miterLimit) < 1.0 || lineJoin == LineJoin::bevel || lineJoin == LineJoin::round) {
				point->flags |= NSVG_PT_BEVEL;
			}
		}

		prevPoint = point++;
	}
}

static void nsvg__flattenShapeStroke(RasterizerState* raster, const Shape* shape, double scale)
{
	int pointIndex, dashPointIndex, closed;
	StrokePoint* prevPoint, *point;
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
		raster->points.clear();
		nsvg__addPathPoint(raster, path->points.front().x*scale, path->points.front().y*scale, NSVG_PT_CORNER);
		for (pointIndex = 0; pointIndex < static_cast<int>(path->points.size())-1; pointIndex += 3) {
			const auto* curve = &path->points[pointIndex];
			nsvg__flattenCubicBez(raster, curve[0].x*scale,curve[0].y*scale, curve[1].x*scale,
				curve[1].y*scale, curve[2].x*scale,curve[2].y*scale, curve[3].x*scale,curve[3].y*scale, 0, NSVG_PT_CORNER);
		}
		if (static_cast<int>(raster->points.size()) < 2)
			continue;

		closed = path->closed;

		// If the first and last points are the same, remove the last, mark as closed path.
		prevPoint = &raster->points[static_cast<int>(raster->points.size())-1];
		point = &raster->points[0];
		if (nsvg__ptEquals(prevPoint->posX,prevPoint->posY, point->posX,point->posY, raster->distTol)) {
			raster->points.pop_back();
			prevPoint = &raster->points[static_cast<int>(raster->points.size())-1];
			closed = 1;
		}

		if (static_cast<int>(shape->strokeDashArray.size()) > 0) {
			int idash = 0, dashState = 1;
			double totalDist = 0, dashLen, allDashLen, dashOffset;
			StrokePoint cur;

			if (closed)
				nsvg__appendPathPoint(raster, raster->points[0]);

			// Duplicate points -> points2.
			nsvg__duplicatePoints(raster);

			raster->points.clear();
			cur = raster->points2[0];
			nsvg__appendPathPoint(raster, cur);

			// Figure out dash offset.
			allDashLen = 0;
			for (dashPointIndex = 0; dashPointIndex < static_cast<int>(shape->strokeDashArray.size()); dashPointIndex++)
				allDashLen += shape->strokeDashArray[dashPointIndex];
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

			for (dashPointIndex = 1; dashPointIndex < static_cast<int>(raster->points2.size()); ) {
				double deltaX = raster->points2[dashPointIndex].posX - cur.posX;
				double deltaY = raster->points2[dashPointIndex].posY - cur.posY;
				double dist = sqrt(deltaX*deltaX + deltaY*deltaY);
				if (--dashBudget < 0 || !isfinite(dist)) return;

				if ((totalDist + dist) > dashLen) {
					// Calculate intermediate point
					double ratio = (dashLen - totalDist) / dist;
					double dashX = cur.posX + deltaX * ratio;
					double dashY = cur.posY + deltaY * ratio;
					// Zero-length entries may toggle the pattern without moving.
					if (dashLen > totalDist && dashX == cur.posX && dashY == cur.posY) return;
					nsvg__addPathPoint(raster, dashX, dashY, NSVG_PT_CORNER);

					// Stroke
					if (static_cast<int>(raster->points.size()) > 1 && dashState) {
						nsvg__prepareStroke(raster, miterLimit, lineJoin);
						nsvg__expandStroke(raster, raster->points.data(),
							static_cast<int>(raster->points.size()), 0, lineJoin, lineCap, lineWidth);
					}
					// Advance dash pattern
					dashState = !dashState;
					idash = (idash+1) % static_cast<int>(shape->strokeDashArray.size());
					dashLen = shape->strokeDashArray[idash] * scale;
					// Restart
					cur.posX = dashX;
					cur.posY = dashY;
					cur.flags = NSVG_PT_CORNER;
					totalDist = 0.0;
					raster->points.clear();
					nsvg__appendPathPoint(raster, cur);
				} else {
					totalDist += dist;
					cur = raster->points2[dashPointIndex];
					nsvg__appendPathPoint(raster, cur);
					dashPointIndex++;
				}
			}
			// Stroke any leftover path
			if (static_cast<int>(raster->points.size()) > 1 && dashState) {
				nsvg__prepareStroke(raster, miterLimit, lineJoin);
				nsvg__expandStroke(raster, raster->points.data(),
					static_cast<int>(raster->points.size()), 0, lineJoin, lineCap, lineWidth);
			}
		} else {
			nsvg__prepareStroke(raster, miterLimit, lineJoin);
			nsvg__expandStroke(raster, raster->points.data(), static_cast<int>(raster->points.size()),
				closed, lineJoin, lineCap, lineWidth);
		}
	}
}




static ActiveEdge* nsvg__addActive(RasterizerState* raster, Edge* edge, double startPoint)
{
	 ActiveEdge* activeEdge;

	if (raster->freelist != NULL) {
		// Restore from freelist.
		activeEdge = raster->freelist;
		raster->freelist = activeEdge->next;
	} else {
		// Alloc new edge.
		raster->activeEdges.emplace_back();
        activeEdge = &raster->activeEdges.back();
	}

	double dxdy = (edge->endX - edge->startX) / (edge->endY - edge->startY);
//	STBTT_assert(edge->startY <= start_point);
	// round stepX down to avoid going too far
	activeEdge->stepX = nsvg__roundf_clamp(NSVG__FIX * dxdy);
	activeEdge->posX = nsvg__roundf_clamp(NSVG__FIX * (edge->startX + dxdy * (startPoint - edge->startY)));
//	activeEdge->posX -= off_x * FIX;
	activeEdge->endY = edge->endY;
	activeEdge->next = 0;
	activeEdge->dir = edge->dir;

	return activeEdge;
}

static void nsvg__freeActive(RasterizerState* raster, ActiveEdge* edge)
{
	edge->next = raster->freelist;
	raster->freelist = edge;
}

static void nsvg__fillScanline(unsigned char* scanline, int len, int startX, int endX, int maxWeight, int* xmin, int* xmax)
{
	int startPixel = startX >> NSVG__FIXSHIFT;
	int endPixel = endX >> NSVG__FIXSHIFT;
	if (startPixel < *xmin) *xmin = startPixel;
	if (endPixel > *xmax) *xmax = endPixel;
	if (startPixel < len && endPixel >= 0) {
		if (startPixel == endPixel) {
			// startX,endX are the same pixel, so compute combined coverage
			scanline[startPixel] = (unsigned char)(scanline[startPixel] + ((endX - startX) * maxWeight >> NSVG__FIXSHIFT));
		} else {
			if (startPixel >= 0) // add antialiasing for startX
				scanline[startPixel] = (unsigned char)(scanline[startPixel] + (((NSVG__FIX - (startX & NSVG__FIXMASK)) * maxWeight) >> NSVG__FIXSHIFT));
			else
				startPixel = -1; // clip

			if (endPixel < len) // add antialiasing for endX
				scanline[endPixel] = (unsigned char)(scanline[endPixel] + (((endX & NSVG__FIXMASK) * maxWeight) >> NSVG__FIXSHIFT));
			else
				endPixel = len; // clip

			for (++startPixel; startPixel < endPixel; ++startPixel) // fill pixels between startX and endX
				scanline[startPixel] = (unsigned char)(scanline[startPixel] + maxWeight);
		}
	}
}

// note: this routine clips fills that extend off the edges... ideally this
// wouldn't happen, but it could happen if the truetype glyph bounding boxes
// are wrong, or if the user supplies a too-small bitmap
static void nsvg__fillActiveEdges(unsigned char* scanline, int len, ActiveEdge* edge, int maxWeight,
	int* xmin, int* xmax, FillRule fillRule)
{
	// non-zero winding fill
	int startX = 0, winding = 0;

	if (fillRule == FillRule::nonzero) {
		// Non-zero
		while (edge != NULL) {
			if (winding == 0) {
				// if we're currently at zero, we need to record the edge start point
				startX = edge->posX; winding += edge->dir;
			} else {
				int endX = edge->posX; winding += edge->dir;
				// if we went to zero, we need to draw
				if (winding == 0)
					nsvg__fillScanline(scanline, len, startX, endX, maxWeight, xmin, xmax);
			}
			edge = edge->next;
		}
	} else if (fillRule == FillRule::evenodd) {
		// Even-odd
		while (edge != NULL) {
			if (winding == 0) {
				// if we're currently at zero, we need to record the edge start point
				startX = edge->posX; winding = 1;
			} else {
				int endX = edge->posX; winding = 0;
				nsvg__fillScanline(scanline, len, startX, endX, maxWeight, xmin, xmax);
			}
			edge = edge->next;
		}
	}
}

static double nsvg__clampf(double value, double lower, double upper) {
	if (isnan(value))
		return lower;
	return value < lower ? lower : (value > upper ? upper : value);
}

static unsigned int nsvg__RGBA(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha)
{
	return ((unsigned int)red) | ((unsigned int)green << 8) | ((unsigned int)blue << 16) | ((unsigned int)alpha << 24);
}

static unsigned int nsvg__lerpRGBA(unsigned int startColor, unsigned int endColor, double ratio)
{
	int weight = (int)(nsvg__clampf(ratio, 0.0, 1.0) * 256.0);
	int red = (((startColor) & 0xff)*(256-weight) + (((endColor) & 0xff)*weight)) >> 8;
	int green = (((startColor>>8) & 0xff)*(256-weight) + (((endColor>>8) & 0xff)*weight)) >> 8;
	int blue = (((startColor>>16) & 0xff)*(256-weight) + (((endColor>>16) & 0xff)*weight)) >> 8;
	int alpha = (((startColor>>24) & 0xff)*(256-weight) + (((endColor>>24) & 0xff)*weight)) >> 8;
	return nsvg__RGBA((unsigned char)red, (unsigned char)green, (unsigned char)blue, (unsigned char)alpha);
}

static unsigned int nsvg__applyOpacity(unsigned int color, double opacity)
{
	int weight = (int)(nsvg__clampf(opacity, 0.0, 1.0) * 256.0);
	int red = (color) & 0xff;
	int green = (color>>8) & 0xff;
	int blue = (color>>16) & 0xff;
	int alpha = (((color>>24) & 0xff)*weight) >> 8;
	return nsvg__RGBA((unsigned char)red, (unsigned char)green, (unsigned char)blue, (unsigned char)alpha);
}

static inline int nsvg__div255(int value)
{
    return ((value+1) * 257) >> 16;
}

static void nsvg__scanlineSolid(unsigned char* dst, int count, unsigned char* cover, int posX, int posY,
								double offsetX, double offsetY, double scale, CachedPaint* cache)
{

	if (cache->type == PaintKind::color) {
		int pixelIndex, srcRed, srcGreen, srcBlue, srcAlpha;
		srcRed = cache->colors[0] & 0xff;
		srcGreen = (cache->colors[0] >> 8) & 0xff;
		srcBlue = (cache->colors[0] >> 16) & 0xff;
		srcAlpha = (cache->colors[0] >> 24) & 0xff;

		for (pixelIndex = 0; pixelIndex < count; pixelIndex++) {
			int red,green,blue;
			int alpha = nsvg__div255((int)cover[0] * srcAlpha);
			int invAlpha = 255 - alpha;
			// Premultiply
			red = nsvg__div255(srcRed * alpha);
			green = nsvg__div255(srcGreen * alpha);
			blue = nsvg__div255(srcBlue * alpha);

			// Blend over
			red += nsvg__div255(invAlpha * (int)dst[0]);
			green += nsvg__div255(invAlpha * (int)dst[1]);
			blue += nsvg__div255(invAlpha * (int)dst[2]);
			alpha += nsvg__div255(invAlpha * (int)dst[3]);

			dst[0] = (unsigned char)red;
			dst[1] = (unsigned char)green;
			dst[2] = (unsigned char)blue;
			dst[3] = (unsigned char)alpha;

			cover++;
			dst += 4;
		}
	} else if (cache->type == PaintKind::linear) {
		// TODO: spread modes.
		// TODO: plenty of opportunities to optimize.
		double sampleX, sampleY, sampleStep, gradientY;
		double* xform = cache->xform;
		int pixelIndex, srcRed, srcGreen, srcBlue, srcAlpha;
		unsigned int color;

		sampleX = ((double)posX - offsetX) / scale;
		sampleY = ((double)posY - offsetY) / scale;
		sampleStep = 1.0 / scale;

		for (pixelIndex = 0; pixelIndex < count; pixelIndex++) {
			int red,green,blue,alpha,invAlpha;
			gradientY = sampleX*xform[1] + sampleY*xform[3] + xform[5];
			color = cache->colors[(int)nsvg__clampf(gradientY*255.0, 0, 255.0)];
			srcRed = (color) & 0xff;
			srcGreen = (color >> 8) & 0xff;
			srcBlue = (color >> 16) & 0xff;
			srcAlpha = (color >> 24) & 0xff;

			alpha = nsvg__div255((int)cover[0] * srcAlpha);
			invAlpha = 255 - alpha;

			// Premultiply
			red = nsvg__div255(srcRed * alpha);
			green = nsvg__div255(srcGreen * alpha);
			blue = nsvg__div255(srcBlue * alpha);

			// Blend over
			red += nsvg__div255(invAlpha * (int)dst[0]);
			green += nsvg__div255(invAlpha * (int)dst[1]);
			blue += nsvg__div255(invAlpha * (int)dst[2]);
			alpha += nsvg__div255(invAlpha * (int)dst[3]);

			dst[0] = (unsigned char)red;
			dst[1] = (unsigned char)green;
			dst[2] = (unsigned char)blue;
			dst[3] = (unsigned char)alpha;

			cover++;
			dst += 4;
			sampleX += sampleStep;
		}
	} else if (cache->type == PaintKind::radial) {
		// TODO: spread modes.
		// TODO: plenty of opportunities to optimize.
		// TODO: focus (fx,fy)
		double sampleX, sampleY, sampleStep, gradientX, gradientY, distance;
		double* xform = cache->xform;
		int pixelIndex, srcRed, srcGreen, srcBlue, srcAlpha;
		unsigned int color;

		sampleX = ((double)posX - offsetX) / scale;
		sampleY = ((double)posY - offsetY) / scale;
		sampleStep = 1.0 / scale;

		for (pixelIndex = 0; pixelIndex < count; pixelIndex++) {
			int red,green,blue,alpha,invAlpha;
			gradientX = sampleX*xform[0] + sampleY*xform[2] + xform[4];
			gradientY = sampleX*xform[1] + sampleY*xform[3] + xform[5];
			distance = sqrt(gradientX*gradientX + gradientY*gradientY);
			color = cache->colors[(int)nsvg__clampf(distance*255.0, 0, 255.0)];
			srcRed = (color) & 0xff;
			srcGreen = (color >> 8) & 0xff;
			srcBlue = (color >> 16) & 0xff;
			srcAlpha = (color >> 24) & 0xff;

			alpha = nsvg__div255((int)cover[0] * srcAlpha);
			invAlpha = 255 - alpha;

			// Premultiply
			red = nsvg__div255(srcRed * alpha);
			green = nsvg__div255(srcGreen * alpha);
			blue = nsvg__div255(srcBlue * alpha);

			// Blend over
			red += nsvg__div255(invAlpha * (int)dst[0]);
			green += nsvg__div255(invAlpha * (int)dst[1]);
			blue += nsvg__div255(invAlpha * (int)dst[2]);
			alpha += nsvg__div255(invAlpha * (int)dst[3]);

			dst[0] = (unsigned char)red;
			dst[1] = (unsigned char)green;
			dst[2] = (unsigned char)blue;
			dst[3] = (unsigned char)alpha;

			cover++;
			dst += 4;
			sampleX += sampleStep;
		}
	}
}

static void nsvg__rasterizeSortedEdges(RasterizerState *raster, double offsetX, double offsetY,
	double scale, CachedPaint* cache, FillRule fillRule)
{
	ActiveEdge *active = NULL;
	int rowIndex, sampleIndex;
	int edgeIndex = 0;
	int maxWeight = (255 / NSVG__SUBSAMPLES);  // weight per vertical scanline
	int xmin, xmax;

	for (rowIndex = 0; rowIndex < raster->height; rowIndex++) {
		memset(raster->scanline.data(), 0, raster->width);
		xmin = raster->width;
		xmax = 0;
		for (sampleIndex = 0; sampleIndex < NSVG__SUBSAMPLES; ++sampleIndex) {
			// find center of pixel for this scanline
			double scany = ((double)rowIndex*NSVG__SUBSAMPLES + sampleIndex) + 0.5;
			ActiveEdge **step = &active;

			// update all active edges;
			// remove all active edges that terminate before the center of this scanline
			while (*step) {
				ActiveEdge *edge = *step;
				if (edge->endY <= scany) {
					*step = edge->next; // delete from list
//					NSVG__assert(edge->valid);
					nsvg__freeActive(raster, edge);
				} else {
					edge->posX = nsvg__iadd_sat(edge->posX, edge->stepX); // advance to position for current scanline
					step = &((*step)->next); // advance through list
				}
			}

			// resort the list if needed
			for (;;) {
				int changed = 0;
				step = &active;
				while (*step && (*step)->next) {
					if ((*step)->posX > (*step)->next->posX) {
						ActiveEdge* current = *step;
						ActiveEdge* next = current->next;
						current->next = next->next;
						next->next = current;
						*step = next;
						changed = 1;
					}
					step = &(*step)->next;
				}
				if (!changed) break;
			}

			// insert all edges that start before the center of this scanline -- omit ones that also end on this scanline
			while (edgeIndex < static_cast<int>(raster->edges.size()) && raster->edges[edgeIndex].startY <= scany) {
				if (raster->edges[edgeIndex].endY > scany) {
					ActiveEdge* edge = nsvg__addActive(raster, &raster->edges[edgeIndex], scany);
					if (edge == NULL) break;
					// find insertion point
					if (active == NULL) {
						active = edge;
					} else if (edge->posX < active->posX) {
						// insert at front
						edge->next = active;
						active = edge;
					} else {
						// find thing to insert AFTER
						ActiveEdge* previous = active;
						while (previous->next && previous->next->posX < edge->posX)
							previous = previous->next;
						// at this point, previous->next->posX is NOT < edge->posX
						edge->next = previous->next;
						previous->next = edge;
					}
				}
				edgeIndex++;
			}

			// now process all active edges in non-zero fashion
			if (active != NULL)
				nsvg__fillActiveEdges(raster->scanline.data(), raster->width, active, maxWeight, &xmin, &xmax, fillRule);
		}
		// Blit
		if (xmin < 0) xmin = 0;
		if (xmax > raster->width-1) xmax = raster->width-1;
		if (xmin <= xmax) {
			nsvg__scanlineSolid(&raster->bitmap[rowIndex * raster->stride] + xmin*4, xmax-xmin+1,
				&raster->scanline[xmin], xmin, rowIndex, offsetX,offsetY, scale, cache);
		}
	}

}

static void nsvg__unpremultiplyAlpha(unsigned char* image, int width, int height, std::ptrdiff_t stride)
{
	int column,rowIndex;

	// Unpremultiply
	for (rowIndex = 0; rowIndex < height; rowIndex++) {
		unsigned char *row = &image[rowIndex*stride];
		for (column = 0; column < width; column++) {
			int red = row[0], green = row[1], blue = row[2], alpha = row[3];
			if (alpha != 0) {
				row[0] = (unsigned char)(red*255/alpha);
				row[1] = (unsigned char)(green*255/alpha);
				row[2] = (unsigned char)(blue*255/alpha);
			}
			row += 4;
		}
	}

	// Defringe
	for (rowIndex = 0; rowIndex < height; rowIndex++) {
		unsigned char *row = &image[rowIndex*stride];
		for (column = 0; column < width; column++) {
			int red = 0, green = 0, blue = 0, alpha = row[3], neighbors = 0;
			if (alpha == 0) {
				if (column-1 > 0 && row[-1] != 0) {
					red += row[-4];
					green += row[-3];
					blue += row[-2];
					neighbors++;
				}
				if (column+1 < width && row[7] != 0) {
					red += row[4];
					green += row[5];
					blue += row[6];
					neighbors++;
				}
				if (rowIndex-1 > 0 && row[-stride+3] != 0) {
					red += row[-stride];
					green += row[-stride+1];
					blue += row[-stride+2];
					neighbors++;
				}
				if (rowIndex+1 < height && row[stride+3] != 0) {
					red += row[stride];
					green += row[stride+1];
					blue += row[stride+2];
					neighbors++;
				}
				if (neighbors > 0) {
					row[0] = (unsigned char)(red/neighbors);
					row[1] = (unsigned char)(green/neighbors);
					row[2] = (unsigned char)(blue/neighbors);
				}
			}
			row += 4;
		}
	}
}


static void nsvg__initPaint(CachedPaint* cache, const Paint* paint, double opacity)
{
	int stopIndex, colorIndex;
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
		for (stopIndex = 0; stopIndex < 256; stopIndex++)
			cache->colors[stopIndex] = 0;
	} else if (static_cast<int>(grad->stops.size()) == 1) {
		unsigned int color = nsvg__applyOpacity(grad->stops[0].color, opacity);
		for (stopIndex = 0; stopIndex < 256; stopIndex++)
			cache->colors[stopIndex] = color;
	} else {
		unsigned int startColor, endColor = 0;
		double startOffset, endOffset, ratioStep, ratio;
		int startIndex, endIndex, count;

		startColor = nsvg__applyOpacity(grad->stops[0].color, opacity);
		startOffset = nsvg__clampf(grad->stops[0].offset, 0, 1);
		endOffset = nsvg__clampf(grad->stops[static_cast<int>(grad->stops.size())-1].offset, startOffset, 1);
		startIndex = (int)(startOffset * 255.0);
		endIndex = (int)(endOffset * 255.0);
		for (stopIndex = 0; stopIndex < startIndex; stopIndex++) {
			cache->colors[stopIndex] = startColor;
		}

		for (stopIndex = 0; stopIndex < static_cast<int>(grad->stops.size())-1; stopIndex++) {
			startColor = nsvg__applyOpacity(grad->stops[stopIndex].color, opacity);
			endColor = nsvg__applyOpacity(grad->stops[stopIndex+1].color, opacity);
			startOffset = nsvg__clampf(grad->stops[stopIndex].offset, 0, 1);
			endOffset = nsvg__clampf(grad->stops[stopIndex+1].offset, 0, 1);
			startIndex = (int)(startOffset * 255.0);
			endIndex = (int)(endOffset * 255.0);
			count = endIndex - startIndex;
			if (count <= 0) continue;
			ratio = 0;
			ratioStep = 1.0 / (double)count;
			for (colorIndex = 0; colorIndex < count; colorIndex++) {
				cache->colors[startIndex+colorIndex] = nsvg__lerpRGBA(startColor,endColor,ratio);
				ratio += ratioStep;
			}
		}

		for (stopIndex = endIndex; stopIndex < 256; stopIndex++)
			cache->colors[stopIndex] = endColor;
	}

}
static void rasterize(RasterizerState* raster, const Image& image, double offsetX, double offsetY, double scale,
                      unsigned char* dst, int width, int height, int stride) {
    raster->scanline.resize(width);
    raster->bitmap = dst;
    raster->width = width; raster->height = height; raster->stride = static_cast<std::size_t>(stride);
    for (int rowIndex = 0; rowIndex < height; ++rowIndex) std::memset(dst + static_cast<std::size_t>(rowIndex)*stride, 0, static_cast<std::size_t>(width)*4);
    for (const auto& shape : image.shapes) {
        if (!shape.visible) continue;
        for (auto order : shape.paintOrder) {
            const Paint* paint = nullptr;
            FillRule rule = shape.fillRule;
            raster->edges.clear();
            if (order == PaintOrder::fill && !std::holds_alternative<std::monostate>(shape.fill)) {
                paint = &shape.fill;
                nsvg__flattenShape(raster, &shape, scale);
            } else if (order == PaintOrder::stroke && !std::holds_alternative<std::monostate>(shape.stroke) && shape.strokeWidth*scale > 0.01) {
                paint = &shape.stroke;
                rule = FillRule::nonzero;
                nsvg__flattenShapeStroke(raster, &shape, scale);
            } else continue;
            for (auto& edge : raster->edges) {
                edge.startX += offsetX; edge.endX += offsetX;
                edge.startY = (offsetY + edge.startY)*NSVG__SUBSAMPLES;
                edge.endY = (offsetY + edge.endY)*NSVG__SUBSAMPLES;
            }
            std::erase_if(raster->edges, [](const auto& edge) {
                return !std::isfinite(edge.startX) || !std::isfinite(edge.endX) || !std::isfinite(edge.startY) || !std::isfinite(edge.endY);
            });
            std::sort(raster->edges.begin(), raster->edges.end(), [](const auto& first, const auto& second) { return first.startY < second.startY; });
            raster->activeEdges.clear();
            raster->freelist = nullptr;
            // Active/free links borrow vector elements; never grow after linking.
            raster->activeEdges.reserve(raster->edges.size());
            CachedPaint cache{};
            nsvg__initPaint(&cache, paint, shape.opacity);
            nsvg__rasterizeSortedEdges(raster, offsetX, offsetY, scale, &cache, rule);
        }
    }
    nsvg__unpremultiplyAlpha(dst, width, height, stride);
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
    int width, int height, int stride, double offsetX, double offsetY, double scale) {
    const auto size = detail::raster_buffer_size(width, height, stride);
    if (!size) return std::unexpected(size.error());
    if (!std::isfinite(offsetX) || !std::isfinite(offsetY) || !std::isfinite(scale) || scale <= 0 ||
        dst.size() < *size || !state_) return std::unexpected(Error::invalid_argument);
    if (*size == 0) return {};
    if (!detail::valid_image(image)) return std::unexpected(Error::invalid_argument);
    // Clear all borrowed call state even when a vector allocation throws.
    struct Reset {
        detail::RasterizerState& raster;
        ~Reset() { raster.bitmap = nullptr; raster.width = raster.height = 0; raster.stride = 0; raster.freelist = nullptr; }
    } reset{*state_};
    try {
        detail::rasterize(state_.get(), image, offsetX, offsetY, scale, dst.data(), width, height, stride);
        return {};
    } catch (const std::bad_alloc&) { return std::unexpected(Error::allocation_failure); }
      catch (const std::length_error&) { return std::unexpected(Error::size_overflow); }
}
} // namespace nanosvg

#endif // NANOSVGRAST_IMPLEMENTATION
#endif // NANOSVGRAST_HPP
