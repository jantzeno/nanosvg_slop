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
inline std::expected<std::size_t, Error> raster_buffer_size(int w, int h, int stride) {
    if (w < 0 || h < 0 || stride < 0) return std::unexpected(Error::invalid_argument);
    if (w == 0 || h == 0) return 0;
    const auto row = static_cast<std::size_t>(w) * 4;
    if (row / 4 != static_cast<std::size_t>(w)) return std::unexpected(Error::size_overflow);
    if (row > static_cast<std::size_t>(stride)) return std::unexpected(Error::invalid_argument);
    const auto rows = static_cast<std::size_t>(h - 1);
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
constexpr double pi = std::numbers::pi;
constexpr double kappa90 = 0.5522847493;
constexpr double epsilon = 1e-12;
constexpr Transform identity{1, 0, 0, 1, 0, 0};
constexpr Color rgb(Color r, Color g, Color b) {
    return r | (g << 8) | (b << 16);
}
constexpr bool is_space(char c) {
    return std::string_view(" \t\n\v\f\r").contains(c);
}
constexpr bool is_digit(char c) {
    return c >= '0' && c <= '9';
}
// Preserve the original operand choice for NaNs and signed zero.
constexpr double min_value(double a, double b) {
    return a < b ? a : b;
}
constexpr double max_value(double a, double b) {
    return a > b ? a : b;
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
    Coordinate x1{0, CoordinateUnit::percent}, y1{0, CoordinateUnit::percent};
    Coordinate x2{100, CoordinateUnit::percent}, y2{0, CoordinateUnit::percent};
};
struct RadialData {
    Coordinate cx{50, CoordinateUnit::percent}, cy{50, CoordinateUnit::percent};
    Coordinate r{50, CoordinateUnit::percent}, fx{}, fy{};
};
struct GradientData {
    std::string id, ref;
    std::variant<LinearData, RadialData> geometry;
    Spread spread = Spread::pad;
    GradientUnits units = GradientUnits::object;
    Transform xform = identity;
    std::vector<GradientStop> stops;
};
struct GradientReference {
    std::string id;
};
using PaintSource = std::variant<std::monostate, Color, GradientReference>;
struct Attributes {
    std::string id;
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

constexpr Transform translation(double x, double y) {
    return {1, 0, 0, 1, x, y};
}
constexpr Transform scaling(double x, double y) {
    return {x, 0, 0, y, 0, 0};
}
static Transform rotation(double a) {
    const double c = std::cos(a), s = std::sin(a);
    return {c, s, -s, c, 0, 0};
}
// Apply t, then s, matching the original SVG transform convention.
constexpr Transform multiply(const Transform &t, const Transform &s) {
    return {t[0] * s[0] + t[1] * s[2], t[0] * s[1] + t[1] * s[3],        t[2] * s[0] + t[3] * s[2],
            t[2] * s[1] + t[3] * s[3], t[4] * s[0] + t[5] * s[2] + s[4], t[4] * s[1] + t[5] * s[3] + s[5]};
}
constexpr Transform inverse(const Transform &t) {
    const double det = t[0] * t[3] - t[2] * t[1];
    if (det > -1e-6 && det < 1e-6) return identity;
    const double r = 1 / det;
    return {t[3] * r,
            -t[1] * r,
            -t[2] * r,
            t[0] * r,
            (t[2] * t[5] - t[3] * t[4]) * r,
            (t[1] * t[4] - t[0] * t[5]) * r};
}
constexpr Point transform_point(Point p, const Transform &t) {
    return {p.x * t[0] + p.y * t[2] + t[4], p.x * t[1] + p.y * t[3] + t[5]};
}
constexpr Point transform_vector(Point p, const Transform &t) {
    return {p.x * t[0] + p.y * t[2], p.x * t[1] + p.y * t[3]};
}
static double average_scale(const Transform &t) {
    return std::midpoint(std::hypot(t[0], t[2]), std::hypot(t[1], t[3]));
}
constexpr Bounds merge_bounds(Bounds a, const Bounds &b) {
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] = i < 2 ? std::min(a[i], b[i]) : std::max(a[i], b[i]);
    return a;
}
constexpr bool point_in_bounds(Point p, const Bounds &b) {
    return p.x >= b[0] && p.x <= b[2] && p.y >= b[1] && p.y <= b[3];
}
constexpr double eval_bezier(double t, double p0, double p1, double p2, double p3) {
    const double it = 1 - t;
    return it * it * it * p0 + 3 * it * it * t * p1 + 3 * it * t * t * p2 + t * t * t * p3;
}
static Bounds curve_bounds(std::span<const Point, 4> curve) {
    const auto [v0, v1, v2, v3] = std::array{curve[0], curve[1], curve[2], curve[3]};
    Bounds bounds{min_value(v0.x, v3.x), min_value(v0.y, v3.y), max_value(v0.x, v3.x), max_value(v0.y, v3.y)};
    if (point_in_bounds(v1, bounds) && point_in_bounds(v2, bounds)) return bounds;
    for (int i = 0; i < 2; ++i) {
        const auto v = i == 0 ? std::array{v0.x, v1.x, v2.x, v3.x} : std::array{v0.y, v1.y, v2.y, v3.y};
        const double a = -3 * v[0] + 9 * v[1] - 9 * v[2] + 3 * v[3];
        const double b = 6 * v[0] - 12 * v[1] + 6 * v[2];
        const double c = 3 * v[1] - 3 * v[0];
        std::array<double, 2> roots{};
        std::size_t count = 0;
        if (std::abs(a) < epsilon) {
            if (std::abs(b) > epsilon) roots[count++] = -c / b;
        } else {
            const double discriminant = b * b - 4 * c * a;
            if (discriminant > epsilon) {
                roots[count++] = (-b + std::sqrt(discriminant)) / (2 * a);
                roots[count++] = (-b - std::sqrt(discriminant)) / (2 * a);
            }
        }
        for (double t : std::span(roots).first(count)) {
            if (!(t > epsilon && t < 1 - epsilon)) continue;
            const double value = eval_bezier(t, v[0], v[1], v[2], v[3]);
            bounds[i] = min_value(bounds[i], value);
            bounds[i + 2] = max_value(bounds[i + 2], value);
        }
    }
    return bounds;
}
static Bounds local_bounds(const Shape &shape, const Transform &xform) {
    Bounds bounds{};
    bool first = true;
    for (const auto &path : shape.paths) {
        for (std::size_t i = 0; i + 3 < path.points.size(); i += 3) {
            std::array<Point, 4> curve;
            for (std::size_t j = 0; j < curve.size(); ++j)
                curve[j] = transform_point(path.points[i + j], xform);
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
    std::size_t n = 0;
    if (n < text.size() && (text[n] == '+' || text[n] == '-')) ++n;
    while (n < text.size() && is_digit(text[n]))
        ++n;
    if (n < text.size() && text[n] == '.') {
        ++n;
        while (n < text.size() && is_digit(text[n]))
            ++n;
    }
    if (n < text.size() && (text[n] == 'e' || text[n] == 'E') &&
        (n + 1 == text.size() || (text[n + 1] != 'm' && text[n + 1] != 'x'))) {
        ++n;
        if (n < text.size() && (text[n] == '+' || text[n] == '-')) ++n;
        while (n < text.size() && is_digit(text[n]))
            ++n;
    }
    auto result = read_number(text.substr(0, n));
    result.consumed = n;
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
    const auto n =
        arcFlag && (tail.front() == '0' || tail.front() == '1')
            ? 1
            : ((tail.front() == '+' || tail.front() == '-' || tail.front() == '.' || is_digit(tail.front()))
                   ? parse_number(tail).consumed
                   : 1);
    return {tail.substr(0, n), start + n};
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
static double convert_to_pixels(Coordinate c, double origin, double length, double dpi, double fontSize) {
    switch (c.units) {
    case CoordinateUnit::pt:
        return c.value / 72 * dpi;
    case CoordinateUnit::pc:
        return c.value / 6 * dpi;
    case CoordinateUnit::mm:
        return c.value / 25.4 * dpi;
    case CoordinateUnit::cm:
        return c.value / 2.54 * dpi;
    case CoordinateUnit::in:
        return c.value * dpi;
    case CoordinateUnit::em:
        return c.value * fontSize;
    case CoordinateUnit::ex:
        return c.value * fontSize * 0.52;
    case CoordinateUnit::percent:
        return origin + c.value / 100 * length;
    case CoordinateUnit::user:
    case CoordinateUnit::px:
        return c.value;
    }
    return c.value;
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
            const char c = text[pos];
            if (c == '+' || c == '-' || c == '.' || is_digit(c)) {
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
            } else if (is_space(c) || c == ',')
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
        Transform t = identity;
        if (name == "matrix" && count == 6)
            t = args;
        else if (name == "translate" && (count == 1 || count == 2))
            t = translation(args[0], args[1]);
        else if (name == "scale" && (count == 1 || count == 2))
            t = scaling(args[0], count == 1 ? args[0] : args[1]);
        else if (name == "rotate" && (count == 1 || count == 3)) {
            t = rotation(args[0] / 180 * pi);
            if (count == 3)
                t = multiply(multiply(translation(-args[1], -args[2]), t), translation(args[1], args[2]));
        } else if (name == "skewX" && count == 1)
            t[2] = std::tan(args[0] / 180 * pi);
        else if (name == "skewY" && count == 1)
            t[1] = std::tan(args[0] / 180 * pi);
        result = multiply(t, result);
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
    for (std::size_t i = 0; i < channels.size(); ++i) {
        rest = trim(rest);
        const bool negative = rest.starts_with('-');
        if (negative || rest.starts_with('+')) rest.remove_prefix(1);
        if (rest.empty()) {
            valid = false;
            break;
        }
        const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), channels[i]);
        if (error == std::errc::invalid_argument) {
            valid = false;
            break;
        }
        if (error == std::errc::result_out_of_range)
            channels[i] = std::numeric_limits<Color>::max();
        else if (negative)
            channels[i] = Color{0} - channels[i];
        rest.remove_prefix(static_cast<std::size_t>(end - rest.data()));
        if (i != 2) {
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
    for (std::size_t i = 0; i < channels.size(); ++i) {
        rest = trim(rest);
        if (rest.starts_with('+')) rest.remove_prefix(1);
        if (rest.empty()) return rgb(128, 128, 128);
        std::size_t n = 0;
        while (n < rest.size() && is_digit(rest[n]))
            ++n;
        if (n < rest.size() && rest[n] == '.') {
            ++n;
            if (n == rest.size() || !is_digit(rest[n])) return rgb(128, 128, 128);
            while (n < rest.size() && is_digit(rest[n]))
                ++n;
        }
        const double value = read_number(rest.substr(0, n)).value;
        rest.remove_prefix(n);
        if (!rest.starts_with('%')) return rgb(128, 128, 128);
        rest.remove_prefix(1);
        rest = trim(rest);
        if (!rest.starts_with(i == 2 ? ')' : ',')) return rgb(128, 128, 128);
        rest.remove_prefix(1);
        channels[i] = static_cast<Color>(
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

static double square(double x) {
    return x * x;
}

static double magnitude(double x, double y) {
    return std::hypot(x, y);
}

static double vector_ratio(double ux, double uy, double vx, double vy) {
    return (ux * vx + uy * vy) / (magnitude(ux, uy) * magnitude(vx, vy));
}

static double vector_angle(double ux, double uy, double vx, double vy) {
    double r = vector_ratio(ux, uy, vx, vy);
    if (r < -1.0) r = -1.0;
    if (r > 1.0) r = 1.0;
    return ((ux * vy < uy * vx) ? -1.0 : 1.0) * acos(r);
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
    void add_point(double x, double y);
    void move_to(double x, double y);
    void line_to(double x, double y);
    void cubic_bez_to(double cx1, double cy1, double cx2, double cy2, double x, double y);
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
    const GradientData *find_gradient_data(std::string_view id) const;
    Paint create_gradient(std::string_view id, const Bounds &bounds, const Transform &xform,
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
    void start_element(std::string_view el, std::span<const Attribute> attr);
    void end_element(std::string_view el);
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
void Parser::add_point(double x, double y) {
    if (points_.size() == static_cast<std::size_t>(std::numeric_limits<int>::max() / 2))
        throw std::length_error("path too large");
    points_.push_back({x, y});
}
void Parser::move_to(double x, double y) {
    if (points_.empty())
        add_point(x, y);
    else
        points_.back() = {x, y};
}
void Parser::line_to(double x, double y) {
    if (points_.empty()) return;
    const auto p = points_.back();
    const double dx = x - p.x, dy = y - p.y;
    add_point(p.x + dx / 3, p.y + dy / 3);
    add_point(x - dx / 3, y - dy / 3);
    add_point(x, y);
}
void Parser::cubic_bez_to(double cx1, double cy1, double cx2, double cy2, double x, double y) {
    if (points_.empty()) return;
    add_point(cx1, cy1);
    add_point(cx2, cy2);
    add_point(x, y);
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
        for (auto it = styles_.rbegin(); it != styles_.rend(); ++it)
            if (it->className == name) parse_style(it->propertiesText);
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
        attr.id = value;
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
    for (std::size_t i = 0; i + 3 < path.points.size(); i += 3) {
        const auto bounds = curve_bounds(std::span<const Point, 4>(path.points.data() + i, 4));
        path.bounds = i == 0 ? bounds : merge_bounds(path.bounds, bounds);
    }
    paths_.push_back(std::move(path));
}
void Parser::add_shape() {
    if (paths_.empty()) return;
    const auto &attr = attributes_.back();
    Shape shape;
    shape.id = attr.id;
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
    if (const auto *ref = std::get_if<GradientReference>(&attr.fill)) shape.fillGradient = ref->id;
    if (const auto *ref = std::get_if<GradientReference>(&attr.stroke)) shape.strokeGradient = ref->id;
    shape.visible = attr.display && attr.visible;
    pending_.push_back({std::move(shape), attr.fillOpacity, attr.strokeOpacity});
}

const GradientData *Parser::find_gradient_data(std::string_view id) const {
    if (!id.empty())
        for (auto it = gradients_.rbegin(); it != gradients_.rend(); ++it)
            if (it->id == id) return &*it;
    return nullptr;
}
Paint Parser::create_gradient(std::string_view id, const Bounds &bounds, const Transform &xform,
                              double opacity) const {
    const auto *data = find_gradient_data(id);
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
    const double ox = object ? bounds[0] : actual_orig_x();
    const double oy = object ? bounds[1] : actual_orig_y();
    const double w = object ? bounds[2] - bounds[0] : actual_width();
    const double h = object ? bounds[3] - bounds[1] : actual_height();
    const double length = std::hypot(w, h) / std::numbers::sqrt2;
    const auto pixels = [object, dpi = dpi_, font = attributes_.back().fontSize](Coordinate c, double origin,
                                                                                 double extent) {
        return object && c.units == CoordinateUnit::user ? origin + c.value * extent
                                                         : convert_to_pixels(c, origin, extent, dpi, font);
    };
    Gradient gradient;
    if (const auto *line = std::get_if<LinearData>(&data->geometry)) {
        const double x1 = pixels(line->x1, ox, w), y1 = pixels(line->y1, oy, h);
        const double x2 = pixels(line->x2, ox, w), y2 = pixels(line->y2, oy, h);
        gradient.xform = {y2 - y1, x1 - x2, x2 - x1, y2 - y1, x1, y1};
    } else {
        const auto &radial = std::get<RadialData>(data->geometry);
        const double cx = pixels(radial.cx, ox, w), cy = pixels(radial.cy, oy, h);
        const double fx = pixels(radial.fx, ox, w), fy = pixels(radial.fy, oy, h);
        const double r = pixels(radial.r, 0, length);
        gradient.kind = GradientKind::radial;
        gradient.xform = {r, 0, 0, r, cx, cy};
        gradient.fx = r > 0 ? (fx - cx) / r : 0;
        gradient.fy = r > 0 ? (fy - cy) / r : 0;
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
            gradient.id = value;
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
                    line->x1 = parse_coordinate_raw(value);
                else if (name == "y1")
                    line->y1 = parse_coordinate_raw(value);
                else if (name == "x2")
                    line->x2 = parse_coordinate_raw(value);
                else if (name == "y2")
                    line->y2 = parse_coordinate_raw(value);
            } else {
                auto &radial = std::get<RadialData>(gradient.geometry);
                if (name == "cx")
                    radial.cx = parse_coordinate_raw(value);
                else if (name == "cy")
                    radial.cy = parse_coordinate_raw(value);
                else if (name == "r")
                    radial.r = parse_coordinate_raw(value);
                else if (name == "fx")
                    radial.fx = parse_coordinate_raw(value);
                else if (name == "fy")
                    radial.fy = parse_coordinate_raw(value);
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
    double x2, y2, cx1, cy1, cx2, cy2;

    if (rel) {
        cx1 = cursor_.point.x + args[0];
        cy1 = cursor_.point.y + args[1];
        cx2 = cursor_.point.x + args[2];
        cy2 = cursor_.point.y + args[3];
        x2 = cursor_.point.x + args[4];
        y2 = cursor_.point.y + args[5];
    } else {
        cx1 = args[0];
        cy1 = args[1];
        cx2 = args[2];
        cy2 = args[3];
        x2 = args[4];
        y2 = args[5];
    }

    cubic_bez_to(cx1, cy1, cx2, cy2, x2, y2);

    cursor_.control.x = cx2;
    cursor_.control.y = cy2;
    cursor_.point.x = x2;
    cursor_.point.y = y2;
}

void Parser::path_cubic_bez_short_to(std::span<const double, 4> args, bool rel) {
    double x1, y1, x2, y2, cx1, cy1, cx2, cy2;

    x1 = cursor_.point.x;
    y1 = cursor_.point.y;
    if (rel) {
        cx2 = cursor_.point.x + args[0];
        cy2 = cursor_.point.y + args[1];
        x2 = cursor_.point.x + args[2];
        y2 = cursor_.point.y + args[3];
    } else {
        cx2 = args[0];
        cy2 = args[1];
        x2 = args[2];
        y2 = args[3];
    }

    cx1 = 2 * x1 - cursor_.control.x;
    cy1 = 2 * y1 - cursor_.control.y;

    cubic_bez_to(cx1, cy1, cx2, cy2, x2, y2);

    cursor_.control.x = cx2;
    cursor_.control.y = cy2;
    cursor_.point.x = x2;
    cursor_.point.y = y2;
}

void Parser::path_quad_bez_to(std::span<const double, 4> args, bool rel) {
    double x1, y1, x2, y2, cx, cy;
    double cx1, cy1, cx2, cy2;

    x1 = cursor_.point.x;
    y1 = cursor_.point.y;
    if (rel) {
        cx = cursor_.point.x + args[0];
        cy = cursor_.point.y + args[1];
        x2 = cursor_.point.x + args[2];
        y2 = cursor_.point.y + args[3];
    } else {
        cx = args[0];
        cy = args[1];
        x2 = args[2];
        y2 = args[3];
    }

    // Convert to cubic bezier
    cx1 = x1 + 2.0 / 3.0 * (cx - x1);
    cy1 = y1 + 2.0 / 3.0 * (cy - y1);
    cx2 = x2 + 2.0 / 3.0 * (cx - x2);
    cy2 = y2 + 2.0 / 3.0 * (cy - y2);

    cubic_bez_to(cx1, cy1, cx2, cy2, x2, y2);

    cursor_.control.x = cx;
    cursor_.control.y = cy;
    cursor_.point.x = x2;
    cursor_.point.y = y2;
}

void Parser::path_quad_bez_short_to(std::span<const double, 2> args, bool rel) {
    double x1, y1, x2, y2, cx, cy;
    double cx1, cy1, cx2, cy2;

    x1 = cursor_.point.x;
    y1 = cursor_.point.y;
    if (rel) {
        x2 = cursor_.point.x + args[0];
        y2 = cursor_.point.y + args[1];
    } else {
        x2 = args[0];
        y2 = args[1];
    }

    cx = 2 * x1 - cursor_.control.x;
    cy = 2 * y1 - cursor_.control.y;

    // Convert to cubix bezier
    cx1 = x1 + 2.0 / 3.0 * (cx - x1);
    cy1 = y1 + 2.0 / 3.0 * (cy - y1);
    cx2 = x2 + 2.0 / 3.0 * (cx - x2);
    cy2 = y2 + 2.0 / 3.0 * (cy - y2);

    cubic_bez_to(cx1, cy1, cx2, cy2, x2, y2);

    cursor_.control.x = cx;
    cursor_.control.y = cy;
    cursor_.point.x = x2;
    cursor_.point.y = y2;
}

void Parser::path_arc_to(std::span<const double, 7> args, bool rel) {
    // Ported from canvg (https://code.google.com/p/canvg/)
    double rx, ry, rotx;
    double x1, y1, x2, y2, cx, cy, dx, dy, d;
    double x1p, y1p, cxp, cyp, s, sa, sb;
    double ux, uy, vx, vy, a1, da;
    double x, y, tanx, tany, a, px = 0, py = 0, ptanx = 0, ptany = 0;
    Transform t;
    double sinrx, cosrx;
    int fa, fs;
    int i, ndivs;
    double hda, kappa;

    rx = fabs(args[0]);                // y radius
    ry = fabs(args[1]);                // x radius
    rotx = args[2] / 180.0 * pi;       // x rotation angle
    fa = fabs(args[3]) > 1e-6 ? 1 : 0; // Large arc
    fs = fabs(args[4]) > 1e-6 ? 1 : 0; // Sweep direction
    x1 = cursor_.point.x;              // start point
    y1 = cursor_.point.y;
    if (rel) { // end point
        x2 = cursor_.point.x + args[5];
        y2 = cursor_.point.y + args[6];
    } else {
        x2 = args[5];
        y2 = args[6];
    }
    if (!isfinite(x2) || !isfinite(y2)) return;

    dx = x1 - x2;
    dy = y1 - y2;
    d = std::hypot(dx, dy);
    if (d < 1e-6 || rx < 1e-6 || ry < 1e-6 || !isfinite(d) || !isfinite(rx) || !isfinite(ry) ||
        !isfinite(rotx)) {
        // The arc degenerates to a line
        line_to(x2, y2);
        cursor_.point.x = x2;
        cursor_.point.y = y2;
        return;
    }

    sinrx = sin(rotx);
    cosrx = cos(rotx);

    // Convert to center point parameterization.
    // http://www.w3.org/TR/SVG11/implnote.html#ArcImplementationNotes
    // 1) Compute x1', y1'
    x1p = cosrx * dx / 2.0 + sinrx * dy / 2.0;
    y1p = -sinrx * dx / 2.0 + cosrx * dy / 2.0;
    d = square(x1p) / square(rx) + square(y1p) / square(ry);
    if (d > 1) {
        d = sqrt(d);
        rx *= d;
        ry *= d;
    }
    // 2) Compute cx', cy'
    s = 0.0;
    sa = square(rx) * square(ry) - square(rx) * square(y1p) - square(ry) * square(x1p);
    sb = square(rx) * square(y1p) + square(ry) * square(x1p);
    if (sa < 0.0) sa = 0.0;
    if (sb > 0.0) s = sqrt(sa / sb);
    if (fa == fs) s = -s;
    cxp = s * rx * y1p / ry;
    cyp = s * -ry * x1p / rx;

    // 3) Compute cx,cy from cx',cy'
    cx = (x1 + x2) / 2.0 + cosrx * cxp - sinrx * cyp;
    cy = (y1 + y2) / 2.0 + sinrx * cxp + cosrx * cyp;

    // 4) Calculate theta1, and delta theta.
    ux = (x1p - cxp) / rx;
    uy = (y1p - cyp) / ry;
    vx = (-x1p - cxp) / rx;
    vy = (-y1p - cyp) / ry;
    a1 = vector_angle(1.0, 0.0, ux, uy); // Initial angle
    da = vector_angle(ux, uy, vx, vy);   // Delta angle

    //	if (vecrat(ux,uy,vx,vy) <= -1.0) da = pi;
    //	if (vecrat(ux,uy,vx,vy) >= 1.0) da = 0;

    if (fs == 0 && da > 0)
        da -= 2 * pi;
    else if (fs == 1 && da < 0)
        da += 2 * pi;

    // Invalid geometry cannot reach the subdivision cast. Tiny swept angles
    // lose precision in (1-cos(hda))/sin(hda); approximate them with a line.
    if (!isfinite(da) || !isfinite(a1) || !isfinite(cx) || !isfinite(cy) || fabs(da) < 2e-3) {
        line_to(x2, y2);
        cursor_.point.x = x2;
        cursor_.point.y = y2;
        return;
    }

    // Approximate the arc using cubic spline segments.
    t[0] = cosrx;
    t[1] = sinrx;
    t[2] = -sinrx;
    t[3] = cosrx;
    t[4] = cx;
    t[5] = cy;

    // Split arc into max 90 degree segments.
    // The loop assumes an iteration per end point (including start and end), this +1.
    ndivs = static_cast<int>(fabs(da) / (pi * 0.5) + 1.0);
    hda = (da / static_cast<double>(ndivs)) / 2.0;
    kappa = fabs(4.0 / 3.0 * (1.0 - cos(hda)) / sin(hda));
    if (da < 0.0) kappa = -kappa;

    for (i = 0; i <= ndivs; i++) {
        a = a1 + da * (static_cast<double>(i) / static_cast<double>(ndivs));
        dx = cos(a);
        dy = sin(a);
        const auto position = transform_point({dx * rx, dy * ry}, t);
        x = position.x;
        y = position.y; // position
        const auto tangent = transform_vector({-dy * rx * kappa, dx * ry * kappa}, t);
        tanx = tangent.x;
        tany = tangent.y; // tangent
        // Keep exact endpoints when reconstructing them loses precision.
        if (i == 0) {
            x = x1;
            y = y1;
        }
        if (i == ndivs) {
            x = x2;
            y = y2;
        }
        if (i > 0) cubic_bez_to(px + ptanx, py + ptany, x - tanx, y - tany, x, y);
        px = x;
        py = y;
        ptanx = tanx;
        ptany = tany;
    }

    cursor_.point.x = x2;
    cursor_.point.y = y2;
}

void Parser::parse_rect(std::span<const Attribute> attr) {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
    double rx = -1.0; // marks not set
    double ry = -1.0;

    for (const auto &[name, value] : attr) {
        if (!parse_attr(name, value)) {
            if (name == "x") x = parse_coordinate(value, actual_orig_x(), actual_width());
            if (name == "y") y = parse_coordinate(value, actual_orig_y(), actual_height());
            if (name == "width") w = parse_coordinate(value, 0.0, actual_width());
            if (name == "height") h = parse_coordinate(value, 0.0, actual_height());
            if (name == "rx") rx = fabs(parse_coordinate(value, 0.0, actual_width()));
            if (name == "ry") ry = fabs(parse_coordinate(value, 0.0, actual_height()));
        }
    }

    if (rx < 0.0 && ry > 0.0) rx = ry;
    if (ry < 0.0 && rx > 0.0) ry = rx;
    if (rx < 0.0) rx = 0.0;
    if (ry < 0.0) ry = 0.0;
    if (rx > w / 2.0) rx = w / 2.0;
    if (ry > h / 2.0) ry = h / 2.0;

    if (w != 0.0 && h != 0.0) {
        reset_path();

        if (rx < 0.00001 || ry < 0.0001) {
            move_to(x, y);
            line_to(x + w, y);
            line_to(x + w, y + h);
            line_to(x, y + h);
        } else {
            // Rounded rectangle
            move_to(x + rx, y);
            line_to(x + w - rx, y);
            cubic_bez_to(x + w - rx * (1 - kappa90), y, x + w, y + ry * (1 - kappa90), x + w, y + ry);
            line_to(x + w, y + h - ry);
            cubic_bez_to(x + w, y + h - ry * (1 - kappa90), x + w - rx * (1 - kappa90), y + h, x + w - rx,
                         y + h);
            line_to(x + rx, y + h);
            cubic_bez_to(x + rx * (1 - kappa90), y + h, x, y + h - ry * (1 - kappa90), x, y + h - ry);
            line_to(x, y + ry);
            cubic_bez_to(x, y + ry * (1 - kappa90), x + rx * (1 - kappa90), y, x + rx, y);
        }

        add_path(1);

        add_shape();
    }
}

void Parser::parse_circle(std::span<const Attribute> attr) {
    double cx = 0.0;
    double cy = 0.0;
    double r = 0.0;

    for (const auto &[name, value] : attr) {
        if (!parse_attr(name, value)) {
            if (name == "cx") cx = parse_coordinate(value, actual_orig_x(), actual_width());
            if (name == "cy") cy = parse_coordinate(value, actual_orig_y(), actual_height());
            if (name == "r") r = fabs(parse_coordinate(value, 0.0, actual_length()));
        }
    }

    if (r > 0.0) {
        reset_path();

        move_to(cx + r, cy);
        cubic_bez_to(cx + r, cy + r * kappa90, cx + r * kappa90, cy + r, cx, cy + r);
        cubic_bez_to(cx - r * kappa90, cy + r, cx - r, cy + r * kappa90, cx - r, cy);
        cubic_bez_to(cx - r, cy - r * kappa90, cx - r * kappa90, cy - r, cx, cy - r);
        cubic_bez_to(cx + r * kappa90, cy - r, cx + r, cy - r * kappa90, cx + r, cy);

        add_path(1);

        add_shape();
    }
}

void Parser::parse_ellipse(std::span<const Attribute> attr) {
    double cx = 0.0;
    double cy = 0.0;
    double rx = 0.0;
    double ry = 0.0;

    for (const auto &[name, value] : attr) {
        if (!parse_attr(name, value)) {
            if (name == "cx") cx = parse_coordinate(value, actual_orig_x(), actual_width());
            if (name == "cy") cy = parse_coordinate(value, actual_orig_y(), actual_height());
            if (name == "rx") rx = fabs(parse_coordinate(value, 0.0, actual_width()));
            if (name == "ry") ry = fabs(parse_coordinate(value, 0.0, actual_height()));
        }
    }

    if (rx > 0.0 && ry > 0.0) {

        reset_path();

        move_to(cx + rx, cy);
        cubic_bez_to(cx + rx, cy + ry * kappa90, cx + rx * kappa90, cy + ry, cx, cy + ry);
        cubic_bez_to(cx - rx * kappa90, cy + ry, cx - rx, cy + ry * kappa90, cx - rx, cy);
        cubic_bez_to(cx - rx, cy - ry * kappa90, cx - rx * kappa90, cy - ry, cx, cy - ry);
        cubic_bez_to(cx + rx * kappa90, cy - ry, cx + rx, cy - ry * kappa90, cx + rx, cy);

        add_path(1);

        add_shape();
    }
}

void Parser::parse_line(std::span<const Attribute> attr) {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;

    for (const auto &[name, value] : attr) {
        if (!parse_attr(name, value)) {
            if (name == "x1") x1 = parse_coordinate(value, actual_orig_x(), actual_width());
            if (name == "y1") y1 = parse_coordinate(value, actual_orig_y(), actual_height());
            if (name == "x2") x2 = parse_coordinate(value, actual_orig_x(), actual_width());
            if (name == "y2") y2 = parse_coordinate(value, actual_orig_y(), actual_height());
        }
    }

    reset_path();

    move_to(x1, y1);
    line_to(x2, y2);

    add_path(0);

    add_shape();
}

void Parser::start_element(std::string_view el, std::span<const Attribute> attr) {

    if (defsFlag_) {
        // Skip everything but gradients and styles in defs
        if (el == "linearGradient") {
            parse_gradient(attr, GradientKind::linear);
        } else if (el == "radialGradient") {
            parse_gradient(attr, GradientKind::radial);
        } else if (el == "stop") {
            parse_gradient_stop(attr);
        } else if (el == "style") {
            styleFlag_ = true;
        }
        return;
    }

    if (el == "g") {
        push_attr();
        parse_attribs(attr);
    } else if (el == "path") {
        push_attr();
        parse_path(attr);
        pop_attr();
    } else if (el == "rect") {
        push_attr();
        parse_rect(attr);
        pop_attr();
    } else if (el == "circle") {
        push_attr();
        parse_circle(attr);
        pop_attr();
    } else if (el == "ellipse") {
        push_attr();
        parse_ellipse(attr);
        pop_attr();
    } else if (el == "line") {
        push_attr();
        parse_line(attr);
        pop_attr();
    } else if (el == "polyline") {
        push_attr();
        parse_poly(attr, 0);
        pop_attr();
    } else if (el == "polygon") {
        push_attr();
        parse_poly(attr, 1);
        pop_attr();
    } else if (el == "linearGradient") {
        parse_gradient(attr, GradientKind::linear);
    } else if (el == "radialGradient") {
        parse_gradient(attr, GradientKind::radial);
    } else if (el == "stop") {
        parse_gradient_stop(attr);
    } else if (el == "defs") {
        defsFlag_ = true;
    } else if (el == "svg") {
        parse_svg(attr);
    } else if (el == "style") {
        styleFlag_ = true;
    }
}

void Parser::end_element(std::string_view el) {

    if (el == "g") {
        pop_attr();
    } else if (el == "defs") {
        defsFlag_ = false;
    } else if (el == "style") {
        styleFlag_ = false;
    }
}

void Parser::parse_path(std::span<const Attribute> attr) {
    std::string_view s;
    char cmd = '\0';
    std::array<double, 10> args{};
    int nargs;
    int rargs = 0;
    bool initPoint;
    bool closedFlag;

    for (const auto &[name, value] : attr) {
        if (name == "d") {
            s = value;
        } else {
            parse_attr(name, value);
        }
    }

    if (!s.empty()) {
        reset_path();
        cursor_.point.x = 0;
        cursor_.point.y = 0;
        cursor_.control.x = 0;
        cursor_.control.y = 0;
        initPoint = false;
        closedFlag = false;
        nargs = 0;

        while (!s.empty()) {
            const auto token = next_path_item(s, (cmd == 'A' || cmd == 'a') && (nargs == 3 || nargs == 4));
            s.remove_prefix(token.consumed);
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
    double tx, ty, sx, sy, us, avgs;
    int i;

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

    tx = -viewMinx_;
    ty = -viewMiny_;
    sx = viewWidth_ > 0 ? image_->width / viewWidth_ : 0;
    sy = viewHeight_ > 0 ? image_->height / viewHeight_ : 0;
    // Unit scaling
    us = 1.0 / output_unit_pixels(units, dpi_);

    // Fix aspect ratio
    if (sx > 0 && sy > 0 && alignType_ == Aspect::meet) {
        // fit whole image into viewbox
        sx = sy = min_value(sx, sy);
        tx += view_align(viewWidth_ * sx, image_->width, alignX_) / sx;
        ty += view_align(viewHeight_ * sy, image_->height, alignY_) / sy;
    } else if (sx > 0 && sy > 0 && alignType_ == Aspect::slice) {
        // fill whole viewbox with image
        sx = sy = max_value(sx, sy);
        tx += view_align(viewWidth_ * sx, image_->width, alignX_) / sx;
        ty += view_align(viewHeight_ * sy, image_->height, alignY_) / sy;
    }

    // Transform
    sx *= us;
    sy *= us;
    avgs = std::midpoint(sx, sy);
    for (auto &shapeValue : image_->shapes) {
        auto *shape = &shapeValue;
        shape->bounds[0] = (shape->bounds[0] + tx) * sx;
        shape->bounds[1] = (shape->bounds[1] + ty) * sy;
        shape->bounds[2] = (shape->bounds[2] + tx) * sx;
        shape->bounds[3] = (shape->bounds[3] + ty) * sy;
        for (auto &pathValue : shape->paths) {
            auto *path = &pathValue;
            path->bounds[0] = (path->bounds[0] + tx) * sx;
            path->bounds[1] = (path->bounds[1] + ty) * sy;
            path->bounds[2] = (path->bounds[2] + tx) * sx;
            path->bounds[3] = (path->bounds[3] + ty) * sy;
            for (auto &point : path->points) {
                point.x = (point.x + tx) * sx;
                point.y = (point.y + ty) * sy;
            }
        }

        if (auto *gradient = std::get_if<Gradient>(&shape->fill)) {
            gradient->xform =
                inverse(multiply(multiply(gradient->xform, translation(tx, ty)), scaling(sx, sy)));
        }
        if (auto *gradient = std::get_if<Gradient>(&shape->stroke)) {
            gradient->xform =
                inverse(multiply(multiply(gradient->xform, translation(tx, ty)), scaling(sx, sy)));
        }

        shape->strokeWidth *= avgs;
        shape->strokeDashOffset *= avgs;
        for (i = 0; i < static_cast<int>(shape->strokeDashArray.size()); i++)
            shape->strokeDashArray[i] *= avgs;
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
