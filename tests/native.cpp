// Native API and C adapter acceptance checks; no test framework required.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "nanosvgrast.hpp"
#ifdef NANOSVG_H
#error "The native API must not include the C API"
#endif
#include "nanosvgrast.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <type_traits>

using namespace nanosvg;
static_assert(std::is_same_v<decltype(Point::x), double>);
static_assert(std::is_same_v<decltype(Image::width), double>);
static_assert(std::is_same_v<decltype(GradientStop::offset), double>);
static_assert(std::is_same_v<decltype(NSVGpath::pts), double*>);
static_assert(!std::is_copy_constructible_v<std::unique_ptr<Image>>);
static_assert(std::is_copy_constructible_v<Path>);

static void precision_and_ownership() {
    auto result = parse("<svg width='1000000002' height='16'><path "
        "d='M1000000000.125 1 L1000000000.375 2'/></svg>");
    assert(result && (*result)->width == 1000000002.0);
    auto image = std::move(*result);
    assert(!*result && image);
    const auto& points = image->shapes.front().paths.front().points;
    assert(points.front().x == 1000000000.125);
    assert(points.back().x - points.front().x == 0.25);
    Path copy = image->shapes.front().paths.front();
    copy.points[0].x += 1;
    assert(copy.points[0].x != points[0].x);
    image.reset();
    assert(copy.points.back().x == 1000000000.375);

    auto decimal = parse("<svg width='1' height='1'><path "
        "d='M+0.123456789012345678901234567890 0 L.25 1e-20'/></svg>");
    assert(decimal);
    const auto& path = (*decimal)->shapes.front().paths.front();
    assert(path.points.front().x == 0.123456789012345678901234567890);
    assert(path.points.back().y == 1e-20);
    auto transformed = parse("<svg width='20000000' height='16'><path transform='translate(16777216.25 .125)' "
        "d='M0 1L.5 2'/></svg>");
    assert(transformed);
    const auto& transformedPath = (*transformed)->shapes.front().paths.front();
    assert(transformedPath.points.front().x == 16777216.25);
    assert(transformedPath.points.back().x == 16777216.75);
    assert(transformedPath.points.front().y == 1.125);

    auto gradient = parse("<svg width='20000000' height='16'><defs><linearGradient id='g' gradientUnits='userSpaceOnUse' "
        "x1='16777216.25' x2='16777216.75'><stop stop-color='red'/></linearGradient></defs>"
        "<rect width='16' height='16' fill='url(#g)'/></svg>");
    assert(gradient);
    const auto& paint = std::get<Gradient>((*gradient)->shapes.front().fill);
    assert(paint.xform[1] == 2.0 && paint.xform[5] == -33554432.5);
}

static void errors_and_files(const std::filesystem::path& scratch) {
    assert(parse("") && parse("<svg><path d='M'/></svg>"));
    assert(parse("<svg/>", "bad").error() == Error::invalid_argument);
    assert(parse("<svg/>", "px", 0).error() == Error::invalid_argument);
    assert(parse("<svg/>", "px", std::numeric_limits<double>::infinity()).error() == Error::invalid_argument);
    assert(parse(std::string_view("<svg/>\0tail", 11)).error() == Error::invalid_argument);
    std::string text = "<svg width='17.125' height='8'><rect width='4' height='4'/></svg>";
    auto image = parse(text);
    text.assign(text.size(), '!');
    assert(image && (*image)->width == 17.125 && (*image)->shapes.size() == 1);
    auto file = scratch / "native.svg";
    {
        std::ofstream out(file);
        assert(out);
        out << "<svg width='17.125' height='8'><rect width='4' height='4'/></svg>";
    }
    auto loaded = parse_file(file);
    assert(loaded && (*loaded)->width == (*image)->width);
    std::filesystem::remove(file);
    assert(parse_file(file).error() == Error::io_error);
    {
        std::ofstream out(file);
        assert(out);
    }
    assert(parse_file(file) && (*parse_file(file))->shapes.empty());
    std::filesystem::remove(file);
}

static void rendering_and_c_edits() {
    constexpr auto svg = "<svg width='32' height='32'><defs><linearGradient id='g'>"
        "<stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs>"
        "<rect x='2' y='2' width='8' height='8' fill='red'/>"
        "<rect x='2' y='16' width='28' height='8' fill='url(#g)'/></svg>";
    auto native = parse(svg);
    std::string mutableSvg(svg);
    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> cImage(nsvgParse(mutableSvg.data(), "px", 96), nsvgDelete);
    auto renderer = create_rasterizer();
    std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> cRenderer(nsvgCreateRasterizer(), nsvgDeleteRasterizer);
    assert(native && cImage && renderer && cRenderer);
    constexpr int stride = 32*4+7;
    constexpr std::size_t length = 31*stride+32*4;
    std::vector<unsigned char> a(length+2, 0xcd), b(a);
    auto nativePixels = std::span(a).subspan(1, length);
    assert((*renderer)->rasterize(**native, nativePixels, 32, 32, stride));
    nsvgRasterize(cRenderer.get(), cImage.get(), 0, 0, 1, b.data()+1, 32, 32, stride);
    assert(a == b && a.front() == 0xcd && a.back() == 0xcd);
    for (int y = 0; y < 31; ++y)
        for (int x = 128; x < stride; ++x) assert(a[1+y*stride+x] == 0xcd);
    const auto before = a;
    auto& shape = (*native)->shapes.front();
    shape.fill = Color(0xff00ff00);
    cImage->shapes->fill.color = 0xff00ff00;
    for (auto& point : shape.paths.front().points) point.x += 10;
    auto* cPath = cImage->shapes->paths;
    for (int i = 0; i < cPath->npts; ++i) cPath->pts[i*2] += 10;
    std::get<Gradient>((*native)->shapes[1].fill).stops[0].color = 0xff00ff00;
    cImage->shapes->next->fill.gradient->stops[0].color = 0xff00ff00;
    assert((*renderer)->rasterize(**native, nativePixels, 32, 32, stride));
    nsvgRasterize(cRenderer.get(), cImage.get(), 0, 0, 1, b.data()+1, 32, 32, stride);
    assert(a == b && a != before);

    auto unchanged = a;
    auto bad = (*renderer)->rasterize(**native, nativePixels.first(8), 32, 32, stride);
    assert(!bad && bad.error() == Error::invalid_argument && a == unchanged);
    assert(!(*renderer)->rasterize(**native, nativePixels, 32, 32, 127));
    assert(!(*renderer)->rasterize(**native, nativePixels, -1, 32, stride));
    assert(!(*renderer)->rasterize(**native, nativePixels, 32, 32, stride, 0, 0, 0));
    assert(!(*renderer)->rasterize(**native, nativePixels, 32, 32, stride, NAN));
    assert(!(*renderer)->rasterize(**native, nativePixels, INT_MAX, INT_MAX, INT_MAX));
    assert((*renderer)->rasterize(**native, {}, 0, 32, 0));
    assert(a == unchanged);
    (*native)->shapes.front().paths.front().points.pop_back();
    assert(!(*renderer)->rasterize(**native, nativePixels, 32, 32, stride) && a == unchanged);
    Rasterizer moved = std::move(**renderer);
    assert(!(*renderer)->rasterize(Image{}, nativePixels, 32, 32, stride));
    assert(moved.rasterize(Image{}, nativePixels, 32, 32, stride));
    assert(a.front() == 0xcd && a.back() == 0xcd);
}

static void long_graph() {
    std::string svg = "<svg width='2' height='2'>";
    for (int i = 0; i < 10000; ++i) svg += "<path d='M0 0L1 1'/>";
    svg += "</svg>";
    auto native = parse(svg);
    assert(native && (*native)->shapes.size() == 10000);
    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> cImage(nsvgParse(svg.data(), "px", 96), nsvgDelete);
    assert(cImage);
}

static void typed_units_and_tokens(const std::filesystem::path& scratch) {
    static_assert(!std::is_convertible_v<int, OutputUnit>);
    static_assert(!std::is_convertible_v<LineCap, LineJoin>);
    const std::array units{OutputUnit::px, OutputUnit::pt, OutputUnit::pc, OutputUnit::mm, OutputUnit::cm, OutputUnit::in};
    const std::array names{"px", "pt", "pc", "mm", "cm", "in"};
    const std::string svg = "<svg width='96' height='96'><path d='M0 0L96 96'/></svg>";
    const auto file = scratch / "typed-units.svg";
    { std::ofstream out(file); out << svg; assert(out); }
    for (std::size_t i = 0; i < units.size(); ++i) {
        auto typed = parse(svg, units[i]);
        auto legacy = parse(svg, names[i]);
        auto loaded = parse_file(file, units[i]);
        assert(typed && legacy && loaded);
        assert((*typed)->width == (*legacy)->width);
        assert((*typed)->shapes[0].bounds == (*legacy)->shapes[0].bounds);
        assert((*typed)->shapes[0].bounds == (*loaded)->shapes[0].bounds);
    }
    assert(parse_file(file, static_cast<OutputUnit>(99)).error() == Error::invalid_argument);
    assert(parse_file(file, OutputUnit::px, 0).error() == Error::invalid_argument);
    std::filesystem::remove(file);
    assert(parse_file(file, OutputUnit::px).error() == Error::io_error);
    assert(parse(svg, static_cast<OutputUnit>(-1)).error() == Error::invalid_argument);
    assert(parse(svg, static_cast<OutputUnit>(99)).error() == Error::invalid_argument);
    assert(parse(svg, OutputUnit::px, NAN).error() == Error::invalid_argument);

    const auto number = std::string("1.") + std::string(70, '0') + "e2";
    const auto document = "<svg width='" + number + "' height='100' viewBox='0 0 " + number + " 100'>"
        "<path transform='translate(" + number + ")' stroke='red' stroke-dasharray='" + number + " 2' "
        "d='M" + number + " 0L0 1'/></svg>";
    auto image = parse(document);
    assert(image && (*image)->width == 100);
    const auto& shape = (*image)->shapes.front();
    assert(shape.paths.front().points.front().x == 200);
    assert(shape.strokeDashArray == std::vector<double>({100, 2}));
    auto unitsImage = parse("<svg width='1' height='1'><path font-size='10' stroke='red' stroke-width='2em' d='M+1e-2-.5L.25 1'/></svg>");
    assert(unitsImage && (*unitsImage)->shapes.front().strokeWidth == 20);
    assert((*unitsImage)->shapes.front().paths.front().points.front().x == .01);
    assert((*unitsImage)->shapes.front().paths.front().points.front().y == -.5);

    auto scaled = parse("<svg width='1' height='1'><path transform='scale(1e200)' "
        "stroke='red' stroke-width='1e-200' d='M0 0L1e-200 1e-200'/></svg>");
    assert(scaled && (*scaled)->shapes.front().strokeWidth == 1);
}

static void retained_data_lifetimes() {
    const std::string id(600, 'g');
    std::unique_ptr<Image> retained;
    {
        std::string svg = "<svg width='32' height='32'><defs><linearGradient id='" + id + "'>"
            "<stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs>"
            "<style>.paint{fill:url(#" + id + ")}</style>";
        for (int i = 0; i < 64; ++i) {
            svg += "<defs><linearGradient id='g" + std::to_string(i) + "'><stop stop-color='green'/></linearGradient></defs>";
            svg += "<style>.unused" + std::to_string(i) + "{fill:blue}</style>";
        }
        for (int i = 0; i < 16; ++i) svg += "<g id='" + id + "'>";
        svg += "<rect class='paint' width='32' height='32'/><rect style='fill:url(#" + id + ")' width='32' height='32'/>";
        for (int i = 0; i < 16; ++i) svg += "</g>";
        svg += "</svg>";
        const auto before = svg;
        auto result = parse(std::string_view(svg));
        assert(result && svg == before);
        retained = std::move(*result);
        assert(!*result);
        svg.assign(svg.size(), '!');
    }
    assert(retained && retained->shapes.size() == 2);
    assert(retained->shapes[0].id == id && retained->shapes[0].fillGradient == id);
    auto& a = std::get<Gradient>(retained->shapes[0].fill);
    auto& b = std::get<Gradient>(retained->shapes[1].fill);
    assert(a.stops.size() == 2 && a.stops[0].color == 0xff0000ff);
    assert(a.xform == b.xform && a.stops.data() != b.stops.data());
    Image copy = *retained;
    a.stops[0].color = 0xff00ff00;
    assert(std::get<Gradient>(copy.shapes[0].fill).stops[0].color == 0xff0000ff);
    retained.reset();
    auto renderer = create_rasterizer();
    std::array<unsigned char, 32*32*4> pixels{};
    const auto before = copy.shapes[0].paths[0];
    assert(renderer && (*renderer)->rasterize(copy, pixels, 32, 32, 128));
    assert(copy.shapes[0].paths[0].bounds == before.bounds);
    for (std::size_t i = 0; i < before.points.size(); ++i) {
        assert(copy.shapes[0].paths[0].points[i].x == before.points[i].x);
        assert(copy.shapes[0].paths[0].points[i].y == before.points[i].y);
    }
}

static void invalid_edited_enums() {
    auto image = parse("<svg width='2' height='2'><defs><linearGradient id='g'><stop stop-color='red'/></linearGradient></defs>"
        "<rect width='2' height='2' fill='url(#g)'/></svg>");
    auto renderer = create_rasterizer();
    assert(image && renderer);
    std::array<unsigned char, 16> pixels;
    pixels.fill(0xcd);
    const auto untouched = pixels;
    const auto rejected = [&] {
        const auto result = (*renderer)->rasterize(**image, pixels, 2, 2, 8);
        assert(!result && result.error() == Error::invalid_argument && pixels == untouched);
    };
    auto& shape = (*image)->shapes[0];
    shape.strokeLineCap = static_cast<LineCap>(99); rejected(); shape.strokeLineCap = LineCap::butt;
    shape.strokeLineJoin = static_cast<LineJoin>(-1); rejected(); shape.strokeLineJoin = LineJoin::miter;
    shape.fillRule = static_cast<FillRule>(99); rejected(); shape.fillRule = FillRule::nonzero;
    shape.paintOrder[0] = static_cast<PaintOrder>(3); rejected(); shape.paintOrder[0] = PaintOrder::fill;
    auto& gradient = std::get<Gradient>(shape.fill);
    gradient.kind = static_cast<GradientKind>(0); rejected(); gradient.kind = GradientKind::linear;
    gradient.spread = static_cast<Spread>(99); rejected(); gradient.spread = Spread::pad;
    assert((*renderer)->rasterize(**image, pixels, 2, 2, 8));

    std::string text = "<svg width='2' height='2'><rect width='2' height='2' fill='red'/></svg>";
    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> cImage(nsvgParse(text.data(), "px", 96), nsvgDelete);
    std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> cRenderer(nsvgCreateRasterizer(), nsvgDeleteRasterizer);
    assert(cImage && cRenderer);
    pixels = untouched;
    cImage->shapes->strokeLineCap = 99;
    nsvgRasterize(cRenderer.get(), cImage.get(), 0, 0, 1, pixels.data(), 2, 2, 8);
    assert(pixels == untouched);
    cImage->shapes->strokeLineCap = NSVG_CAP_BUTT;
    nsvgRasterize(cRenderer.get(), cImage.get(), 0, 0, 1, pixels.data(), 2, 2, 8);
    assert(pixels[0] == 255 && pixels[3] == 255);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    typed_units_and_tokens(argv[1]);
    retained_data_lifetimes();
    invalid_edited_enums();
    precision_and_ownership();
    errors_and_files(argv[1]);
    rendering_and_c_edits();
    long_graph();
    std::puts("NanoSVG native API and C adapter checks passed");
}
