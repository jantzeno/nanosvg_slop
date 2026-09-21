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

struct RasterOptions {
    int width = 0, height = 0;
    Point offset{};
    double scale = 1.0;
};
struct Rgba8 {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
    bool operator==(const Rgba8&) const = default;
};
static_assert(sizeof(Rgba8) == 4);
struct RasterImage {
    int width = 0, height = 0;
    std::vector<Rgba8> pixels;
};

// Returns owned, tightly packed, straight-alpha pixels. No input or shared
// state is modified; concurrent calls may share an image that is not edited.
// Dimensions must be nonnegative, offsets finite, and scale finite and positive.
[[nodiscard]] std::expected<std::unique_ptr<RasterImage>, Error>
rasterize(const Image& image, const RasterOptions& options);
} // namespace nanosvg

#ifdef NANOSVGRAST_IMPLEMENTATION
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <variant>

namespace nanosvg::detail {
constexpr int raster_subsamples = 5;
constexpr int fixed_shift = 10;
constexpr int fixed_unit = 1 << fixed_shift;
constexpr int fixed_mask = fixed_unit - 1;
constexpr double tessellation_tolerance = 0.25;
constexpr double distance_tolerance = 0.01;
constexpr int raster_int_max = std::numeric_limits<int>::max();
constexpr int raster_int_min = std::numeric_limits<int>::min();

struct Direction {
    double x = 0, y = 0;
    bool operator==(const Direction&) const = default;
};
struct NormalizedDirection {
    Direction direction;
    double length;
};
struct FixedX { int value = 0; };
struct FixedStep { int value = 0; };
enum class EdgeWinding : int { negative = -1, positive = 1 };
struct Edge {
    Point start, end;
    EdgeWinding winding;
};
struct StrokePoint {
    Point position{};
    Direction direction{};
    Direction extrusion{};
    bool corner = false, bevel = false, leftTurn = false;
};
struct StrokeSides { Point left, right; };
struct ActiveEdge {
    FixedX x;
    FixedStep step;
    double endY = 0;
    EdgeWinding winding = EdgeWinding::positive;
    ActiveEdge* next = nullptr;
};
struct SolidPaint { Color color = 0; };
struct GradientRamp {
    Spread spread = Spread::pad;
    Transform xform{};
    std::array<Color, 256> colors{};
};
struct LinearPaint { GradientRamp ramp; };
struct RadialPaint {
    GradientRamp ramp;
    Point focus;
    double focusScale = 0;
};
using CachedPaint = std::variant<std::monostate, SolidPaint, LinearPaint, RadialPaint>;

static bool points_equal(Point first, Point second, double tolerance) {
    const double dx = second.x - first.x, dy = second.y - first.y;
    return dx*dx + dy*dy < tolerance*tolerance;
}
static NormalizedDirection normalize(Direction direction) {
    const double length = std::hypot(direction.x, direction.y);
    if (length > 1e-6) {
        // Scale finite components first if their true length exceeds double.
        // The returned length still records the overflow for dash limits.
        if (std::isinf(length) && std::isfinite(direction.x) && std::isfinite(direction.y)) {
            const double largest = std::max(std::abs(direction.x), std::abs(direction.y));
            direction.x /= largest;
            direction.y /= largest;
            const double scaledLength = std::hypot(direction.x, direction.y);
            direction.x /= scaledLength;
            direction.y /= scaledLength;
        } else {
            const double inverse = 1.0 / length;
            direction.x *= inverse;
            direction.y *= inverse;
        }
    }
    return {direction, length};
}
constexpr Direction difference(Point end, Point start) {
    return {end.x - start.x, end.y - start.y};
}
static int round_clamped(double value) {
    if (!std::isfinite(value)) return value > 0 ? raster_int_max : (value < 0 ? raster_int_min : 0);
    const double rounded = std::round(value);
    if (rounded >= static_cast<double>(raster_int_max)) return raster_int_max;
    if (rounded <= static_cast<double>(raster_int_min)) return raster_int_min;
    return static_cast<int>(rounded);
}
constexpr int add_saturated(int lhs, int rhs) {
    if (rhs > 0 && lhs > raster_int_max - rhs) return raster_int_max;
    if (rhs < 0 && lhs < raster_int_min - rhs) return raster_int_min;
    return lhs + rhs;
}
constexpr FixedX advance(FixedX position, FixedStep step) {
    return {add_saturated(position.value, step.value)};
}
static int curve_divisions(double radius, double arc, double tolerance) {
    const double angleStep = std::acos(radius / (radius + tolerance)) * 2.0;
    // Huge radii can round the ratio to one; use the minimum subdivision.
    if (!(angleStep > 0.0)) return 2;
    const double divisions = std::ceil(arc / angleStep);
    if (!std::isfinite(divisions) || divisions >= static_cast<double>(raster_int_max)) return 2;
    return divisions < 2 ? 2 : static_cast<int>(divisions);
}
static StrokeSides stroke_sides(Point center, Direction normal, double halfWidth) {
    return {{center.x - normal.x*halfWidth, center.y - normal.y*halfWidth},
            {center.x + normal.x*halfWidth, center.y + normal.y*halfWidth}};
}
static StrokeSides closed_sides(Point previous, Point point, double lineWidth) {
    const auto normalized = normalize(difference(point, previous));
    const auto direction = normalized.direction;
    // Retain the short-segment normalization threshold used by strokes.
    const Point midpoint = normalized.length > 1e-6
        ? Point{std::midpoint(previous.x, point.x), std::midpoint(previous.y, point.y)}
        : Point{previous.x + direction.x*normalized.length*0.5,
                previous.y + direction.y*normalized.length*0.5};
    return stroke_sides(midpoint, {direction.y, -direction.x}, lineWidth*0.5);
}
static double clamp_finite_or_nan(double value, double lower, double upper) {
    return std::isnan(value) ? lower : std::clamp(value, lower, upper);
}
static int gradient_index(double value, Spread spread) {
    // Overflowed coordinates retain the pad fallback; never cast NaN to an index.
    if (std::isfinite(value)) {
        if (spread == Spread::repeat) value -= std::floor(value);
        else if (spread == Spread::reflect) {
            value = std::fmod(value, 2.0);
            if (value < 0) value += 2.0;
            if (value > 1) value = 2.0 - value;
        }
    }
    return static_cast<int>(clamp_finite_or_nan(value, 0, 1) * 255.0);
}
static double radial_distance(Point point, const RadialPaint& paint) {
    const auto delta = difference(point, paint.focus);
    const double distance = std::hypot(delta.x, delta.y);
    if (distance == 0 || !std::isfinite(distance)) return distance;
    const double projection = (delta.x/distance)*paint.focus.x + (delta.y/distance)*paint.focus.y;
    const double root = std::sqrt(projection*projection + paint.focusScale);
    // Rationalize the outward case to avoid cancellation near the boundary.
    // ponytail: undefined SVG 1.1 boundary repeats use pad; SVG 2 needs averaged stops.
    const double boundary = projection > 0 ? paint.focusScale/(root + projection) : root - projection;
    return boundary > 0 ? distance/boundary : std::numeric_limits<double>::infinity();
}
constexpr Color pack_rgba(Rgba8 pixel) {
    return Color{pixel.r} | (Color{pixel.g} << 8) | (Color{pixel.b} << 16) | (Color{pixel.a} << 24);
}
constexpr Rgba8 unpack_rgba(Color color) {
    return {static_cast<std::uint8_t>(color), static_cast<std::uint8_t>(color >> 8),
            static_cast<std::uint8_t>(color >> 16), static_cast<std::uint8_t>(color >> 24)};
}
static Color lerp_rgba(Color startColor, Color endColor, double ratio) {
    const auto weight = static_cast<unsigned>(clamp_finite_or_nan(ratio, 0, 1)*256.0);
    const auto channel = [=](unsigned shift) {
        return static_cast<std::uint8_t>((((startColor >> shift) & 0xff)*(256-weight) +
                                         ((endColor >> shift) & 0xff)*weight) >> 8);
    };
    return pack_rgba({channel(0), channel(8), channel(16), channel(24)});
}
static Color apply_opacity(Color color, double opacity) {
    const auto weight = static_cast<unsigned>(clamp_finite_or_nan(opacity, 0, 1)*256.0);
    auto pixel = unpack_rgba(color);
    pixel.a = static_cast<std::uint8_t>((pixel.a*weight) >> 8);
    return pack_rgba(pixel);
}
constexpr int div255(int value) { return ((value + 1)*257) >> 16; }
constexpr Rgba8 blend_pixel(Rgba8 destination, std::uint8_t coverage, Rgba8 source) {
    const int alpha = div255(coverage*source.a);
    if (alpha == 0) return destination;
    if (alpha == 255) return source;
    const int inverse = 255 - alpha;
    const auto channel = [=](int foreground, int background) {
        return static_cast<std::uint8_t>(div255(foreground*alpha) + div255(inverse*background));
    };
    return {channel(source.r, destination.r), channel(source.g, destination.g), channel(source.b, destination.b),
            static_cast<std::uint8_t>(alpha + div255(inverse*destination.a))};
}
static CachedPaint make_paint(const Paint& paint, double opacity) {
    if (const auto* color = std::get_if<Color>(&paint)) return SolidPaint{apply_opacity(*color, opacity)};
    const auto* gradient = std::get_if<Gradient>(&paint);
    if (!gradient) return {};
    GradientRamp ramp{gradient->spread, gradient->xform, {}};
    const auto& stops = gradient->stops;
    if (stops.size() == 1) ramp.colors.fill(apply_opacity(stops.front().color, opacity));
    else if (!stops.empty()) {
        Color startColor = apply_opacity(stops.front().color, opacity), endColor = 0;
        const double firstOffset = clamp_finite_or_nan(stops.front().offset, 0, 1);
        int endIndex = 0;
        std::fill_n(ramp.colors.begin(), static_cast<int>(firstOffset*255.0), startColor);
        for (std::size_t index = 0; index + 1 < stops.size(); ++index) {
            startColor = apply_opacity(stops[index].color, opacity);
            endColor = apply_opacity(stops[index+1].color, opacity);
            const int startIndex = static_cast<int>(clamp_finite_or_nan(stops[index].offset, 0, 1)*255.0);
            endIndex = static_cast<int>(clamp_finite_or_nan(stops[index+1].offset, 0, 1)*255.0);
            const int count = endIndex - startIndex;
            if (count <= 0) continue;
            double ratio = 0;
            const double step = 1.0 / count;
            for (int colorIndex = 0; colorIndex < count; ++colorIndex) {
                ramp.colors[startIndex+colorIndex] = lerp_rgba(startColor, endColor, ratio);
                ratio += step;
            }
        }
        std::fill(ramp.colors.begin() + endIndex, ramp.colors.end(), endColor);
    }
    if (gradient->kind == GradientKind::linear) return LinearPaint{std::move(ramp)};
    Point focus{gradient->fx, gradient->fy};
    const double focusLength = std::hypot(focus.x, focus.y);
    double focusScale = 0;
    if (focusLength >= 1) {
        const double largest = std::max(std::abs(focus.x), std::abs(focus.y));
        focus.x /= largest;
        focus.y /= largest;
        const double length = std::hypot(focus.x, focus.y);
        focus.x /= length;
        focus.y /= length;
    } else focusScale = (1 - focusLength)*(1 + focusLength);
    return RadialPaint{std::move(ramp), focus, focusScale};
}

class Rasterizer {
  public:
    explicit Rasterizer(RasterOptions options) : options_(options), image_(std::make_unique<RasterImage>()) {}
    Rasterizer(const Rasterizer&) = delete;
    Rasterizer& operator=(const Rasterizer&) = delete;
    Rasterizer(Rasterizer&&) = delete;
    Rasterizer& operator=(Rasterizer&&) = delete;
    std::unique_ptr<RasterImage> run(const Image& image, std::size_t pixelCount);
  private:
    const RasterOptions options_;
    std::unique_ptr<RasterImage> image_;
    std::vector<Edge> edges_;
    std::vector<StrokePoint> points_, dashPoints_;
    std::vector<ActiveEdge> activeEdges_;
    ActiveEdge* freelist_ = nullptr; // Borrows stable elements of activeEdges_.
    std::vector<std::uint8_t> scanline_;
    void append_point(StrokePoint point);
    void add_path_point(Point point, bool corner);
    void add_edge(Point start, Point end);
    void flatten_cubic(std::span<const Point, 4> curve, int level, bool corner);
    void flatten_path(const Path& path, bool corners);
    void flatten_shape(const Shape& shape);
    StrokeSides cap(StrokeSides previous, Point point, Direction direction, double width, int divisions,
                    LineCap kind, bool connect);
    StrokeSides join(StrokeSides previous, const StrokePoint& before, const StrokePoint& point,
                     double width, int divisions, LineJoin kind);
    void prepare_stroke(double miterLimit, LineJoin kind);
    void expand_stroke(std::span<const StrokePoint> points, bool closed, LineJoin join, LineCap cap, double width);
    void flatten_stroke(const Shape& shape);
    ActiveEdge* add_active(const Edge& edge, double scanY);
    void fill_scanline(FixedX start, FixedX end, int weight);
    void fill_active_edges(const ActiveEdge* edge, int weight, FillRule rule);
    void scanline_solid(std::span<Rgba8> destination, std::span<const std::uint8_t> coverage,
                        int x, int y, const CachedPaint& paint);
    void rasterize_edges(const CachedPaint& paint, FillRule rule);
    void unpremultiply_alpha();
    int minPixel_ = 0, maxPixel_ = 0;
};

void Rasterizer::append_point(StrokePoint point) {
    if (points_.size() == static_cast<std::size_t>(raster_int_max)) throw std::length_error("too many points");
    points_.push_back(point);
}
void Rasterizer::add_path_point(Point point, bool corner) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return;
    if (!points_.empty() && points_equal(points_.back().position, point, distance_tolerance)) {
        points_.back().corner |= corner;
        return;
    }
    append_point({.position = point, .corner = corner});
}
void Rasterizer::add_edge(Point start, Point end) {
    if (start.y == end.y || !std::isfinite(start.x) || !std::isfinite(start.y) ||
        !std::isfinite(end.x) || !std::isfinite(end.y)) return;
    if (edges_.size() == static_cast<std::size_t>(raster_int_max)) throw std::length_error("too many edges");
    if (start.y < end.y) edges_.push_back({start, end, EdgeWinding::positive});
    else edges_.push_back({end, start, EdgeWinding::negative});
}
void Rasterizer::flatten_cubic(std::span<const Point, 4> curve, int level, bool corner) {
    if (level > 10) return;
    const auto [start, control1, control2, end] = std::array{curve[0], curve[1], curve[2], curve[3]};
    const auto delta = difference(end, start);
    const double deviation1 = std::abs((control1.x - end.x)*delta.y - (control1.y - end.y)*delta.x);
    const double deviation2 = std::abs((control2.x - end.x)*delta.y - (control2.y - end.y)*delta.x);
    if ((deviation1 + deviation2)*(deviation1 + deviation2) <
        tessellation_tolerance*(delta.x*delta.x + delta.y*delta.y)) {
        add_path_point(end, corner);
        return;
    }
    const auto midpoint = [](Point first, Point second) {
        return Point{std::midpoint(first.x, second.x), std::midpoint(first.y, second.y)};
    };
    const auto p12 = midpoint(start, control1), p23 = midpoint(control1, control2), p34 = midpoint(control2, end);
    const auto p123 = midpoint(p12, p23), p234 = midpoint(p23, p34), middle = midpoint(p123, p234);
    flatten_cubic(std::array{start, p12, p123, middle}, level + 1, false);
    flatten_cubic(std::array{middle, p234, p34, end}, level + 1, corner);
}
void Rasterizer::flatten_path(const Path& path, bool corners) {
    points_.clear();
    if (path.points.empty()) return;
    const double scale = options_.scale;
    const auto scaled = [scale](Point point) { return Point{point.x*scale, point.y*scale}; };
    add_path_point(scaled(path.points.front()), corners);
    for (std::size_t index = 0; index + 3 < path.points.size(); index += 3) {
        const auto curve = std::span(path.points).subspan(index, 4);
        flatten_cubic(std::array{scaled(curve[0]), scaled(curve[1]), scaled(curve[2]), scaled(curve[3])}, 0, corners);
    }
}
void Rasterizer::flatten_shape(const Shape& shape) {
    for (const auto& path : shape.paths) {
        if (path.points.empty()) continue;
        flatten_path(path, false);
        const double scale = options_.scale;
        add_path_point({path.points.front().x*scale, path.points.front().y*scale}, false);
        if (points_.empty()) continue;
        Point previous = points_.back().position;
        for (const auto& point : points_) {
            add_edge(previous, point.position);
            previous = point.position;
        }
    }
}
StrokeSides Rasterizer::cap(StrokeSides previous, Point point, Direction direction, double width,
                            int divisions, LineCap kind, bool connect) {
    const double halfWidth = width*0.5;
    const Direction normal{direction.y, -direction.x};
    StrokeSides result{};
    if (kind == LineCap::round) {
        Point before{};
        for (int step = 0; step < divisions; ++step) {
            const double angle = static_cast<double>(step)/(divisions-1)*std::numbers::pi;
            const double offsetX = std::cos(angle)*halfWidth, offsetY = std::sin(angle)*halfWidth;
            const Point next{point.x - normal.x*offsetX - direction.x*offsetY,
                             point.y - normal.y*offsetX - direction.y*offsetY};
            if (step > 0) add_edge(before, next);
            else result.left = next;
            before = next;
        }
        result.right = before;
    } else {
        if (kind == LineCap::square) {
            point.x -= direction.x*halfWidth;
            point.y -= direction.y*halfWidth;
        }
        result = stroke_sides(point, normal, halfWidth);
        add_edge(result.left, result.right);
    }
    if (connect) {
        add_edge(previous.left, result.left);
        add_edge(result.right, previous.right);
    }
    return result;
}
StrokeSides Rasterizer::join(StrokeSides previous, const StrokePoint& before, const StrokePoint& point,
                             double width, int divisions, LineJoin kind) {
    const double halfWidth = width*0.5;
    if (!point.corner) {
        const auto result = stroke_sides(point.position, point.extrusion, halfWidth);
        add_edge(result.left, previous.left);
        add_edge(previous.right, result.right);
        return result;
    }
    const Direction normal0{before.direction.y, -before.direction.x};
    const Direction normal1{point.direction.y, -point.direction.x};
    if (kind == LineJoin::round) {
        const double startAngle = std::atan2(normal0.y, normal0.x);
        double sweep = std::atan2(normal1.y, normal1.x) - startAngle;
        if (sweep < std::numbers::pi) sweep += std::numbers::pi*2;
        if (sweep > std::numbers::pi) sweep -= std::numbers::pi*2;
        const int steps = std::clamp(round_clamped(std::ceil(std::abs(sweep)/std::numbers::pi*divisions)), 2, divisions);
        for (int step = 0; step < steps; ++step) {
            const double angle = startAngle + static_cast<double>(step)/(steps-1)*sweep;
            const auto next = stroke_sides(point.position, {std::cos(angle), std::sin(angle)}, halfWidth);
            add_edge(next.left, previous.left);
            add_edge(previous.right, next.right);
            previous = next;
        }
        return previous;
    }
    auto incoming = stroke_sides(point.position, normal0, halfWidth);
    auto outgoing = stroke_sides(point.position, normal1, halfWidth);
    if (kind == LineJoin::bevel || point.bevel) {
        add_edge(incoming.left, previous.left);
        add_edge(outgoing.left, incoming.left);
        add_edge(previous.right, incoming.right);
        add_edge(incoming.right, outgoing.right);
    } else {
        const auto miter = stroke_sides(point.position, point.extrusion, halfWidth);
        if (point.leftTurn) {
            outgoing.left = miter.left;
            add_edge(outgoing.left, previous.left);
            add_edge(previous.right, incoming.right);
            add_edge(incoming.right, outgoing.right);
        } else {
            outgoing.right = miter.right;
            add_edge(incoming.left, previous.left);
            add_edge(outgoing.left, incoming.left);
            add_edge(previous.right, outgoing.right);
        }
    }
    return outgoing;
}
void Rasterizer::prepare_stroke(double miterLimit, LineJoin kind) {
    if (points_.empty()) return;
    for (std::size_t index = 0; index < points_.size(); ++index) {
        auto& previous = points_[index == 0 ? points_.size()-1 : index-1];
        const auto normalized = normalize(difference(points_[index].position, previous.position));
        previous.direction = normalized.direction;
    }
    for (std::size_t index = 0; index < points_.size(); ++index) {
        const auto& before = points_[index == 0 ? points_.size()-1 : index-1];
        auto& point = points_[index];
        point.extrusion = {(before.direction.y + point.direction.y)*0.5,
                          (-before.direction.x - point.direction.x)*0.5};
        const double squared = point.extrusion.x*point.extrusion.x + point.extrusion.y*point.extrusion.y;
        if (squared > 0.000001) {
            const double miterScale = std::min(1.0/squared, 600.0);
            point.extrusion.x *= miterScale;
            point.extrusion.y *= miterScale;
        }
        point.leftTurn = point.direction.x*before.direction.y - before.direction.x*point.direction.y > 0;
        point.bevel = point.corner && (squared*miterLimit*miterLimit < 1.0 ||
                                      kind == LineJoin::bevel || kind == LineJoin::round);
    }
}
void Rasterizer::expand_stroke(std::span<const StrokePoint> points, bool closed, LineJoin joinKind,
                              LineCap capKind, double width) {
    if (points.empty() || (!closed && points.size() < 2)) return;
    const int divisions = curve_divisions(width*0.5, std::numbers::pi, tessellation_tolerance);
    StrokeSides sides = closed
        ? closed_sides(points.back().position, points.front().position, width)
        : cap({}, points.front().position, normalize(difference(points[1].position, points[0].position)).direction,
              width, divisions, capKind, false);
    const auto first = sides;
    const std::size_t end = closed ? points.size() : points.size()-1;
    for (std::size_t index = closed ? 0 : 1; index < end; ++index) {
        const auto& previous = points[index == 0 ? points.size()-1 : index-1];
        sides = join(sides, previous, points[index], width, divisions, joinKind);
    }
    if (closed) {
        add_edge(first.left, sides.left);
        add_edge(sides.right, first.right);
    } else {
        const auto direction = normalize(difference(points.back().position, points[points.size()-2].position)).direction;
        cap({sides.right, sides.left}, points.back().position, {-direction.x, -direction.y},
            width, divisions, capKind, true);
    }
}
void Rasterizer::flatten_stroke(const Shape& shape) {
    const double scale = options_.scale;
    const double width = shape.strokeWidth*scale;
    // ponytail: limit dash work per shape; omit the remainder on exhaustion.
    // Clip paths before dashing if larger patterns need to be rendered in full.
    int dashBudget = 10000;
    for (const auto& path : shape.paths) {
        flatten_path(path, true);
        if (points_.size() < 2) continue;
        bool closed = path.closed;
        if (points_equal(points_.back().position, points_.front().position, distance_tolerance)) {
            points_.pop_back();
            closed = true;
        }
        if (shape.strokeDashArray.empty()) {
            prepare_stroke(shape.miterLimit, shape.strokeLineJoin);
            expand_stroke(points_, closed, shape.strokeLineJoin, shape.strokeLineCap, width);
            continue;
        }
        if (closed) append_point(points_.front());
        // Transfer the path while retaining both buffers' capacity for later paths.
        dashPoints_.swap(points_);
        points_.clear();
        auto current = dashPoints_.front();
        append_point(current);
        const auto& pattern = shape.strokeDashArray;
        double patternLength = std::accumulate(pattern.begin(), pattern.end(), 0.0);
        if (pattern.size() % 2 != 0) patternLength *= 2.0;
        if (!(patternLength > 0) || !std::isfinite(patternLength)) continue;
        double offset = std::fmod(shape.strokeDashOffset, patternLength);
        if (!std::isfinite(offset)) continue;
        if (offset < 0) offset += patternLength;
        std::size_t dashIndex = 0;
        bool drawDash = true;
        while (offset > pattern[dashIndex]) {
            if (--dashBudget < 0) return;
            offset -= pattern[dashIndex];
            dashIndex = (dashIndex + 1) % pattern.size();
            drawDash = !drawDash;
        }
        double dashLength = (pattern[dashIndex] - offset)*scale;
        double totalDistance = 0;
        const auto stroke = [&] {
            if (points_.size() > 1 && drawDash) {
                prepare_stroke(shape.miterLimit, shape.strokeLineJoin);
                expand_stroke(points_, false, shape.strokeLineJoin, shape.strokeLineCap, width);
            }
        };
        for (std::size_t index = 1; index < dashPoints_.size();) {
            const auto delta = difference(dashPoints_[index].position, current.position);
            const double distance = std::hypot(delta.x, delta.y);
            if (--dashBudget < 0 || !std::isfinite(distance)) return;
            if (totalDistance + distance > dashLength) {
                const double ratio = (dashLength - totalDistance)/distance;
                const Point split{current.position.x + delta.x*ratio, current.position.y + delta.y*ratio};
                // Zero-length entries may toggle the pattern without moving.
                if (dashLength > totalDistance && split.x == current.position.x && split.y == current.position.y) return;
                add_path_point(split, true);
                stroke();
                drawDash = !drawDash;
                dashIndex = (dashIndex + 1) % pattern.size();
                dashLength = pattern[dashIndex]*scale;
                current.position = split;
                current.corner = true;
                current.bevel = current.leftTurn = false;
                totalDistance = 0;
                points_.clear();
                append_point(current);
            } else {
                totalDistance += distance;
                current = dashPoints_[index++];
                append_point(current);
            }
        }
        stroke();
    }
}

ActiveEdge* Rasterizer::add_active(const Edge& edge, double scanY) {
    ActiveEdge* active = nullptr;
    if (freelist_) {
        active = freelist_;
        freelist_ = active->next;
    } else {
        activeEdges_.emplace_back();
        active = &activeEdges_.back();
    }
    const double slope = (edge.end.x - edge.start.x)/(edge.end.y - edge.start.y);
    *active = {{round_clamped(fixed_unit*(edge.start.x + slope*(scanY - edge.start.y)))},
               {round_clamped(fixed_unit*slope)}, edge.end.y, edge.winding, nullptr};
    return active;
}
void Rasterizer::fill_scanline(FixedX start, FixedX end, int weight) {
    int startPixel = start.value >> fixed_shift;
    int endPixel = end.value >> fixed_shift;
    minPixel_ = std::min(minPixel_, startPixel);
    maxPixel_ = std::max(maxPixel_, endPixel);
    const int width = options_.width;
    if (startPixel >= width || endPixel < 0) return;
    if (startPixel == endPixel) {
        scanline_[startPixel] = static_cast<std::uint8_t>(scanline_[startPixel] +
            ((end.value - start.value)*weight >> fixed_shift));
    } else {
        if (startPixel >= 0)
            scanline_[startPixel] = static_cast<std::uint8_t>(scanline_[startPixel] +
                ((fixed_unit - (start.value & fixed_mask))*weight >> fixed_shift));
        else startPixel = -1;
        if (endPixel < width)
            scanline_[endPixel] = static_cast<std::uint8_t>(scanline_[endPixel] +
                ((end.value & fixed_mask)*weight >> fixed_shift));
        else endPixel = width;
        for (++startPixel; startPixel < endPixel; ++startPixel)
            scanline_[startPixel] = static_cast<std::uint8_t>(scanline_[startPixel] + weight);
    }
}
void Rasterizer::fill_active_edges(const ActiveEdge* edge, int weight, FillRule rule) {
    FixedX start{};
    int winding = 0;
    for (; edge; edge = edge->next) {
        if (winding == 0) start = edge->x;
        if (rule == FillRule::nonzero) winding += std::to_underlying(edge->winding);
        else winding = 1 - winding;
        if (winding == 0) fill_scanline(start, edge->x, weight);
    }
}
void Rasterizer::scanline_solid(std::span<Rgba8> destination, std::span<const std::uint8_t> coverage,
                                int x, int y, const CachedPaint& paint) {
    // Dispatch once per row; solid channels and gradient transforms stay outside pixel loops.
    if (const auto* solid = std::get_if<SolidPaint>(&paint)) {
        const auto color = unpack_rgba(solid->color);
        for (std::size_t index = 0; index < destination.size(); ++index)
            destination[index] = blend_pixel(destination[index], coverage[index], color);
        return;
    }
    const double scale = options_.scale;
    const double sampleX = (static_cast<double>(x) - options_.offset.x)/scale;
    const double sampleY = (static_cast<double>(y) - options_.offset.y)/scale;
    if (const auto* linear = std::get_if<LinearPaint>(&paint)) {
        const auto& ramp = linear->ramp;
        const auto& transform = ramp.xform;
        const double start = sampleX*transform[1] + sampleY*transform[3] + transform[5];
        const double step = transform[1]/scale;
        for (std::size_t index = 0; index < destination.size(); ++index) {
            // Index from the row origin so long scanlines do not accumulate drift.
            const auto color = ramp.colors[gradient_index(start + static_cast<double>(index)*step, ramp.spread)];
            destination[index] = blend_pixel(destination[index], coverage[index], unpack_rgba(color));
        }
    } else if (const auto* radial = std::get_if<RadialPaint>(&paint)) {
        const auto& ramp = radial->ramp;
        const auto& transform = ramp.xform;
        const Point start{sampleX*transform[0] + sampleY*transform[2] + transform[4],
                          sampleX*transform[1] + sampleY*transform[3] + transform[5]};
        const Direction step{transform[0]/scale, transform[1]/scale};
        const auto scan = [&](auto distance) {
            for (std::size_t index = 0; index < destination.size(); ++index) {
                const double offset = static_cast<double>(index);
                const double value = distance(Point{start.x + offset*step.x, start.y + offset*step.y});
                const auto color = ramp.colors[gradient_index(value, ramp.spread)];
                destination[index] = blend_pixel(destination[index], coverage[index], unpack_rgba(color));
            }
        };
        if (radial->focus.x == 0 && radial->focus.y == 0) scan([](Point point) {
            const double squared = point.x*point.x + point.y*point.y;
            return std::isfinite(squared) ? std::sqrt(squared) : std::hypot(point.x, point.y);
        });
        else scan([&](Point point) { return radial_distance(point, *radial); });
    }
}
void Rasterizer::rasterize_edges(const CachedPaint& paint, FillRule rule) {
    ActiveEdge* active = nullptr;
    std::size_t edgeIndex = 0;
    constexpr int weight = 255/raster_subsamples;
    const int width = options_.width, height = options_.height;
    for (int row = 0; row < height; ++row) {
        std::ranges::fill(scanline_, 0);
        minPixel_ = width;
        maxPixel_ = 0;
        for (int sample = 0; sample < raster_subsamples; ++sample) {
            const double scanY = (static_cast<double>(row)*raster_subsamples + sample) + 0.5;
            auto** link = &active;
            while (*link) {
                auto* edge = *link;
                if (edge->endY <= scanY) {
                    *link = edge->next;
                    edge->next = freelist_;
                    freelist_ = edge;
                } else {
                    edge->x = advance(edge->x, edge->step);
                    link = &edge->next;
                }
            }
            for (;;) {
                bool changed = false;
                link = &active;
                while (*link && (*link)->next) {
                    if ((*link)->x.value > (*link)->next->x.value) {
                        auto* current = *link;
                        auto* next = current->next;
                        current->next = next->next;
                        next->next = current;
                        *link = next;
                        changed = true;
                    }
                    link = &(*link)->next;
                }
                if (!changed) break;
            }
            while (edgeIndex < edges_.size() && edges_[edgeIndex].start.y <= scanY) {
                if (edges_[edgeIndex].end.y > scanY) {
                    auto* edge = add_active(edges_[edgeIndex], scanY);
                    if (!active || edge->x.value < active->x.value) {
                        edge->next = active;
                        active = edge;
                    } else {
                        auto* previous = active;
                        while (previous->next && previous->next->x.value < edge->x.value)
                            previous = previous->next;
                        edge->next = previous->next;
                        previous->next = edge;
                    }
                }
                ++edgeIndex;
            }
            fill_active_edges(active, weight, rule);
        }
        const int first = std::max(minPixel_, 0), last = std::min(maxPixel_, width-1);
        if (first <= last) {
            const auto count = static_cast<std::size_t>(last-first+1);
            const auto offset = static_cast<std::size_t>(row)*width + first;
            scanline_solid(std::span(image_->pixels).subspan(offset, count),
                           std::span(scanline_).subspan(first, count), first, row, paint);
        }
    }
}
constexpr Rgba8 unpremultiply_pixel(Rgba8 pixel) {
    if (pixel.a != 0) {
        pixel.r = static_cast<std::uint8_t>(pixel.r*255/pixel.a);
        pixel.g = static_cast<std::uint8_t>(pixel.g*255/pixel.a);
        pixel.b = static_cast<std::uint8_t>(pixel.b*255/pixel.a);
    }
    return pixel;
}
void Rasterizer::unpremultiply_alpha() {
    for (auto& pixel : image_->pixels) pixel = unpremultiply_pixel(pixel);
    const auto width = static_cast<std::size_t>(options_.width);
    const auto height = static_cast<std::size_t>(options_.height);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const auto index = y*width + x;
            auto& pixel = image_->pixels[index];
            if (pixel.a != 0) continue;
            int red = 0, green = 0, blue = 0, neighbors = 0;
            const auto include = [&](Rgba8 neighbor) {
                if (neighbor.a == 0) return;
                red += neighbor.r;
                green += neighbor.g;
                blue += neighbor.b;
                ++neighbors;
            };
            // Preserve the original defringing neighborhood at the top/left edge.
            if (x > 1) include(image_->pixels[index-1]);
            if (x+1 < width) include(image_->pixels[index+1]);
            if (y > 1) include(image_->pixels[index-width]);
            if (y+1 < height) include(image_->pixels[index+width]);
            if (neighbors > 0) {
                pixel.r = static_cast<std::uint8_t>(red/neighbors);
                pixel.g = static_cast<std::uint8_t>(green/neighbors);
                pixel.b = static_cast<std::uint8_t>(blue/neighbors);
            }
        }
    }
}
std::unique_ptr<RasterImage> Rasterizer::run(const Image& image, std::size_t pixelCount) {
    image_->width = options_.width;
    image_->height = options_.height;
    image_->pixels.resize(pixelCount);
    if (pixelCount == 0) return std::move(image_);
    scanline_.resize(options_.width);
    for (const auto& shape : image.shapes) {
        if (!shape.visible) continue;
        for (auto order : shape.paintOrder) {
            const Paint* paint = nullptr;
            FillRule rule = shape.fillRule;
            edges_.clear();
            if (order == PaintOrder::fill && !std::holds_alternative<std::monostate>(shape.fill)) {
                paint = &shape.fill;
                flatten_shape(shape);
            } else if (order == PaintOrder::stroke && !std::holds_alternative<std::monostate>(shape.stroke) &&
                       shape.strokeWidth*options_.scale > 0.01) {
                paint = &shape.stroke;
                rule = FillRule::nonzero;
                flatten_stroke(shape);
            } else continue;
            for (auto& edge : edges_) {
                edge.start.x += options_.offset.x;
                edge.end.x += options_.offset.x;
                edge.start.y = (options_.offset.y + edge.start.y)*raster_subsamples;
                edge.end.y = (options_.offset.y + edge.end.y)*raster_subsamples;
            }
            std::erase_if(edges_, [](const Edge& edge) {
                return !std::isfinite(edge.start.x) || !std::isfinite(edge.end.x) ||
                       !std::isfinite(edge.start.y) || !std::isfinite(edge.end.y);
            });
            std::ranges::sort(edges_, {}, [](const Edge& edge) { return edge.start.y; });
            activeEdges_.clear();
            freelist_ = nullptr;
            // Active/free links borrow vector elements; never grow after linking.
            activeEdges_.reserve(edges_.size());
            const auto cache = make_paint(*paint, shape.opacity);
            rasterize_edges(cache, rule);
        }
    }
    unpremultiply_alpha();
    return std::move(image_);
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
            shape.strokeDashArray.size() > raster_int_max) return false;
        for (double dash : shape.strokeDashArray) if (!std::isfinite(dash) || dash < 0) return false;
        for (const auto& path : shape.paths) {
            if (path.points.empty()) continue;
            if (path.points.size() < 4 || path.points.size() % 3 != 1 || path.points.size() > raster_int_max) return false;
            for (const auto& point : path.points)
                if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
        }
        for (const Paint* paint : {&shape.fill, &shape.stroke}) {
            if (paint->valueless_by_exception()) return false;
            if (const auto* grad = std::get_if<Gradient>(paint)) {
                if ((grad->kind != GradientKind::linear && grad->kind != GradientKind::radial) ||
                    grad->spread < Spread::pad || grad->spread > Spread::repeat) return false;
                if (grad->stops.size() > raster_int_max) return false;
                if (!std::isfinite(grad->fx) || !std::isfinite(grad->fy)) return false;
                for (double value : grad->xform) if (!std::isfinite(value)) return false;
                for (const auto& stop : grad->stops) if (!std::isfinite(stop.offset)) return false;
            }
        }
    }
    return true;
}
static std::expected<std::size_t, Error> raster_pixel_count(const RasterOptions& options) {
    const auto width = static_cast<std::size_t>(options.width);
    const auto height = static_cast<std::size_t>(options.height);
    if (width == 0 || height == 0) return 0;
    const auto limit = std::min(std::numeric_limits<std::size_t>::max()/sizeof(Rgba8),
                                std::vector<Rgba8>{}.max_size());
    if (width > limit/height) return std::unexpected(Error::size_overflow);
    return width*height;
}
} // namespace nanosvg::detail

namespace nanosvg {
std::expected<std::unique_ptr<RasterImage>, Error> rasterize(const Image& image, const RasterOptions& options) {
    if (options.width < 0 || options.height < 0 ||
        !std::isfinite(options.offset.x) || !std::isfinite(options.offset.y) ||
        !std::isfinite(options.scale) || options.scale <= 0)
        return std::unexpected(Error::invalid_argument);
    const auto count = detail::raster_pixel_count(options);
    if (!count) return std::unexpected(count.error());
    if (*count != 0 && !detail::valid_image(image)) return std::unexpected(Error::invalid_argument);
    try {
        return detail::Rasterizer(options).run(image, *count);
    } catch (const std::bad_alloc&) { return std::unexpected(Error::allocation_failure); }
      catch (const std::length_error&) { return std::unexpected(Error::size_overflow); }
}
} // namespace nanosvg
#endif // NANOSVGRAST_IMPLEMENTATION
#endif // NANOSVGRAST_HPP
