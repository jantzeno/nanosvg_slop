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
 * Bounding box calculation based on
 * http://blog.hackers-cafe.net/2009/06/how-to-calculate-bezier-curves-bounding.html
 *
 */

// Altered for NanoSVG 2: native C++23 ownership and double precision.
// Define NANOSVG_IMPLEMENTATION in exactly one C++23 translation unit
// before the first NanoSVG include to compile its implementation.
#ifndef NANOSVG_HPP
#define NANOSVG_HPP

#include <array>
#include <cstdint>
#include <limits>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nanosvg {

enum class OutputUnit { px, pt, pc, mm, cm, in };
enum class Error { invalid_argument, io_error, allocation_failure, size_overflow };
enum class GradientKind { linear = 2, radial = 3 };
enum class Spread { pad, reflect, repeat };
enum class LineJoin { miter, round, bevel };
enum class LineCap { butt, round, square };
enum class FillRule { nonzero, evenodd };
enum class PaintOrder { fill, markers, stroke };
using Color = std::uint32_t;          // Packed ABGR; raster output is byte-ordered RGBA.
using Bounds = std::array<double, 4>; // min x, min y, max x, max y
using Transform = std::array<double, 6>;

struct Point {
    double x = 0.0, y = 0.0;
};
struct GradientStop {
    Color color = 0;
    double offset = 0.0;
};
struct Gradient {
    GradientKind kind = GradientKind::linear;
    Transform xform{1, 0, 0, 1, 0, 0};
    Spread spread = Spread::pad;
    double fx = 0.0, fy = 0.0;
    std::vector<GradientStop> stops;
};
using Paint = std::variant<std::monostate, Color, Gradient>;

struct Path {
    // One start point followed by groups of three cubic Bezier points.
    std::vector<Point> points;
    bool closed = false;
    Bounds bounds{};
};

struct Shape {
    std::string id;
    Paint fill, stroke;
    double opacity = 1.0;
    double strokeWidth = 1.0, strokeDashOffset = 0.0;
    std::vector<double> strokeDashArray;
    LineJoin strokeLineJoin = LineJoin::miter;
    LineCap strokeLineCap = LineCap::butt;
    double miterLimit = 4.0;
    FillRule fillRule = FillRule::nonzero;
    std::array<PaintOrder, 3> paintOrder{PaintOrder::fill, PaintOrder::stroke, PaintOrder::markers};
    bool visible = true;
    Bounds bounds{};
    std::string fillGradient, strokeGradient;
    Transform xform{1, 0, 0, 1, 0, 0};
    std::vector<Path> paths;
};

struct Image {
    double width = 0.0, height = 0.0;
    std::vector<Shape> shapes;
};

// Owns its result independently of input. Unsupported/malformed SVG is parsed
// permissively; errors describe API arguments, resources, or file I/O.
[[nodiscard]] std::expected<std::unique_ptr<Image>, Error>
parse(std::string_view svg, std::string_view units = "px", double dpi = 96.0);
[[nodiscard]] std::expected<std::unique_ptr<Image>, Error>
parse_file(const std::filesystem::path &filename, std::string_view units = "px", double dpi = 96.0);

[[nodiscard]] std::expected<std::unique_ptr<Image>, Error> parse(std::string_view svg, OutputUnit units,
                                                                 double dpi = 96.0);
[[nodiscard]] std::expected<std::unique_ptr<Image>, Error> parse_file(const std::filesystem::path &filename,
                                                                      OutputUnit units, double dpi = 96.0);

} // namespace nanosvg

// Shared implementation details; not part of the public API.
namespace nanosvg::detail {
inline std::expected<std::size_t, Error> raster_buffer_size(int width, int height, int stride) {
    if (width < 0 || height < 0 || stride < 0) return std::unexpected(Error::invalid_argument);
    if (width == 0 || height == 0) return 0;
    const auto row = static_cast<std::size_t>(width) * 4;
    if (row / 4 != static_cast<std::size_t>(width)) return std::unexpected(Error::size_overflow);
    if (row > static_cast<std::size_t>(stride)) return std::unexpected(Error::invalid_argument);
    const auto rows = static_cast<std::size_t>(height - 1);
    if (rows > (std::numeric_limits<std::size_t>::max() - row) / static_cast<std::size_t>(stride))
        return std::unexpected(Error::size_overflow);
    return rows * static_cast<std::size_t>(stride) + row;
}
} // namespace nanosvg::detail

#ifdef NANOSVG_IMPLEMENTATION
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <new>
#include <numbers>
#include <numeric>
#include <span>
#include <stdexcept>
#include <utility>

namespace nanosvg {
namespace detail {
using std::isfinite;
using std::isnan;
constexpr double circlePi = std::numbers::pi;
constexpr double kappa90 = 0.5522847493;
constexpr double epsilon = 1e-12;
constexpr Transform identity{1, 0, 0, 1, 0, 0};
constexpr Color rgb(Color red, Color green, Color blue) {
    return red | (green << 8) | (blue << 16);
}
constexpr bool is_space(char character) {
    return std::string_view(" \t\n\v\f\r").contains(character);
}
constexpr bool is_digit(char character) {
    return character >= '0' && character <= '9';
}
// Preserve the original operand choice for NaNs and signed zero.
constexpr double min_value(double lhs, double rhs) {
    return lhs < rhs ? lhs : rhs;
}
constexpr double max_value(double lhs, double rhs) {
    return lhs > rhs ? lhs : rhs;
}
static std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\n\v\f\r");
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\n\v\f\r") - first + 1);
}

enum class CoordinateUnit { user, px, pt, pc, mm, cm, in, percent, em, ex };
enum class GradientUnits { user, object };
enum class Align { min, mid, max };
enum class Aspect { none, meet, slice };
struct Coordinate {
    double value = 0;
    CoordinateUnit units = CoordinateUnit::user;
};
struct LinearData {
    Coordinate startX{0, CoordinateUnit::percent}, startY{0, CoordinateUnit::percent};
    Coordinate endX{100, CoordinateUnit::percent}, endY{0, CoordinateUnit::percent};
};
struct RadialData {
    Coordinate centerX{50, CoordinateUnit::percent}, centerY{50, CoordinateUnit::percent};
    Coordinate radius{50, CoordinateUnit::percent}, focusX{}, focusY{};
};
struct GradientData {
    std::string identifier, ref;
    std::variant<LinearData, RadialData> geometry;
    Spread spread = Spread::pad;
    GradientUnits units = GradientUnits::object;
    Transform xform = identity;
    std::vector<GradientStop> stops;
};
struct GradientReference {
    std::string identifier;
};
using PaintSource = std::variant<std::monostate, Color, GradientReference>;
struct Attributes {
    std::string identifier;
    Transform xform = identity;
    PaintSource fill = Color{0}, stroke;
    double opacity = 1, fillOpacity = 1, strokeOpacity = 1;
    double strokeWidth = 1, strokeDashOffset = 0;
    std::vector<double> strokeDashArray;
    LineJoin strokeLineJoin = LineJoin::miter;
    LineCap strokeLineCap = LineCap::butt;
    double miterLimit = 4;
    FillRule fillRule = FillRule::nonzero;
    double fontSize = 0;
    Color stopColor = 0;
    double stopOpacity = 1, stopOffset = 0;
    bool display = true, visible = true;
    std::array<PaintOrder, 3> paintOrder{PaintOrder::fill, PaintOrder::stroke, PaintOrder::markers};
};
struct Attribute {
    std::string_view name, value;
};
struct StyleDeclaration {
    std::string className, propertiesText;
};
struct PendingShape {
    Shape shape;
    double fillOpacity, strokeOpacity;
};

constexpr Transform translation(double offsetX, double offsetY) {
    return {1, 0, 0, 1, offsetX, offsetY};
}
constexpr Transform scaling(double scaleX, double scaleY) {
    return {scaleX, 0, 0, scaleY, 0, 0};
}
static Transform rotation(double angle) {
    const double cosine = std::cos(angle), sine = std::sin(angle);
    return {cosine, sine, -sine, cosine, 0, 0};
}
// Apply first, then second, matching the original SVG transform convention.
constexpr Transform multiply(const Transform &first, const Transform &second) {
    return {first[0] * second[0] + first[1] * second[2],
            first[0] * second[1] + first[1] * second[3],
            first[2] * second[0] + first[3] * second[2],
            first[2] * second[1] + first[3] * second[3],
            first[4] * second[0] + first[5] * second[2] + second[4],
            first[4] * second[1] + first[5] * second[3] + second[5]};
}
constexpr Transform inverse(const Transform &xform) {
    const double det = xform[0] * xform[3] - xform[2] * xform[1];
    if (det > -1e-6 && det < 1e-6) return identity;
    const double invDet = 1 / det;
    return {xform[3] * invDet,
            -xform[1] * invDet,
            -xform[2] * invDet,
            xform[0] * invDet,
            (xform[2] * xform[5] - xform[3] * xform[4]) * invDet,
            (xform[1] * xform[4] - xform[0] * xform[5]) * invDet};
}
constexpr Point transform_point(Point point, const Transform &xform) {
    return {point.x * xform[0] + point.y * xform[2] + xform[4], point.x * xform[1] + point.y * xform[3] + xform[5]};
}
constexpr Point transform_vector(Point vector, const Transform &xform) {
    return {vector.x * xform[0] + vector.y * xform[2], vector.x * xform[1] + vector.y * xform[3]};
}
static double average_scale(const Transform &xform) {
    return std::midpoint(std::hypot(xform[0], xform[2]), std::hypot(xform[1], xform[3]));
}
constexpr Bounds merge_bounds(Bounds bounds, const Bounds &other) {
    for (std::size_t axis = 0; axis < bounds.size(); ++axis)
        bounds[axis] = axis < 2 ? std::min(bounds[axis], other[axis]) : std::max(bounds[axis], other[axis]);
    return bounds;
}
constexpr bool point_in_bounds(Point point, const Bounds &bounds) {
    return point.x >= bounds[0] && point.x <= bounds[2] && point.y >= bounds[1] && point.y <= bounds[3];
}
constexpr double eval_bezier(double ratio, double start, double control1, double control2, double end) {
    const double inverseRatio = 1 - ratio;
    return inverseRatio * inverseRatio * inverseRatio * start +
           3 * inverseRatio * inverseRatio * ratio * control1 +
           3 * inverseRatio * ratio * ratio * control2 + ratio * ratio * ratio * end;
}
static Bounds curve_bounds(std::span<const Point, 4> curve) {
    const auto [start, control1, control2, end] = std::array{curve[0], curve[1], curve[2], curve[3]};
    Bounds bounds{min_value(start.x, end.x), min_value(start.y, end.y), max_value(start.x, end.x), max_value(start.y, end.y)};
    if (point_in_bounds(control1, bounds) && point_in_bounds(control2, bounds)) return bounds;
    for (int axis = 0; axis < 2; ++axis) {
        const auto coords = axis == 0 ? std::array{start.x, control1.x, control2.x, end.x}
                                      : std::array{start.y, control1.y, control2.y, end.y};
        const double quadratic = -3 * coords[0] + 9 * coords[1] - 9 * coords[2] + 3 * coords[3];
        const double linear = 6 * coords[0] - 12 * coords[1] + 6 * coords[2];
        const double constant = 3 * coords[1] - 3 * coords[0];
        std::array<double, 2> roots{};
        std::size_t count = 0;
        if (std::abs(quadratic) < epsilon) {
            if (std::abs(linear) > epsilon) roots[count++] = -constant / linear;
        } else {
            const double discriminant = linear * linear - 4 * constant * quadratic;
            if (discriminant > epsilon) {
                roots[count++] = (-linear + std::sqrt(discriminant)) / (2 * quadratic);
                roots[count++] = (-linear - std::sqrt(discriminant)) / (2 * quadratic);
            }
        }
        for (double ratio : std::span(roots).first(count)) {
            if (!(ratio > epsilon && ratio < 1 - epsilon)) continue;
            const double value = eval_bezier(ratio, coords[0], coords[1], coords[2], coords[3]);
            bounds[axis] = min_value(bounds[axis], value);
            bounds[axis + 2] = max_value(bounds[axis + 2], value);
        }
    }
    return bounds;
}
static Bounds local_bounds(const Shape &shape, const Transform &xform) {
    Bounds bounds{};
    bool first = true;
    for (const auto &path : shape.paths) {
        for (std::size_t segment = 0; segment + 3 < path.points.size(); segment += 3) {
            std::array<Point, 4> curve;
            for (std::size_t pointIndex = 0; pointIndex < curve.size(); ++pointIndex)
                curve[pointIndex] = transform_point(path.points[segment + pointIndex], xform);
            const auto box = curve_bounds(curve);
            bounds = first ? box : merge_bounds(bounds, box);
            first = false;
        }
    }
    return bounds;
}

struct Number {
    double value = 0;
    std::size_t consumed = 0;
    std::errc error{};
};
static Number read_number(std::string_view text) {
    Number result;
    if (text.empty()) {
        result.error = std::errc::invalid_argument;
        return result;
    }
    const bool plus = text.front() == '+';
    if (plus) text.remove_prefix(1);
    if (text.empty()) {
        result.error = std::errc::invalid_argument;
        return result;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result.value);
    result.consumed = static_cast<std::size_t>(end - text.data()) + plus;
    result.error = error;
    if (error == std::errc::result_out_of_range) result.value = std::numeric_limits<double>::quiet_NaN();
    return result;
}
// Retain the SVG token boundary even for malformed numbers such as "1e+".
static Number parse_number(std::string_view text) {
    std::size_t length = 0;
    if (length < text.size() && (text[length] == '+' || text[length] == '-')) ++length;
    while (length < text.size() && is_digit(text[length]))
        ++length;
    if (length < text.size() && text[length] == '.') {
        ++length;
        while (length < text.size() && is_digit(text[length]))
            ++length;
    }
    if (length < text.size() && (text[length] == 'e' || text[length] == 'E') &&
        (length + 1 == text.size() || (text[length + 1] != 'm' && text[length + 1] != 'x'))) {
        ++length;
        if (length < text.size() && (text[length] == '+' || text[length] == '-')) ++length;
        while (length < text.size() && is_digit(text[length]))
            ++length;
    }
    auto result = read_number(text.substr(0, length));
    result.consumed = length;
    return result;
}
constexpr bool is_coordinate(std::string_view text) {
    if (text.starts_with('-') || text.starts_with('+')) text.remove_prefix(1);
    return !text.empty() && (is_digit(text.front()) || text.front() == '.');
}
struct Token {
    std::string_view text;
    std::size_t consumed;
};
static Token next_path_item(std::string_view text, bool arcFlag = false) {
    const auto start = text.find_first_not_of(" \t\n\v\f\r,");
    if (start == text.npos) return {{}, text.size()};
    auto tail = text.substr(start);
    const auto length =
        arcFlag && (tail.front() == '0' || tail.front() == '1')
            ? 1
            : ((tail.front() == '+' || tail.front() == '-' || tail.front() == '.' || is_digit(tail.front()))
                   ? parse_number(tail).consumed
                   : 1);
    return {tail.substr(0, length), start + length};
}
static CoordinateUnit parse_units(std::string_view text) {
    if (text.starts_with("px")) return CoordinateUnit::px;
    if (text.starts_with("pt")) return CoordinateUnit::pt;
    if (text.starts_with("pc")) return CoordinateUnit::pc;
    if (text.starts_with("mm")) return CoordinateUnit::mm;
    if (text.starts_with("cm")) return CoordinateUnit::cm;
    if (text.starts_with("in")) return CoordinateUnit::in;
    if (text.starts_with('%')) return CoordinateUnit::percent;
    if (text.starts_with("em")) return CoordinateUnit::em;
    if (text.starts_with("ex")) return CoordinateUnit::ex;
    return CoordinateUnit::user;
}
static Coordinate parse_coordinate_raw(std::string_view text) {
    const auto number = parse_number(text);
    return {number.value, parse_units(text.substr(number.consumed))};
}
static double convert_to_pixels(Coordinate coord, double origin, double length, double dpi, double fontSize) {
    switch (coord.units) {
    case CoordinateUnit::pt:
        return coord.value / 72 * dpi;
    case CoordinateUnit::pc:
        return coord.value / 6 * dpi;
    case CoordinateUnit::mm:
        return coord.value / 25.4 * dpi;
    case CoordinateUnit::cm:
        return coord.value / 2.54 * dpi;
    case CoordinateUnit::in:
        return coord.value * dpi;
    case CoordinateUnit::em:
        return coord.value * fontSize;
    case CoordinateUnit::ex:
        return coord.value * fontSize * 0.52;
    case CoordinateUnit::percent:
        return origin + coord.value / 100 * length;
    case CoordinateUnit::user:
    case CoordinateUnit::px:
        return coord.value;
    }
    return coord.value;
}
static double parse_opacity(std::string_view text) {
    const double value = read_number(text).value;
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}
static double parse_miter_limit(std::string_view text) {
    const double value = read_number(text).value;
    return std::isfinite(value) ? std::max(value, 0.0) : 0.0;
}
static std::string parse_url(std::string_view text) {
    text.remove_prefix(4);
    if (text.starts_with('#')) text.remove_prefix(1);
    return std::string(text.substr(0, text.find(')')));
}
static Transform parse_transform(std::string_view text) {
    Transform result = identity;
    while (!text.empty()) {
        const auto nameEnd = text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
        std::string_view name;
        for (std::string_view candidate : {"matrix", "translate", "scale", "rotate", "skewX", "skewY"})
            if (text.starts_with(candidate)) {
                name = candidate;
                break;
            }
        if (name.empty()) {
            text.remove_prefix(1);
            continue;
        }
        std::size_t pos = nameEnd == text.npos ? text.size() : nameEnd;
        while (pos < text.size() && is_space(text[pos]))
            ++pos;
        if (pos == text.size() || text[pos] != '(') {
            text.remove_prefix(pos);
            continue;
        }
        ++pos;
        std::array<double, 6> args{};
        std::size_t count = 0;
        const std::size_t maxArgs = name == "matrix"                           ? 6
                                    : name == "rotate"                         ? 3
                                    : (name == "translate" || name == "scale") ? 2
                                                                               : 1;
        bool valid = true;
        while (pos < text.size() && text[pos] != ')') {
            const char character = text[pos];
            if (character == '+' || character == '-' || character == '.' || is_digit(character)) {
                if (count == maxArgs) {
                    valid = false;
                    break;
                }
                const auto number = parse_number(text.substr(pos));
                pos += number.consumed;
                if (number.error != std::errc{} || !std::isfinite(number.value)) {
                    valid = false;
                    break;
                }
                args[count++] = number.value;
            } else if (is_space(character) || character == ',')
                ++pos;
            else {
                valid = false;
                break;
            }
        }
        valid = valid && pos < text.size() && text[pos] == ')';
        if (valid) ++pos;
        text.remove_prefix(pos);
        if (!valid) continue;
        Transform xform = identity;
        if (name == "matrix" && count == 6)
            xform = args;
        else if (name == "translate" && (count == 1 || count == 2))
            xform = translation(args[0], args[1]);
        else if (name == "scale" && (count == 1 || count == 2))
            xform = scaling(args[0], count == 1 ? args[0] : args[1]);
        else if (name == "rotate" && (count == 1 || count == 3)) {
            xform = rotation(args[0] / 180 * circlePi);
            if (count == 3)
                xform = multiply(multiply(translation(-args[1], -args[2]), xform), translation(args[1], args[2]));
        } else if (name == "skewX" && count == 1)
            xform[2] = std::tan(args[0] / 180 * circlePi);
        else if (name == "skewY" && count == 1)
            xform[1] = std::tan(args[0] / 180 * circlePi);
        result = multiply(xform, result);
    }
    return result;
}

struct NamedColor {
    std::string_view name;
    Color color;
};
static constexpr NamedColor named_colors[] = {

    {"red", rgb(255, 0, 0)},
    {"green", rgb(0, 128, 0)},
    {"blue", rgb(0, 0, 255)},
    {"yellow", rgb(255, 255, 0)},
    {"cyan", rgb(0, 255, 255)},
    {"magenta", rgb(255, 0, 255)},
    {"black", rgb(0, 0, 0)},
    {"grey", rgb(128, 128, 128)},
    {"gray", rgb(128, 128, 128)},
    {"white", rgb(255, 255, 255)},

#ifdef NANOSVG_ALL_COLOR_KEYWORDS
    {"aliceblue", rgb(240, 248, 255)},
    {"antiquewhite", rgb(250, 235, 215)},
    {"aqua", rgb(0, 255, 255)},
    {"aquamarine", rgb(127, 255, 212)},
    {"azure", rgb(240, 255, 255)},
    {"beige", rgb(245, 245, 220)},
    {"bisque", rgb(255, 228, 196)},
    {"blanchedalmond", rgb(255, 235, 205)},
    {"blueviolet", rgb(138, 43, 226)},
    {"brown", rgb(165, 42, 42)},
    {"burlywood", rgb(222, 184, 135)},
    {"cadetblue", rgb(95, 158, 160)},
    {"chartreuse", rgb(127, 255, 0)},
    {"chocolate", rgb(210, 105, 30)},
    {"coral", rgb(255, 127, 80)},
    {"cornflowerblue", rgb(100, 149, 237)},
    {"cornsilk", rgb(255, 248, 220)},
    {"crimson", rgb(220, 20, 60)},
    {"darkblue", rgb(0, 0, 139)},
    {"darkcyan", rgb(0, 139, 139)},
    {"darkgoldenrod", rgb(184, 134, 11)},
    {"darkgray", rgb(169, 169, 169)},
    {"darkgreen", rgb(0, 100, 0)},
    {"darkgrey", rgb(169, 169, 169)},
    {"darkkhaki", rgb(189, 183, 107)},
    {"darkmagenta", rgb(139, 0, 139)},
    {"darkolivegreen", rgb(85, 107, 47)},
    {"darkorange", rgb(255, 140, 0)},
    {"darkorchid", rgb(153, 50, 204)},
    {"darkred", rgb(139, 0, 0)},
    {"darksalmon", rgb(233, 150, 122)},
    {"darkseagreen", rgb(143, 188, 143)},
    {"darkslateblue", rgb(72, 61, 139)},
    {"darkslategray", rgb(47, 79, 79)},
    {"darkslategrey", rgb(47, 79, 79)},
    {"darkturquoise", rgb(0, 206, 209)},
    {"darkviolet", rgb(148, 0, 211)},
    {"deeppink", rgb(255, 20, 147)},
    {"deepskyblue", rgb(0, 191, 255)},
    {"dimgray", rgb(105, 105, 105)},
    {"dimgrey", rgb(105, 105, 105)},
    {"dodgerblue", rgb(30, 144, 255)},
    {"firebrick", rgb(178, 34, 34)},
    {"floralwhite", rgb(255, 250, 240)},
    {"forestgreen", rgb(34, 139, 34)},
    {"fuchsia", rgb(255, 0, 255)},
    {"gainsboro", rgb(220, 220, 220)},
    {"ghostwhite", rgb(248, 248, 255)},
    {"gold", rgb(255, 215, 0)},
    {"goldenrod", rgb(218, 165, 32)},
    {"greenyellow", rgb(173, 255, 47)},
    {"honeydew", rgb(240, 255, 240)},
    {"hotpink", rgb(255, 105, 180)},
    {"indianred", rgb(205, 92, 92)},
    {"indigo", rgb(75, 0, 130)},
    {"ivory", rgb(255, 255, 240)},
    {"khaki", rgb(240, 230, 140)},
    {"lavender", rgb(230, 230, 250)},
    {"lavenderblush", rgb(255, 240, 245)},
    {"lawngreen", rgb(124, 252, 0)},
    {"lemonchiffon", rgb(255, 250, 205)},
    {"lightblue", rgb(173, 216, 230)},
    {"lightcoral", rgb(240, 128, 128)},
    {"lightcyan", rgb(224, 255, 255)},
    {"lightgoldenrodyellow", rgb(250, 250, 210)},
    {"lightgray", rgb(211, 211, 211)},
    {"lightgreen", rgb(144, 238, 144)},
    {"lightgrey", rgb(211, 211, 211)},
    {"lightpink", rgb(255, 182, 193)},
    {"lightsalmon", rgb(255, 160, 122)},
    {"lightseagreen", rgb(32, 178, 170)},
    {"lightskyblue", rgb(135, 206, 250)},
    {"lightslategray", rgb(119, 136, 153)},
    {"lightslategrey", rgb(119, 136, 153)},
    {"lightsteelblue", rgb(176, 196, 222)},
    {"lightyellow", rgb(255, 255, 224)},
    {"lime", rgb(0, 255, 0)},
    {"limegreen", rgb(50, 205, 50)},
    {"linen", rgb(250, 240, 230)},
    {"maroon", rgb(128, 0, 0)},
    {"mediumaquamarine", rgb(102, 205, 170)},
    {"mediumblue", rgb(0, 0, 205)},
    {"mediumorchid", rgb(186, 85, 211)},
    {"mediumpurple", rgb(147, 112, 219)},
    {"mediumseagreen", rgb(60, 179, 113)},
    {"mediumslateblue", rgb(123, 104, 238)},
    {"mediumspringgreen", rgb(0, 250, 154)},
    {"mediumturquoise", rgb(72, 209, 204)},
    {"mediumvioletred", rgb(199, 21, 133)},
    {"midnightblue", rgb(25, 25, 112)},
    {"mintcream", rgb(245, 255, 250)},
    {"mistyrose", rgb(255, 228, 225)},
    {"moccasin", rgb(255, 228, 181)},
    {"navajowhite", rgb(255, 222, 173)},
    {"navy", rgb(0, 0, 128)},
    {"oldlace", rgb(253, 245, 230)},
    {"olive", rgb(128, 128, 0)},
    {"olivedrab", rgb(107, 142, 35)},
    {"orange", rgb(255, 165, 0)},
    {"orangered", rgb(255, 69, 0)},
    {"orchid", rgb(218, 112, 214)},
    {"palegoldenrod", rgb(238, 232, 170)},
    {"palegreen", rgb(152, 251, 152)},
    {"paleturquoise", rgb(175, 238, 238)},
    {"palevioletred", rgb(219, 112, 147)},
    {"papayawhip", rgb(255, 239, 213)},
    {"peachpuff", rgb(255, 218, 185)},
    {"peru", rgb(205, 133, 63)},
    {"pink", rgb(255, 192, 203)},
    {"plum", rgb(221, 160, 221)},
    {"powderblue", rgb(176, 224, 230)},
    {"purple", rgb(128, 0, 128)},
    {"rosybrown", rgb(188, 143, 143)},
    {"royalblue", rgb(65, 105, 225)},
    {"saddlebrown", rgb(139, 69, 19)},
    {"salmon", rgb(250, 128, 114)},
    {"sandybrown", rgb(244, 164, 96)},
    {"seagreen", rgb(46, 139, 87)},
    {"seashell", rgb(255, 245, 238)},
    {"sienna", rgb(160, 82, 45)},
    {"silver", rgb(192, 192, 192)},
    {"skyblue", rgb(135, 206, 235)},
    {"slateblue", rgb(106, 90, 205)},
    {"slategray", rgb(112, 128, 144)},
    {"slategrey", rgb(112, 128, 144)},
    {"snow", rgb(255, 250, 250)},
    {"springgreen", rgb(0, 255, 127)},
    {"steelblue", rgb(70, 130, 180)},
    {"tan", rgb(210, 180, 140)},
    {"teal", rgb(0, 128, 128)},
    {"thistle", rgb(216, 191, 216)},
    {"tomato", rgb(255, 99, 71)},
    {"turquoise", rgb(64, 224, 208)},
    {"violet", rgb(238, 130, 238)},
    {"wheat", rgb(245, 222, 179)},
    {"whitesmoke", rgb(245, 245, 245)},
    {"yellowgreen", rgb(154, 205, 50)},
#endif
};

static Color parse_color_hex(std::string_view text) {
    text.remove_prefix(1);
    for (std::size_t width : {2u, 1u}) {
        std::array<Color, 3> channels{};
        auto rest = text;
        bool valid = true;
        for (auto &channel : channels) {
            if (rest.empty()) {
                valid = false;
                break;
            }
            const auto part = rest.substr(0, width);
            const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), channel, 16);
            if (error != std::errc{}) {
                valid = false;
                break;
            }
            rest.remove_prefix(static_cast<std::size_t>(end - part.data()));
        }
        if (valid)
            return width == 2 ? rgb(channels[0], channels[1], channels[2])
                              : rgb(channels[0] * 17, channels[1] * 17, channels[2] * 17);
    }
    return rgb(128, 128, 128);
}
static Color parse_color_rgb(std::string_view text) {
    const auto values = text.substr(4);
    std::array<Color, 3> channels{};
    auto rest = values;
    bool valid = true;
    for (std::size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex) {
        rest = trim(rest);
        const bool negative = rest.starts_with('-');
        if (negative || rest.starts_with('+')) rest.remove_prefix(1);
        if (rest.empty()) {
            valid = false;
            break;
        }
        const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), channels[channelIndex]);
        if (error == std::errc::invalid_argument) {
            valid = false;
            break;
        }
        if (error == std::errc::result_out_of_range)
            channels[channelIndex] = std::numeric_limits<Color>::max();
        else if (negative)
            channels[channelIndex] = Color{0} - channels[channelIndex];
        rest.remove_prefix(static_cast<std::size_t>(end - rest.data()));
        if (channelIndex != 2) {
            if (!rest.starts_with(',')) {
                valid = false;
                break;
            }
            rest.remove_prefix(1);
        }
    }
    if (valid)
        return rgb(std::min(channels[0], Color{255}), std::min(channels[1], Color{255}),
                   std::min(channels[2], Color{255}));
    rest = values;
    for (std::size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex) {
        rest = trim(rest);
        if (rest.starts_with('+')) rest.remove_prefix(1);
        if (rest.empty()) return rgb(128, 128, 128);
        std::size_t length = 0;
        while (length < rest.size() && is_digit(rest[length]))
            ++length;
        if (length < rest.size() && rest[length] == '.') {
            ++length;
            if (length == rest.size() || !is_digit(rest[length])) return rgb(128, 128, 128);
            while (length < rest.size() && is_digit(rest[length]))
                ++length;
        }
        const double value = read_number(rest.substr(0, length)).value;
        rest.remove_prefix(length);
        if (!rest.starts_with('%')) return rgb(128, 128, 128);
        rest.remove_prefix(1);
        rest = trim(rest);
        if (!rest.starts_with(channelIndex == 2 ? ')' : ',')) return rgb(128, 128, 128);
        rest.remove_prefix(1);
        channels[channelIndex] = static_cast<Color>(
            std::round(std::isfinite(value) ? std::clamp(value, 0.0, 100.0) * 255 / 100 : 0));
    }
    return rgb(channels[0], channels[1], channels[2]);
}
static Color parse_color(std::string_view text) {
    while (text.starts_with(' '))
        text.remove_prefix(1);
    if (text.starts_with('#')) return parse_color_hex(text);
    if (text.starts_with("rgb(")) return parse_color_rgb(text);
    for (const auto &color : named_colors)
        if (color.name == text) return color.color;
    return rgb(128, 128, 128);
}

static LineCap parse_line_cap(std::string_view str) {
    if (str == "butt")
        return LineCap::butt;
    else if (str == "round")
        return LineCap::round;
    else if (str == "square")
        return LineCap::square;
    // TODO: handle inherit.
    return LineCap::butt;
}

static LineJoin parse_line_join(std::string_view str) {
    if (str == "miter")
        return LineJoin::miter;
    else if (str == "round")
        return LineJoin::round;
    else if (str == "bevel")
        return LineJoin::bevel;
    // TODO: handle inherit.
    return LineJoin::miter;
}

static FillRule parse_fill_rule(std::string_view str) {
    if (str == "nonzero")
        return FillRule::nonzero;
    else if (str == "evenodd")
        return FillRule::evenodd;
    // TODO: handle inherit.
    return FillRule::nonzero;
}

static std::array<PaintOrder, 3> parse_paint_order(std::string_view str) {
    if (str == "normal" || str == "fill stroke markers")
        return {PaintOrder::fill, PaintOrder::stroke, PaintOrder::markers};
    else if (str == "fill markers stroke")
        return {PaintOrder::fill, PaintOrder::markers, PaintOrder::stroke};
    else if (str == "markers fill stroke")
        return {PaintOrder::markers, PaintOrder::fill, PaintOrder::stroke};
    else if (str == "markers stroke fill")
        return {PaintOrder::markers, PaintOrder::stroke, PaintOrder::fill};
    else if (str == "stroke fill markers")
        return {PaintOrder::stroke, PaintOrder::fill, PaintOrder::markers};
    else if (str == "stroke markers fill")
        return {PaintOrder::stroke, PaintOrder::markers, PaintOrder::fill};
    // TODO: handle inherit.
    return {PaintOrder::fill, PaintOrder::stroke, PaintOrder::markers};
}

static int get_args_per_element(char cmd) {
    switch (cmd) {
    case 'v':
    case 'V':
    case 'h':
    case 'H':
        return 1;
    case 'm':
    case 'M':
    case 'l':
    case 'L':
    case 't':
    case 'T':
        return 2;
    case 'q':
    case 'Q':
    case 's':
    case 'S':
        return 4;
    case 'c':
    case 'C':
        return 6;
    case 'a':
    case 'A':
        return 7;
    case 'z':
    case 'Z':
        return 0;
    }
    return -1;
}

static double square(double value) {
    return value * value;
}

static double magnitude(double deltaX, double deltaY) {
    return std::hypot(deltaX, deltaY);
}

static double vector_ratio(double fromX, double fromY, double toX, double toY) {
    return (fromX * toX + fromY * toY) / (magnitude(fromX, fromY) * magnitude(toX, toY));
}

static double vector_angle(double fromX, double fromY, double toX, double toY) {
    double cosine = vector_ratio(fromX, fromY, toX, toY);
    if (cosine < -1.0) cosine = -1.0;
    if (cosine > 1.0) cosine = 1.0;
    return ((fromX * toY < fromY * toX) ? -1.0 : 1.0) * acos(cosine);
}

static double view_align(double content, double container, Align type) {
    if (type == Align::min)
        return 0;
    else if (type == Align::max)
        return container - content;
    // mid
    return (container - content) * 0.5;
}

static std::expected<OutputUnit, Error> output_unit(std::string_view units) {
    if (units == "px") return OutputUnit::px;
    if (units == "pt") return OutputUnit::pt;
    if (units == "pc") return OutputUnit::pc;
    if (units == "mm") return OutputUnit::mm;
    if (units == "cm") return OutputUnit::cm;
    if (units == "in") return OutputUnit::in;
    return std::unexpected(Error::invalid_argument);
}
static double output_unit_pixels(OutputUnit units, double dpi) {
    switch (units) {
    case OutputUnit::px:
        return 1;
    case OutputUnit::pt:
        return dpi / 72;
    case OutputUnit::pc:
        return dpi / 6;
    case OutputUnit::mm:
        return dpi / 25.4;
    case OutputUnit::cm:
        return dpi / 2.54;
    case OutputUnit::in:
        return dpi;
    }
    return 0;
}
static bool valid_arguments(std::string_view svg, OutputUnit units, double dpi) {
    return !svg.contains('\0') && std::isfinite(dpi) && dpi > 0 && units >= OutputUnit::px &&
           units <= OutputUnit::in;
}

class Parser {
  public:
    Parser(std::string input, double dpi) : input_(std::move(input)), dpi_(dpi) {}
    Parser(const Parser &) = delete;
    Parser &operator=(const Parser &) = delete;
    Parser(Parser &&) = delete;
    Parser &operator=(Parser &&) = delete;
    std::unique_ptr<Image> run(OutputUnit units) {
        parse_xml();
        create_gradients();
        scale_to_viewbox(units);
        return std::move(image_);
    }

  private:
    const std::string input_;
    const double dpi_;
    std::unique_ptr<Image> image_ = std::make_unique<Image>();
    std::vector<Attributes> attributes_{Attributes{}};
    std::vector<Point> points_;
    std::vector<Path> paths_;
    std::vector<StyleDeclaration> styles_;
    std::vector<GradientData> gradients_;
    std::vector<PendingShape> pending_;
    struct Cursor {
        Point point, control;
    } cursor_;
    double viewMinx_ = 0, viewMiny_ = 0, viewWidth_ = 0, viewHeight_ = 0;
    Align alignX_ = Align::min, alignY_ = Align::min;
    Aspect alignType_ = Aspect::none;
    bool defsFlag_ = false, styleFlag_ = false;
    void parse_xml();
    void parse_element(std::string_view text);
    void reset_path();
    void add_point(double posX, double posY);
    void move_to(double posX, double posY);
    void line_to(double posX, double posY);
    void cubic_bez_to(double cx1, double cy1, double cx2, double cy2, double posX, double posY);
    void push_attr();
    void pop_attr();
    double actual_orig_x() const;
    double actual_orig_y() const;
    double actual_width() const;
    double actual_height() const;
    double actual_length() const;
    double parse_coordinate(std::string_view text, double origin, double length) const;
    std::vector<double> parse_stroke_dash_array(std::string_view text) const;
    void apply_class_styles(std::string_view text);
    void parse_style(std::string_view text);
    void parse_attribs(std::span<const Attribute> attributes);
    bool parse_attr(std::string_view name, std::string_view value);
    void add_path(bool closed);
    void add_shape();
    const GradientData *find_gradient_data(std::string_view identifier) const;
    Paint create_gradient(std::string_view gradientId, const Bounds &bounds, const Transform &xform,
                          double opacity) const;
    void create_gradients();
    void parse_gradient(std::span<const Attribute> attributes, GradientKind kind);
    void parse_gradient_stop(std::span<const Attribute> attributes);
    void content(std::string_view text);
    Bounds image_bounds() const;
    void parse_poly(std::span<const Attribute> attributes, bool closed);
    void parse_svg(std::span<const Attribute> attributes);
    void path_move_to(std::span<const double, 2> args, bool rel);
    void path_line_to(std::span<const double, 2> args, bool rel);
    void path_h_line_to(std::span<const double, 1> args, bool rel);
    void path_v_line_to(std::span<const double, 1> args, bool rel);
    void path_cubic_bez_to(std::span<const double, 6> args, bool rel);
    void path_cubic_bez_short_to(std::span<const double, 4> args, bool rel);
    void path_quad_bez_to(std::span<const double, 4> args, bool rel);
    void path_quad_bez_short_to(std::span<const double, 2> args, bool rel);
    void path_arc_to(std::span<const double, 7> args, bool rel);
    void parse_rect(std::span<const Attribute> attr);
    void parse_circle(std::span<const Attribute> attr);
    void parse_ellipse(std::span<const Attribute> attr);
    void parse_line(std::span<const Attribute> attr);
    void start_element(std::string_view element, std::span<const Attribute> attr);
    void end_element(std::string_view element);
    void parse_path(std::span<const Attribute> attr);
    void scale_to_viewbox(OutputUnit units);
};
void Parser::parse_xml() {
    std::string_view remaining = input_;
    while (!remaining.empty()) {
        const auto open = remaining.find('<');
        if (open == remaining.npos) break;
        content(remaining.substr(0, open));
        remaining.remove_prefix(open + 1);
        const auto close = remaining.find('>');
        if (close == remaining.npos) break;
        parse_element(remaining.substr(0, close));
        remaining.remove_prefix(close + 1);
    }
}

void Parser::parse_element(std::string_view text) {
    text = trim(text);
    const bool end = text.starts_with('/');
    if (end) text.remove_prefix(1);
    if (text.empty() || text.starts_with('?') || text.starts_with('!')) return;
    const auto nameEnd = text.find_first_of(" \t\n\v\f\r");
    auto name = text.substr(0, nameEnd);
    text.remove_prefix(name.size());
    bool selfClosing = false;
    std::array<Attribute, 127> attributes;
    std::size_t count = 0;
    while (!end && !text.empty() && count < attributes.size()) {
        text = trim(text);
        if (text.empty()) break;
        if (text.starts_with('/')) {
            selfClosing = true;
            break;
        }
        const auto keyEnd = text.find_first_of(" \t\n\v\f\r=");
        const auto key = text.substr(0, keyEnd);
        text.remove_prefix(key.size());
        const auto quoteStart = text.find_first_of("\"'");
        if (quoteStart == text.npos) break;
        const char quote = text[quoteStart];
        text.remove_prefix(quoteStart + 1);
        const auto valueEnd = text.find(quote);
        const auto value = text.substr(0, valueEnd);
        text.remove_prefix(value.size());
        if (!text.empty()) text.remove_prefix(1);
        attributes[count++] = {key, value};
    }
    if (!end) start_element(name, std::span(attributes).first(count));
    if (end || selfClosing) end_element(name);
}

void Parser::reset_path() {
    points_.clear();
}
void Parser::add_point(double posX, double posY) {
    if (points_.size() == static_cast<std::size_t>(std::numeric_limits<int>::max() / 2))
        throw std::length_error("path too large");
    points_.push_back({posX, posY});
}
void Parser::move_to(double posX, double posY) {
    if (points_.empty())
        add_point(posX, posY);
    else
        points_.back() = {posX, posY};
}
void Parser::line_to(double posX, double posY) {
    if (points_.empty()) return;
    const auto start = points_.back();
    const double deltaX = posX - start.x, deltaY = posY - start.y;
    add_point(start.x + deltaX / 3, start.y + deltaY / 3);
    add_point(posX - deltaX / 3, posY - deltaY / 3);
    add_point(posX, posY);
}
void Parser::cubic_bez_to(double cx1, double cy1, double cx2, double cy2, double posX, double posY) {
    if (points_.empty()) return;
    add_point(cx1, cy1);
    add_point(cx2, cy2);
    add_point(posX, posY);
}
void Parser::push_attr() {
    // Copy before growth: no reference into the vector may survive reallocation.
    auto next = attributes_.back();
    attributes_.push_back(std::move(next));
}
void Parser::pop_attr() {
    if (attributes_.size() > 1) attributes_.pop_back();
}
double Parser::actual_orig_x() const {
    return viewMinx_;
}
double Parser::actual_orig_y() const {
    return viewMiny_;
}
double Parser::actual_width() const {
    return viewWidth_ != 0 ? viewWidth_ : image_->width;
}
double Parser::actual_height() const {
    return viewHeight_ != 0 ? viewHeight_ : image_->height;
}
double Parser::actual_length() const {
    return std::hypot(actual_width(), actual_height()) / std::numbers::sqrt2;
}
double Parser::parse_coordinate(std::string_view text, double origin, double length) const {
    return convert_to_pixels(parse_coordinate_raw(text), origin, length, dpi_, attributes_.back().fontSize);
}

std::vector<double> Parser::parse_stroke_dash_array(std::string_view text) const {
    std::vector<double> dashes;
    if (text.starts_with('n')) return dashes;
    double sum = 0;
    while (!text.empty()) {
        const auto start = text.find_first_not_of(" \t\n\v\f\r,");
        if (start == text.npos) break;
        text.remove_prefix(start);
        const auto end = text.find_first_of(" \t\n\v\f\r,");
        const auto token = text.substr(0, end);
        if (dashes.size() < 8) {
            const double value = std::abs(parse_coordinate(token, 0, actual_length()));
            dashes.push_back(value);
            sum += value;
        }
        text.remove_prefix(token.size());
    }
    if (sum <= 1e-6) dashes.clear();
    return dashes;
}
void Parser::apply_class_styles(std::string_view text) {
    while (!text.empty()) {
        text = trim(text);
        const auto end = text.find_first_of(" \t\n\v\f\r");
        const auto name = text.substr(0, end);
        for (auto style = styles_.rbegin(); style != styles_.rend(); ++style)
            if (style->className == name) parse_style(style->propertiesText);
        text.remove_prefix(name.size());
    }
}
void Parser::parse_style(std::string_view text) {
    while (!text.empty()) {
        const auto separator = text.find(';');
        const auto declaration = trim(text.substr(0, separator));
        const auto colon = declaration.find(':');
        if (colon != declaration.npos) {
            const auto name = trim(declaration.substr(0, colon));
            // Class selectors belong to element attributes, not CSS declarations.
            if (!name.empty() && name != "class") parse_attr(name, trim(declaration.substr(colon + 1)));
        }
        if (separator == text.npos) break;
        text.remove_prefix(separator + 1);
    }
}
void Parser::parse_attribs(std::span<const Attribute> attributes) {
    for (const auto &[name, value] : attributes)
        parse_attr(name, value);
}
bool Parser::parse_attr(std::string_view name, std::string_view value) {
    auto &attr = attributes_.back();
    if (name == "style")
        parse_style(value);
    else if (name == "display") {
        if (value == "none") attr.display = false;
    } else if (name == "visibility") {
        if (value == "hidden" || value == "collapse")
            attr.visible = false;
        else if (value == "visible")
            attr.visible = true;
    } else if (name == "fill" || name == "stroke") {
        auto &paint = name == "fill" ? attr.fill : attr.stroke;
        if (value == "none")
            paint = std::monostate{};
        else if (value.starts_with("url("))
            paint = GradientReference{parse_url(value)};
        else
            paint = parse_color(value);
    } else if (name == "opacity")
        attr.opacity = parse_opacity(value);
    else if (name == "fill-opacity")
        attr.fillOpacity = parse_opacity(value);
    else if (name == "stroke-opacity")
        attr.strokeOpacity = parse_opacity(value);
    else if (name == "stroke-width")
        attr.strokeWidth = parse_coordinate(value, 0, actual_length());
    else if (name == "stroke-dasharray")
        attr.strokeDashArray = parse_stroke_dash_array(value);
    else if (name == "stroke-dashoffset")
        attr.strokeDashOffset = parse_coordinate(value, 0, actual_length());
    else if (name == "stroke-linecap")
        attr.strokeLineCap = parse_line_cap(value);
    else if (name == "stroke-linejoin")
        attr.strokeLineJoin = parse_line_join(value);
    else if (name == "stroke-miterlimit")
        attr.miterLimit = parse_miter_limit(value);
    else if (name == "fill-rule")
        attr.fillRule = parse_fill_rule(value);
    else if (name == "font-size")
        attr.fontSize = parse_coordinate(value, 0, actual_length());
    else if (name == "transform")
        attr.xform = multiply(parse_transform(value), attr.xform);
    else if (name == "stop-color")
        attr.stopColor = parse_color(value);
    else if (name == "stop-opacity")
        attr.stopOpacity = parse_opacity(value);
    else if (name == "offset")
        attr.stopOffset = parse_coordinate(value, 0, 1);
    else if (name == "paint-order")
        attr.paintOrder = parse_paint_order(value);
    else if (name == "id")
        attr.identifier = value;
    else if (name == "class")
        apply_class_styles(value);
    else
        return false;
    return true;
}

void Parser::add_path(bool closed) {
    if (points_.size() < 4) return;
    if (closed) line_to(points_.front().x, points_.front().y);
    if (points_.size() % 3 != 1) return;
    Path path;
    path.closed = closed;
    path.points.reserve(points_.size());
    for (Point point : points_) {
        point = transform_point(point, attributes_.back().xform);
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return;
        path.points.push_back(point);
    }
    for (std::size_t segment = 0; segment + 3 < path.points.size(); segment += 3) {
        const auto bounds = curve_bounds(std::span<const Point, 4>(path.points.data() + segment, 4));
        path.bounds = segment == 0 ? bounds : merge_bounds(path.bounds, bounds);
    }
    paths_.push_back(std::move(path));
}
void Parser::add_shape() {
    if (paths_.empty()) return;
    const auto &attr = attributes_.back();
    Shape shape;
    shape.id = attr.identifier;
    shape.xform = attr.xform;
    const double scale = average_scale(attr.xform);
    shape.strokeWidth = attr.strokeWidth * scale;
    shape.strokeDashOffset = attr.strokeDashOffset * scale;
    shape.strokeDashArray.reserve(attr.strokeDashArray.size());
    for (double dash : attr.strokeDashArray)
        shape.strokeDashArray.push_back(dash * scale);
    shape.strokeLineJoin = attr.strokeLineJoin;
    shape.strokeLineCap = attr.strokeLineCap;
    shape.miterLimit = attr.miterLimit;
    shape.fillRule = attr.fillRule;
    shape.opacity = attr.opacity;
    shape.paintOrder = attr.paintOrder;
    shape.paths = std::move(paths_);
    paths_.clear();
    // Preserve the path order exposed by the original parser.
    std::ranges::reverse(shape.paths);
    shape.bounds = shape.paths.front().bounds;
    for (const auto &path : shape.paths)
        shape.bounds = merge_bounds(shape.bounds, path.bounds);
    if (const auto *color = std::get_if<Color>(&attr.fill))
        shape.fill = Color(*color | (static_cast<Color>(attr.fillOpacity * 255) << 24));
    if (const auto *color = std::get_if<Color>(&attr.stroke))
        shape.stroke = Color(*color | (static_cast<Color>(attr.strokeOpacity * 255) << 24));
    if (const auto *ref = std::get_if<GradientReference>(&attr.fill)) shape.fillGradient = ref->identifier;
    if (const auto *ref = std::get_if<GradientReference>(&attr.stroke)) shape.strokeGradient = ref->identifier;
    shape.visible = attr.display && attr.visible;
    pending_.push_back({std::move(shape), attr.fillOpacity, attr.strokeOpacity});
}

const GradientData *Parser::find_gradient_data(std::string_view identifier) const {
    if (!identifier.empty())
        for (auto gradient = gradients_.rbegin(); gradient != gradients_.rend(); ++gradient)
            if (gradient->identifier == identifier) return &*gradient;
    return nullptr;
}
Paint Parser::create_gradient(std::string_view gradientId, const Bounds &bounds, const Transform &xform,
                              double opacity) const {
    const auto *data = find_gradient_data(gradientId);
    if (!data) return {};
    const std::vector<GradientStop> *stops = nullptr;
    const auto *ref = data;
    for (int depth = 0; ref && depth <= 32; ++depth) {
        if (!ref->stops.empty()) {
            stops = &ref->stops;
            break;
        }
        ref = find_gradient_data(ref->ref);
    }
    if (!stops) return {};
    const bool object = data->units == GradientUnits::object;
    const double originX = object ? bounds[0] : actual_orig_x();
    const double originY = object ? bounds[1] : actual_orig_y();
    const double width = object ? bounds[2] - bounds[0] : actual_width();
    const double height = object ? bounds[3] - bounds[1] : actual_height();
    const double length = std::hypot(width, height) / std::numbers::sqrt2;
    const auto pixels = [object, dpi = dpi_, font = attributes_.back().fontSize](
                            Coordinate coord, double origin, double extent) {
        return object && coord.units == CoordinateUnit::user ? origin + coord.value * extent
                                                            : convert_to_pixels(coord, origin, extent, dpi, font);
    };
    Gradient gradient;
    if (const auto *line = std::get_if<LinearData>(&data->geometry)) {
        const double startX = pixels(line->startX, originX, width), startY = pixels(line->startY, originY, height);
        const double endX = pixels(line->endX, originX, width), endY = pixels(line->endY, originY, height);
        gradient.xform = {endY - startY, startX - endX, endX - startX, endY - startY, startX, startY};
    } else {
        const auto &radial = std::get<RadialData>(data->geometry);
        const double centerX = pixels(radial.centerX, originX, width), centerY = pixels(radial.centerY, originY, height);
        const double focusX = pixels(radial.focusX, originX, width), focusY = pixels(radial.focusY, originY, height);
        const double radius = pixels(radial.radius, 0, length);
        gradient.kind = GradientKind::radial;
        gradient.xform = {radius, 0, 0, radius, centerX, centerY};
        gradient.fx = radius > 0 ? (focusX - centerX) / radius : 0;
        gradient.fy = radius > 0 ? (focusY - centerY) / radius : 0;
    }
    gradient.xform = multiply(multiply(gradient.xform, data->xform), xform);
    gradient.spread = data->spread;
    gradient.stops = *stops;
    for (auto &stop : gradient.stops)
        stop.color = (stop.color & 0x00ffffff) | (static_cast<Color>((stop.color >> 24) * opacity) << 24);
    return gradient;
}
void Parser::create_gradients() {
    image_->shapes.reserve(pending_.size());
    for (auto &pending : pending_) {
        auto &shape = pending.shape;
        if (!shape.fillGradient.empty() || !shape.strokeGradient.empty()) {
            const auto bounds = local_bounds(shape, inverse(shape.xform));
            if (!shape.fillGradient.empty())
                shape.fill = create_gradient(shape.fillGradient, bounds, shape.xform, pending.fillOpacity);
            if (!shape.strokeGradient.empty())
                shape.stroke =
                    create_gradient(shape.strokeGradient, bounds, shape.xform, pending.strokeOpacity);
        }
        image_->shapes.push_back(std::move(shape));
    }
    pending_.clear();
}
void Parser::parse_gradient(std::span<const Attribute> attributes, GradientKind kind) {
    GradientData gradient;
    if (kind == GradientKind::radial) gradient.geometry = RadialData{};
    for (const auto &[name, value] : attributes) {
        if (name == "id")
            gradient.identifier = value;
        else if (!parse_attr(name, value)) {
            if (name == "gradientUnits")
                gradient.units = value == "objectBoundingBox" ? GradientUnits::object : GradientUnits::user;
            else if (name == "gradientTransform")
                gradient.xform = parse_transform(value);
            else if (name == "spreadMethod") {
                if (value == "pad")
                    gradient.spread = Spread::pad;
                else if (value == "reflect")
                    gradient.spread = Spread::reflect;
                else if (value == "repeat")
                    gradient.spread = Spread::repeat;
            } else if (name == "xlink:href")
                gradient.ref = value.empty() ? value : value.substr(1);
            else if (auto *line = std::get_if<LinearData>(&gradient.geometry)) {
                if (name == "x1")
                    line->startX = parse_coordinate_raw(value);
                else if (name == "y1")
                    line->startY = parse_coordinate_raw(value);
                else if (name == "x2")
                    line->endX = parse_coordinate_raw(value);
                else if (name == "y2")
                    line->endY = parse_coordinate_raw(value);
            } else {
                auto &radial = std::get<RadialData>(gradient.geometry);
                if (name == "cx")
                    radial.centerX = parse_coordinate_raw(value);
                else if (name == "cy")
                    radial.centerY = parse_coordinate_raw(value);
                else if (name == "r")
                    radial.radius = parse_coordinate_raw(value);
                else if (name == "fx")
                    radial.focusX = parse_coordinate_raw(value);
                else if (name == "fy")
                    radial.focusY = parse_coordinate_raw(value);
            }
        }
    }
    gradients_.push_back(std::move(gradient));
}
void Parser::parse_gradient_stop(std::span<const Attribute> attributes) {
    auto &attr = attributes_.back();
    attr.stopOffset = 0;
    attr.stopColor = 0;
    attr.stopOpacity = 1;
    parse_attribs(attributes);
    if (gradients_.empty()) return;
    auto &stops = gradients_.back().stops;
    GradientStop stop{attr.stopColor | (static_cast<Color>(attr.stopOpacity * 255) << 24), attr.stopOffset};
    if (!std::isfinite(stop.offset)) stop.offset = 0;
    const auto pos = std::ranges::upper_bound(stops, stop.offset, {}, &GradientStop::offset);
    stops.insert(pos, stop);
}
void Parser::content(std::string_view text) {
    if (!styleFlag_) return;
    text = trim(text);
    while (!text.empty()) {
        const auto open = text.find('{');
        if (open == text.npos) break;
        auto selectors = text.substr(0, open);
        text.remove_prefix(open + 1);
        const auto close = text.find('}');
        const auto properties = text.substr(0, close);
        std::size_t count = 0;
        while (!selectors.empty()) {
            const auto start = selectors.find_first_not_of(" \t\n\v\f\r,");
            if (start == selectors.npos) break;
            selectors.remove_prefix(start);
            const auto end = selectors.find_first_of(" \t\n\v\f\r,");
            const auto name = selectors.substr(0, end);
            if (name.starts_with('.') && count < 32) {
                styles_.push_back({std::string(name.substr(1)), std::string(properties)});
                ++count;
            }
            selectors.remove_prefix(name.size());
        }
        if (close == text.npos) break;
        text.remove_prefix(close + 1);
    }
}
Bounds Parser::image_bounds() const {
    Bounds bounds{};
    if (!image_->shapes.empty()) {
        bounds = image_->shapes.front().bounds;
        for (const auto &shape : image_->shapes)
            bounds = merge_bounds(bounds, shape.bounds);
    }
    return bounds;
}
void Parser::parse_poly(std::span<const Attribute> attributes, bool closed) {
    reset_path();
    for (const auto &[name, value] : attributes) {
        if (parse_attr(name, value) || name != "points") continue;
        auto text = value;
        std::array<double, 2> args{};
        std::size_t count = 0;
        bool first = true;
        while (!text.empty()) {
            const auto token = next_path_item(text);
            text.remove_prefix(token.consumed);
            args[count++] = read_number(token.text).value;
            if (count == args.size()) {
                if (first)
                    move_to(args[0], args[1]);
                else
                    line_to(args[0], args[1]);
                count = 0;
                first = false;
            }
        }
    }
    add_path(closed);
    add_shape();
}
void Parser::parse_svg(std::span<const Attribute> attributes) {
    for (const auto &[name, value] : attributes) {
        if (parse_attr(name, value)) continue;
        if (name == "width")
            image_->width = parse_coordinate(value, 0, 0);
        else if (name == "height")
            image_->height = parse_coordinate(value, 0, 0);
        else if (name == "viewBox") {
            auto text = value;
            for (double *component : {&viewMinx_, &viewMiny_, &viewWidth_, &viewHeight_}) {
                const auto number = parse_number(text);
                *component = number.value;
                text.remove_prefix(number.consumed);
                const auto next = text.find_first_not_of(" \t\n\v\f\r%,");
                if (next == text.npos) break;
                text.remove_prefix(next);
            }
        } else if (name == "preserveAspectRatio") {
            if (value.contains("none"))
                alignType_ = Aspect::none;
            else {
                if (value.contains("xMin"))
                    alignX_ = Align::min;
                else if (value.contains("xMid"))
                    alignX_ = Align::mid;
                else if (value.contains("xMax"))
                    alignX_ = Align::max;
                if (value.contains("YMin"))
                    alignY_ = Align::min;
                else if (value.contains("YMid"))
                    alignY_ = Align::mid;
                else if (value.contains("YMax"))
                    alignY_ = Align::max;
                alignType_ = value.contains("slice") ? Aspect::slice : Aspect::meet;
            }
        }
    }
}

void Parser::path_move_to(std::span<const double, 2> args, bool rel) {
    if (rel) {
        cursor_.point.x += args[0];
        cursor_.point.y += args[1];
    } else {
        cursor_.point.x = args[0];
        cursor_.point.y = args[1];
    }
    move_to(cursor_.point.x, cursor_.point.y);
}

void Parser::path_line_to(std::span<const double, 2> args, bool rel) {
    if (rel) {
        cursor_.point.x += args[0];
        cursor_.point.y += args[1];
    } else {
        cursor_.point.x = args[0];
        cursor_.point.y = args[1];
    }
    line_to(cursor_.point.x, cursor_.point.y);
}

void Parser::path_h_line_to(std::span<const double, 1> args, bool rel) {
    if (rel)
        cursor_.point.x += args[0];
    else
        cursor_.point.x = args[0];
    line_to(cursor_.point.x, cursor_.point.y);
}

void Parser::path_v_line_to(std::span<const double, 1> args, bool rel) {
    if (rel)
        cursor_.point.y += args[0];
    else
        cursor_.point.y = args[0];
    line_to(cursor_.point.x, cursor_.point.y);
}

void Parser::path_cubic_bez_to(std::span<const double, 6> args, bool rel) {
    double endX, endY, cx1, cy1, cx2, cy2;

    if (rel) {
        cx1 = cursor_.point.x + args[0];
        cy1 = cursor_.point.y + args[1];
        cx2 = cursor_.point.x + args[2];
        cy2 = cursor_.point.y + args[3];
        endX = cursor_.point.x + args[4];
        endY = cursor_.point.y + args[5];
    } else {
        cx1 = args[0];
        cy1 = args[1];
        cx2 = args[2];
        cy2 = args[3];
        endX = args[4];
        endY = args[5];
    }

    cubic_bez_to(cx1, cy1, cx2, cy2, endX, endY);

    cursor_.control.x = cx2;
    cursor_.control.y = cy2;
    cursor_.point.x = endX;
    cursor_.point.y = endY;
}

void Parser::path_cubic_bez_short_to(std::span<const double, 4> args, bool rel) {
    double startX, startY, endX, endY, cx1, cy1, cx2, cy2;

    startX = cursor_.point.x;
    startY = cursor_.point.y;
    if (rel) {
        cx2 = cursor_.point.x + args[0];
        cy2 = cursor_.point.y + args[1];
        endX = cursor_.point.x + args[2];
        endY = cursor_.point.y + args[3];
    } else {
        cx2 = args[0];
        cy2 = args[1];
        endX = args[2];
        endY = args[3];
    }

    cx1 = 2 * startX - cursor_.control.x;
    cy1 = 2 * startY - cursor_.control.y;

    cubic_bez_to(cx1, cy1, cx2, cy2, endX, endY);

    cursor_.control.x = cx2;
    cursor_.control.y = cy2;
    cursor_.point.x = endX;
    cursor_.point.y = endY;
}

void Parser::path_quad_bez_to(std::span<const double, 4> args, bool rel) {
    double startX, startY, endX, endY, controlX, controlY;
    double cx1, cy1, cx2, cy2;

    startX = cursor_.point.x;
    startY = cursor_.point.y;
    if (rel) {
        controlX = cursor_.point.x + args[0];
        controlY = cursor_.point.y + args[1];
        endX = cursor_.point.x + args[2];
        endY = cursor_.point.y + args[3];
    } else {
        controlX = args[0];
        controlY = args[1];
        endX = args[2];
        endY = args[3];
    }

    // Convert to cubic bezier
    cx1 = startX + 2.0 / 3.0 * (controlX - startX);
    cy1 = startY + 2.0 / 3.0 * (controlY - startY);
    cx2 = endX + 2.0 / 3.0 * (controlX - endX);
    cy2 = endY + 2.0 / 3.0 * (controlY - endY);

    cubic_bez_to(cx1, cy1, cx2, cy2, endX, endY);

    cursor_.control.x = controlX;
    cursor_.control.y = controlY;
    cursor_.point.x = endX;
    cursor_.point.y = endY;
}

void Parser::path_quad_bez_short_to(std::span<const double, 2> args, bool rel) {
    double startX, startY, endX, endY, controlX, controlY;
    double cx1, cy1, cx2, cy2;

    startX = cursor_.point.x;
    startY = cursor_.point.y;
    if (rel) {
        endX = cursor_.point.x + args[0];
        endY = cursor_.point.y + args[1];
    } else {
        endX = args[0];
        endY = args[1];
    }

    controlX = 2 * startX - cursor_.control.x;
    controlY = 2 * startY - cursor_.control.y;

    // Convert to cubix bezier
    cx1 = startX + 2.0 / 3.0 * (controlX - startX);
    cy1 = startY + 2.0 / 3.0 * (controlY - startY);
    cx2 = endX + 2.0 / 3.0 * (controlX - endX);
    cy2 = endY + 2.0 / 3.0 * (controlY - endY);

    cubic_bez_to(cx1, cy1, cx2, cy2, endX, endY);

    cursor_.control.x = controlX;
    cursor_.control.y = controlY;
    cursor_.point.x = endX;
    cursor_.point.y = endY;
}

void Parser::path_arc_to(std::span<const double, 7> args, bool rel) {
    // Ported from canvg (https://code.google.com/p/canvg/)
    double radiusX, radiusY, rotx;
    double startX, startY, endX, endY, centerX, centerY, deltaX, deltaY, metric;
    double x1p, y1p, cxp, cyp, centerScale, numerator, denominator;
    double fromX, fromY, toX, toY, startAngle, sweepAngle;
    double posX, posY, tanx, tany, angle, prevX = 0, prevY = 0, ptanx = 0, ptany = 0;
    Transform xform;
    double sinrx, cosrx;
    int largeArc, sweep;
    int segment, ndivs;
    double hda, kappa;

    radiusX = fabs(args[0]);                // x radius
    radiusY = fabs(args[1]);                // y radius
    rotx = args[2] / 180.0 * circlePi;       // x rotation angle
    largeArc = fabs(args[3]) > 1e-6 ? 1 : 0; // Large arc
    sweep = fabs(args[4]) > 1e-6 ? 1 : 0; // Sweep direction
    startX = cursor_.point.x;              // start point
    startY = cursor_.point.y;
    if (rel) { // end point
        endX = cursor_.point.x + args[5];
        endY = cursor_.point.y + args[6];
    } else {
        endX = args[5];
        endY = args[6];
    }
    if (!isfinite(endX) || !isfinite(endY)) return;

    deltaX = startX - endX;
    deltaY = startY - endY;
    metric = std::hypot(deltaX, deltaY);
    if (metric < 1e-6 || radiusX < 1e-6 || radiusY < 1e-6 || !isfinite(metric) || !isfinite(radiusX) || !isfinite(radiusY) ||
        !isfinite(rotx)) {
        // The arc degenerates to a line
        line_to(endX, endY);
        cursor_.point.x = endX;
        cursor_.point.y = endY;
        return;
    }

    sinrx = sin(rotx);
    cosrx = cos(rotx);

    // Convert to center point parameterization.
    // http://www.w3.org/TR/SVG11/implnote.html#ArcImplementationNotes
    // 1) Compute x1', y1'
    x1p = cosrx * deltaX / 2.0 + sinrx * deltaY / 2.0;
    y1p = -sinrx * deltaX / 2.0 + cosrx * deltaY / 2.0;
    metric = square(x1p) / square(radiusX) + square(y1p) / square(radiusY);
    if (metric > 1) {
        metric = sqrt(metric);
        radiusX *= metric;
        radiusY *= metric;
    }
    // 2) Compute cx', cy'
    centerScale = 0.0;
    numerator = square(radiusX) * square(radiusY) - square(radiusX) * square(y1p) - square(radiusY) * square(x1p);
    denominator = square(radiusX) * square(y1p) + square(radiusY) * square(x1p);
    if (numerator < 0.0) numerator = 0.0;
    if (denominator > 0.0) centerScale = sqrt(numerator / denominator);
    if (largeArc == sweep) centerScale = -centerScale;
    cxp = centerScale * radiusX * y1p / radiusY;
    cyp = centerScale * -radiusY * x1p / radiusX;

    // 3) Compute centerX,centerY from cx',cy'
    centerX = (startX + endX) / 2.0 + cosrx * cxp - sinrx * cyp;
    centerY = (startY + endY) / 2.0 + sinrx * cxp + cosrx * cyp;

    // 4) Calculate theta1, and delta theta.
    fromX = (x1p - cxp) / radiusX;
    fromY = (y1p - cyp) / radiusY;
    toX = (-x1p - cxp) / radiusX;
    toY = (-y1p - cyp) / radiusY;
    startAngle = vector_angle(1.0, 0.0, fromX, fromY); // Initial angle
    sweepAngle = vector_angle(fromX, fromY, toX, toY);   // Delta angle

    //	if (vecrat(fromX,fromY,toX,toY) <= -1.0) sweepAngle = circlePi;
    //	if (vecrat(fromX,fromY,toX,toY) >= 1.0) sweepAngle = 0;

    if (sweep == 0 && sweepAngle > 0)
        sweepAngle -= 2 * circlePi;
    else if (sweep == 1 && sweepAngle < 0)
        sweepAngle += 2 * circlePi;

    // Invalid geometry cannot reach the subdivision cast. Tiny swept angles
    // lose precision in (1-cos(hda))/sin(hda); approximate them with a line.
    if (!isfinite(sweepAngle) || !isfinite(startAngle) || !isfinite(centerX) || !isfinite(centerY) || fabs(sweepAngle) < 2e-3) {
        line_to(endX, endY);
        cursor_.point.x = endX;
        cursor_.point.y = endY;
        return;
    }

    // Approximate the arc using cubic spline segments.
    xform[0] = cosrx;
    xform[1] = sinrx;
    xform[2] = -sinrx;
    xform[3] = cosrx;
    xform[4] = centerX;
    xform[5] = centerY;

    // Split arc into max 90 degree segments.
    // The loop assumes an iteration per end point (including start and end), this +1.
    ndivs = static_cast<int>(fabs(sweepAngle) / (circlePi * 0.5) + 1.0);
    hda = (sweepAngle / static_cast<double>(ndivs)) / 2.0;
    kappa = fabs(4.0 / 3.0 * (1.0 - cos(hda)) / sin(hda));
    if (sweepAngle < 0.0) kappa = -kappa;

    for (segment = 0; segment <= ndivs; segment++) {
        angle = startAngle + sweepAngle * (static_cast<double>(segment) / static_cast<double>(ndivs));
        deltaX = cos(angle);
        deltaY = sin(angle);
        const auto position = transform_point({deltaX * radiusX, deltaY * radiusY}, xform);
        posX = position.x;
        posY = position.y; // position
        const auto tangent = transform_vector({-deltaY * radiusX * kappa, deltaX * radiusY * kappa}, xform);
        tanx = tangent.x;
        tany = tangent.y; // tangent
        // Keep exact endpoints when reconstructing them loses precision.
        if (segment == 0) {
            posX = startX;
            posY = startY;
        }
        if (segment == ndivs) {
            posX = endX;
            posY = endY;
        }
        if (segment > 0) cubic_bez_to(prevX + ptanx, prevY + ptany, posX - tanx, posY - tany, posX, posY);
        prevX = posX;
        prevY = posY;
        ptanx = tanx;
        ptany = tany;
    }

    cursor_.point.x = endX;
    cursor_.point.y = endY;
}

void Parser::parse_rect(std::span<const Attribute> attr) {
    double posX = 0.0;
    double posY = 0.0;
    double width = 0.0;
    double height = 0.0;
    double radiusX = -1.0; // marks not set
    double radiusY = -1.0;

    for (const auto &[name, value] : attr) {
        if (!parse_attr(name, value)) {
            if (name == "x") posX = parse_coordinate(value, actual_orig_x(), actual_width());
            if (name == "y") posY = parse_coordinate(value, actual_orig_y(), actual_height());
            if (name == "width") width = parse_coordinate(value, 0.0, actual_width());
            if (name == "height") height = parse_coordinate(value, 0.0, actual_height());
            if (name == "rx") radiusX = fabs(parse_coordinate(value, 0.0, actual_width()));
            if (name == "ry") radiusY = fabs(parse_coordinate(value, 0.0, actual_height()));
        }
    }

    if (radiusX < 0.0 && radiusY > 0.0) radiusX = radiusY;
    if (radiusY < 0.0 && radiusX > 0.0) radiusY = radiusX;
    if (radiusX < 0.0) radiusX = 0.0;
    if (radiusY < 0.0) radiusY = 0.0;
    if (radiusX > width / 2.0) radiusX = width / 2.0;
    if (radiusY > height / 2.0) radiusY = height / 2.0;

    if (width != 0.0 && height != 0.0) {
        reset_path();

        if (radiusX < 0.00001 || radiusY < 0.0001) {
            move_to(posX, posY);
            line_to(posX + width, posY);
            line_to(posX + width, posY + height);
            line_to(posX, posY + height);
        } else {
            // Rounded rectangle
            move_to(posX + radiusX, posY);
            line_to(posX + width - radiusX, posY);
            cubic_bez_to(posX + width - radiusX * (1 - kappa90), posY,
                         posX + width, posY + radiusY * (1 - kappa90), posX + width, posY + radiusY);
            line_to(posX + width, posY + height - radiusY);
            cubic_bez_to(posX + width, posY + height - radiusY * (1 - kappa90),
                         posX + width - radiusX * (1 - kappa90), posY + height,
                         posX + width - radiusX, posY + height);
            line_to(posX + radiusX, posY + height);
            cubic_bez_to(posX + radiusX * (1 - kappa90), posY + height,
                         posX, posY + height - radiusY * (1 - kappa90), posX, posY + height - radiusY);
            line_to(posX, posY + radiusY);
            cubic_bez_to(posX, posY + radiusY * (1 - kappa90), posX + radiusX * (1 - kappa90), posY, posX + radiusX, posY);
        }

        add_path(1);

        add_shape();
    }
}

void Parser::parse_circle(std::span<const Attribute> attr) {
    double centerX = 0.0;
    double centerY = 0.0;
    double radius = 0.0;

    for (const auto &[name, value] : attr) {
        if (!parse_attr(name, value)) {
            if (name == "cx") centerX = parse_coordinate(value, actual_orig_x(), actual_width());
            if (name == "cy") centerY = parse_coordinate(value, actual_orig_y(), actual_height());
            if (name == "r") radius = fabs(parse_coordinate(value, 0.0, actual_length()));
        }
    }

    if (radius > 0.0) {
        reset_path();

        move_to(centerX + radius, centerY);
        cubic_bez_to(centerX + radius, centerY + radius * kappa90, centerX + radius * kappa90,
            centerY + radius, centerX, centerY + radius);
        cubic_bez_to(centerX - radius * kappa90, centerY + radius, centerX - radius,
            centerY + radius * kappa90, centerX - radius, centerY);
        cubic_bez_to(centerX - radius, centerY - radius * kappa90, centerX - radius * kappa90,
            centerY - radius, centerX, centerY - radius);
        cubic_bez_to(centerX + radius * kappa90, centerY - radius, centerX + radius,
            centerY - radius * kappa90, centerX + radius, centerY);

        add_path(1);

        add_shape();
    }
}

void Parser::parse_ellipse(std::span<const Attribute> attr) {
    double centerX = 0.0;
    double centerY = 0.0;
    double radiusX = 0.0;
    double radiusY = 0.0;

    for (const auto &[name, value] : attr) {
        if (!parse_attr(name, value)) {
            if (name == "cx") centerX = parse_coordinate(value, actual_orig_x(), actual_width());
            if (name == "cy") centerY = parse_coordinate(value, actual_orig_y(), actual_height());
            if (name == "rx") radiusX = fabs(parse_coordinate(value, 0.0, actual_width()));
            if (name == "ry") radiusY = fabs(parse_coordinate(value, 0.0, actual_height()));
        }
    }

    if (radiusX > 0.0 && radiusY > 0.0) {

        reset_path();

        move_to(centerX + radiusX, centerY);
        cubic_bez_to(centerX + radiusX, centerY + radiusY * kappa90, centerX + radiusX * kappa90,
            centerY + radiusY, centerX, centerY + radiusY);
        cubic_bez_to(centerX - radiusX * kappa90, centerY + radiusY, centerX - radiusX,
            centerY + radiusY * kappa90, centerX - radiusX, centerY);
        cubic_bez_to(centerX - radiusX, centerY - radiusY * kappa90, centerX - radiusX * kappa90,
            centerY - radiusY, centerX, centerY - radiusY);
        cubic_bez_to(centerX + radiusX * kappa90, centerY - radiusY, centerX + radiusX,
            centerY - radiusY * kappa90, centerX + radiusX, centerY);

        add_path(1);

        add_shape();
    }
}

void Parser::parse_line(std::span<const Attribute> attr) {
    double startX = 0.0;
    double startY = 0.0;
    double endX = 0.0;
    double endY = 0.0;

    for (const auto &[name, value] : attr) {
        if (!parse_attr(name, value)) {
            if (name == "x1") startX = parse_coordinate(value, actual_orig_x(), actual_width());
            if (name == "y1") startY = parse_coordinate(value, actual_orig_y(), actual_height());
            if (name == "x2") endX = parse_coordinate(value, actual_orig_x(), actual_width());
            if (name == "y2") endY = parse_coordinate(value, actual_orig_y(), actual_height());
        }
    }

    reset_path();

    move_to(startX, startY);
    line_to(endX, endY);

    add_path(0);

    add_shape();
}

void Parser::start_element(std::string_view element, std::span<const Attribute> attr) {

    if (defsFlag_) {
        // Skip everything but gradients and styles in defs
        if (element == "linearGradient") {
            parse_gradient(attr, GradientKind::linear);
        } else if (element == "radialGradient") {
            parse_gradient(attr, GradientKind::radial);
        } else if (element == "stop") {
            parse_gradient_stop(attr);
        } else if (element == "style") {
            styleFlag_ = true;
        }
        return;
    }

    if (element == "g") {
        push_attr();
        parse_attribs(attr);
    } else if (element == "path") {
        push_attr();
        parse_path(attr);
        pop_attr();
    } else if (element == "rect") {
        push_attr();
        parse_rect(attr);
        pop_attr();
    } else if (element == "circle") {
        push_attr();
        parse_circle(attr);
        pop_attr();
    } else if (element == "ellipse") {
        push_attr();
        parse_ellipse(attr);
        pop_attr();
    } else if (element == "line") {
        push_attr();
        parse_line(attr);
        pop_attr();
    } else if (element == "polyline") {
        push_attr();
        parse_poly(attr, 0);
        pop_attr();
    } else if (element == "polygon") {
        push_attr();
        parse_poly(attr, 1);
        pop_attr();
    } else if (element == "linearGradient") {
        parse_gradient(attr, GradientKind::linear);
    } else if (element == "radialGradient") {
        parse_gradient(attr, GradientKind::radial);
    } else if (element == "stop") {
        parse_gradient_stop(attr);
    } else if (element == "defs") {
        defsFlag_ = true;
    } else if (element == "svg") {
        parse_svg(attr);
    } else if (element == "style") {
        styleFlag_ = true;
    }
}

void Parser::end_element(std::string_view element) {

    if (element == "g") {
        pop_attr();
    } else if (element == "defs") {
        defsFlag_ = false;
    } else if (element == "style") {
        styleFlag_ = false;
    }
}

void Parser::parse_path(std::span<const Attribute> attr) {
    std::string_view pathData;
    char cmd = '\0';
    std::array<double, 10> args{};
    int nargs;
    int rargs = 0;
    bool initPoint;
    bool closedFlag;

    for (const auto &[name, value] : attr) {
        if (name == "d") {
            pathData = value;
        } else {
            parse_attr(name, value);
        }
    }

    if (!pathData.empty()) {
        reset_path();
        cursor_.point.x = 0;
        cursor_.point.y = 0;
        cursor_.control.x = 0;
        cursor_.control.y = 0;
        initPoint = false;
        closedFlag = false;
        nargs = 0;

        while (!pathData.empty()) {
            const auto token = next_path_item(pathData, (cmd == 'A' || cmd == 'a') && (nargs == 3 || nargs == 4));
            pathData.remove_prefix(token.consumed);
            const auto item = token.text;
            if (item.empty()) break;
            if (cmd != '\0' && is_coordinate(item)) {
                if (nargs < 10) args[nargs++] = read_number(item).value;
                if (nargs >= rargs) {
                    switch (cmd) {
                    case 'm':
                    case 'M':
                        path_move_to(std::span(args).first<2>(), cmd == 'm');
                        // Moveto can be followed by multiple coordinate pairs,
                        // which should be treated as linetos.
                        cmd = (cmd == 'm') ? 'l' : 'L';
                        rargs = get_args_per_element(cmd);
                        cursor_.control.x = cursor_.point.x;
                        cursor_.control.y = cursor_.point.y;
                        initPoint = true;
                        break;
                    case 'l':
                    case 'L':
                        path_line_to(std::span(args).first<2>(), cmd == 'l');
                        cursor_.control.x = cursor_.point.x;
                        cursor_.control.y = cursor_.point.y;
                        break;
                    case 'H':
                    case 'h':
                        path_h_line_to(std::span(args).first<1>(), cmd == 'h');
                        cursor_.control.x = cursor_.point.x;
                        cursor_.control.y = cursor_.point.y;
                        break;
                    case 'V':
                    case 'v':
                        path_v_line_to(std::span(args).first<1>(), cmd == 'v');
                        cursor_.control.x = cursor_.point.x;
                        cursor_.control.y = cursor_.point.y;
                        break;
                    case 'C':
                    case 'c':
                        path_cubic_bez_to(std::span(args).first<6>(), cmd == 'c');
                        break;
                    case 'S':
                    case 's':
                        path_cubic_bez_short_to(std::span(args).first<4>(), cmd == 's');
                        break;
                    case 'Q':
                    case 'q':
                        path_quad_bez_to(std::span(args).first<4>(), cmd == 'q');
                        break;
                    case 'T':
                    case 't':
                        path_quad_bez_short_to(std::span(args).first<2>(), cmd == 't');
                        break;
                    case 'A':
                    case 'a':
                        path_arc_to(std::span(args).first<7>(), cmd == 'a');
                        cursor_.control.x = cursor_.point.x;
                        cursor_.control.y = cursor_.point.y;
                        break;
                    default:
                        if (nargs >= 2) {
                            cursor_.point.x = args[nargs - 2];
                            cursor_.point.y = args[nargs - 1];
                            cursor_.control.x = cursor_.point.x;
                            cursor_.control.y = cursor_.point.y;
                        }
                        break;
                    }
                    nargs = 0;
                }
            } else {
                cmd = item[0];
                if (cmd == 'M' || cmd == 'm') {
                    // Commit path.
                    if (!points_.empty()) add_path(closedFlag);
                    // Start new subpath.
                    reset_path();
                    closedFlag = false;
                    nargs = 0;
                } else if (!initPoint) {
                    // Do not allow other commands until initial point has been set (moveTo called once).
                    cmd = '\0';
                }
                if (cmd == 'Z' || cmd == 'z') {
                    closedFlag = true;
                    // Commit path.
                    if (!points_.empty()) {
                        // Move current point to first point
                        cursor_.point.x = points_.front().x;
                        cursor_.point.y = points_.front().y;
                        cursor_.control.x = cursor_.point.x;
                        cursor_.control.y = cursor_.point.y;
                        add_path(closedFlag);
                    }
                    // Start new subpath.
                    reset_path();
                    move_to(cursor_.point.x, cursor_.point.y);
                    closedFlag = false;
                    nargs = 0;
                }
                rargs = get_args_per_element(cmd);
                if (rargs == -1) {
                    // Command not recognized
                    cmd = '\0';
                    rargs = 0;
                }
            }
        }
        // Commit path.
        if (!points_.empty()) add_path(closedFlag);
    }

    add_shape();
}

void Parser::scale_to_viewbox(OutputUnit units) {
    double offsetX, offsetY, scaleX, scaleY, unitScale, avgs;
    int dashIndex;

    // Guess image size if not set completely.
    const auto bounds = image_bounds();

    if (viewWidth_ == 0) {
        if (image_->width > 0) {
            viewWidth_ = image_->width;
        } else {
            viewMinx_ = bounds[0];
            viewWidth_ = bounds[2] - bounds[0];
        }
    }
    if (viewHeight_ == 0) {
        if (image_->height > 0) {
            viewHeight_ = image_->height;
        } else {
            viewMiny_ = bounds[1];
            viewHeight_ = bounds[3] - bounds[1];
        }
    }
    if (image_->width == 0) image_->width = viewWidth_;
    if (image_->height == 0) image_->height = viewHeight_;

    offsetX = -viewMinx_;
    offsetY = -viewMiny_;
    scaleX = viewWidth_ > 0 ? image_->width / viewWidth_ : 0;
    scaleY = viewHeight_ > 0 ? image_->height / viewHeight_ : 0;
    // Unit scaling
    unitScale = 1.0 / output_unit_pixels(units, dpi_);

    // Fix aspect ratio
    if (scaleX > 0 && scaleY > 0 && alignType_ == Aspect::meet) {
        // fit whole image into viewbox
        scaleX = scaleY = min_value(scaleX, scaleY);
        offsetX += view_align(viewWidth_ * scaleX, image_->width, alignX_) / scaleX;
        offsetY += view_align(viewHeight_ * scaleY, image_->height, alignY_) / scaleY;
    } else if (scaleX > 0 && scaleY > 0 && alignType_ == Aspect::slice) {
        // fill whole viewbox with image
        scaleX = scaleY = max_value(scaleX, scaleY);
        offsetX += view_align(viewWidth_ * scaleX, image_->width, alignX_) / scaleX;
        offsetY += view_align(viewHeight_ * scaleY, image_->height, alignY_) / scaleY;
    }

    // Transform
    scaleX *= unitScale;
    scaleY *= unitScale;
    avgs = std::midpoint(scaleX, scaleY);
    for (auto &shapeValue : image_->shapes) {
        auto *shape = &shapeValue;
        shape->bounds[0] = (shape->bounds[0] + offsetX) * scaleX;
        shape->bounds[1] = (shape->bounds[1] + offsetY) * scaleY;
        shape->bounds[2] = (shape->bounds[2] + offsetX) * scaleX;
        shape->bounds[3] = (shape->bounds[3] + offsetY) * scaleY;
        for (auto &pathValue : shape->paths) {
            auto *path = &pathValue;
            path->bounds[0] = (path->bounds[0] + offsetX) * scaleX;
            path->bounds[1] = (path->bounds[1] + offsetY) * scaleY;
            path->bounds[2] = (path->bounds[2] + offsetX) * scaleX;
            path->bounds[3] = (path->bounds[3] + offsetY) * scaleY;
            for (auto &point : path->points) {
                point.x = (point.x + offsetX) * scaleX;
                point.y = (point.y + offsetY) * scaleY;
            }
        }

        if (auto *gradient = std::get_if<Gradient>(&shape->fill)) {
            gradient->xform =
                inverse(multiply(multiply(gradient->xform, translation(offsetX, offsetY)), scaling(scaleX, scaleY)));
        }
        if (auto *gradient = std::get_if<Gradient>(&shape->stroke)) {
            gradient->xform =
                inverse(multiply(multiply(gradient->xform, translation(offsetX, offsetY)), scaling(scaleX, scaleY)));
        }

        shape->strokeWidth *= avgs;
        shape->strokeDashOffset *= avgs;
        for (dashIndex = 0; dashIndex < static_cast<int>(shape->strokeDashArray.size()); dashIndex++)
            shape->strokeDashArray[dashIndex] *= avgs;
    }
}

} // namespace detail

std::expected<std::unique_ptr<Image>, Error> parse(std::string_view svg, OutputUnit units, double dpi) {
    if (!detail::valid_arguments(svg, units, dpi)) return std::unexpected(Error::invalid_argument);
    try {
        return std::make_unique<detail::Parser>(std::string(svg), dpi)->run(units);
    } catch (const std::bad_alloc &) {
        return std::unexpected(Error::allocation_failure);
    } catch (const std::length_error &) {
        return std::unexpected(Error::size_overflow);
    }
}
std::expected<std::unique_ptr<Image>, Error> parse(std::string_view svg, std::string_view units, double dpi) {
    const auto unit = detail::output_unit(units);
    if (!unit) return std::unexpected(unit.error());
    return parse(svg, *unit, dpi);
}
std::expected<std::unique_ptr<Image>, Error> parse_file(const std::filesystem::path &filename,
                                                        OutputUnit units, double dpi) {
    try {
        if (filename.native().find(typename std::filesystem::path::value_type{}) !=
            std::filesystem::path::string_type::npos)
            return std::unexpected(Error::invalid_argument);
        std::ifstream file(filename, std::ios::binary);
        if (!file) return std::unexpected(Error::io_error);
        std::string input;
        std::array<char, 8192> chunk;
        while (file.read(chunk.data(), chunk.size()) || file.gcount())
            input.append(chunk.data(), static_cast<std::size_t>(file.gcount()));
        if (!file.eof()) return std::unexpected(Error::io_error);
        if (!detail::valid_arguments(input, units, dpi)) return std::unexpected(Error::invalid_argument);
        return std::make_unique<detail::Parser>(std::move(input), dpi)->run(units);
    } catch (const std::bad_alloc &) {
        return std::unexpected(Error::allocation_failure);
    } catch (const std::length_error &) {
        return std::unexpected(Error::size_overflow);
    } catch (const std::filesystem::filesystem_error &) {
        return std::unexpected(Error::io_error);
    }
}
std::expected<std::unique_ptr<Image>, Error> parse_file(const std::filesystem::path &filename,
                                                        std::string_view units, double dpi) {
    const auto unit = detail::output_unit(units);
    if (!unit) return std::unexpected(unit.error());
    return parse_file(filename, *unit, dpi);
}
} // namespace nanosvg
#endif // NANOSVG_IMPLEMENTATION
#endif // NANOSVG_HPP
