#include "CaptionRender.h"

#include "CaptionGeneration.h"
#include "CaptionLayout.h"
#include "CaptionRenderer.h"
#include "MotionCoreTextFont.h"
#include "MotionRender.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

namespace {

// Owns one Core Foundation reference.
template <typename T>
class CFRef {
public:
    CFRef() = default;
    explicit CFRef(T reference) : reference_(reference) {}
    ~CFRef() {
        if (reference_ != nullptr) CFRelease(reference_);
    }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;
    CFRef(CFRef&& other) noexcept : reference_(other.reference_) {
        other.reference_ = nullptr;
    }
    CFRef& operator=(CFRef&& other) noexcept {
        if (this != &other) {
            if (reference_ != nullptr) CFRelease(reference_);
            reference_ = other.reference_;
            other.reference_ = nullptr;
        }
        return *this;
    }
    T get() const { return reference_; }
    explicit operator bool() const { return reference_ != nullptr; }

private:
    T reference_ = nullptr;
};

CFRef<CFStringRef> MakeString(const std::string& text) {
    return CFRef<CFStringRef>(CFStringCreateWithBytes(
        kCFAllocatorDefault,
        static_cast<const UInt8*>(static_cast<const void*>(text.data())),
        static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false));
}

std::string ToUtf8(CFStringRef text) {
    if (text == nullptr) return {};
    const CFIndex length = CFStringGetLength(text);
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string buffer(static_cast<size_t>(capacity), '\0');
    if (!CFStringGetCString(text, buffer.data(), capacity,
                            kCFStringEncodingUTF8)) {
        return {};
    }
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    return buffer;
}

// The fonts of one call. A family is looked up by the motion measurer, so a
// carton and a composition naming one family draw the same face; one the
// measurer does not find is drawn in the fallback.
class Fonts {
public:
    struct Resolved {
        CTFontRef font = nullptr;
        std::string family;
        std::string face;
        bool substituted = false;
    };

    bool Find(const CaptionStyle& style, double size, Resolved& resolved,
              std::string& error) {
        const std::string family = CaptionFontName(style);
        const auto key = std::make_tuple(family, style.font_weight, size);
        const auto cached = cache_.find(key);
        if (cached != cache_.end()) {
            resolved = cached->second;
            return true;
        }
        // The style's weight, matched as a composition's is: the face CSS
        // would pick, or the wght axis of a variable family
        // (MOTION-WEB-2026-10 MW7). The fallback is asked the same weight.
        MotionTextStyle request;
        request.font_family = family;
        request.font_weight = static_cast<double>(style.font_weight);
        request.font_size = size;
        MotionError missing;
        resolved = {};
        resolved.family = family;
        resolved.font = MotionCoreTextFont(measurer_, request, missing);
        if (resolved.font == nullptr) {
            request.font_family = kCaptionFallbackFamily;
            MotionError fallbackMissing;
            resolved.font =
                MotionCoreTextFont(measurer_, request, fallbackMissing);
            if (resolved.font == nullptr) {
                error = "caption font '" + family + "' and its fallback '" +
                        kCaptionFallbackFamily +
                        "' are both missing: " + fallbackMissing.message;
                return false;
            }
            resolved.family = kCaptionFallbackFamily;
            resolved.substituted = true;
        }
        const CFRef<CFStringRef> name(CTFontCopyPostScriptName(resolved.font));
        resolved.face = ToUtf8(name.get());
        cache_.emplace(key, resolved);
        return true;
    }

private:
    MotionCoreTextMeasurer measurer_;
    std::map<std::tuple<std::string, int32_t, double>, Resolved> cache_;
};

// A line of text in `font`, its colours taken from the context, as the
// motion renderer draws its text (DrawGlyphs, MotionRender.mm).
CFRef<CTLineRef> MakeLine(const std::string& text, CTFontRef font) {
    const CFRef<CFStringRef> string = MakeString(text);
    if (!string) return CFRef<CTLineRef>();
    CFTypeRef keys[] = {kCTFontAttributeName,
                        kCTForegroundColorFromContextAttributeName};
    CFTypeRef values[] = {font, kCFBooleanTrue};
    const CFRef<CFDictionaryRef> attributes(CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    const CFRef<CFAttributedStringRef> attributed(CFAttributedStringCreate(
        kCFAllocatorDefault, string.get(), attributes.get()));
    return CFRef<CTLineRef>(CTLineCreateWithAttributedString(attributed.get()));
}

// The width the line's pen travels, its trailing spaces left out: they put
// no ink on the picture and centre nothing.
double Advance(CTLineRef line) {
    const double width =
        CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) -
        CTLineGetTrailingWhitespaceWidth(line);
    return std::max(0.0, width);
}

bool Measure(const CaptionStyle& style, const std::vector<std::string>& lines,
             std::vector<double>& advances, std::string& error) {
    advances.clear();
    Fonts fonts;
    Fonts::Resolved resolved;
    if (!fonts.Find(style, std::max(1, style.font_size), resolved, error)) {
        return false;
    }
    for (const std::string& text : lines) {
        const CFRef<CTLineRef> line = MakeLine(text, resolved.font);
        if (!line) {
            error = "caption text is not UTF-8";
            return false;
        }
        advances.push_back(Advance(line.get()));
    }
    return true;
}

struct Colour {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 0.0;
};

bool ReadColour(const std::string& text, double opacity, Colour& colour,
                std::string& error) {
    std::array<uint8_t, 4> rgba{};
    if (!ParseCaptionColor(text, rgba)) {
        error = "caption colour cannot be read: " + text;
        return false;
    }
    colour = {rgba[0] / 255.0, rgba[1] / 255.0, rgba[2] / 255.0,
              (rgba[3] / 255.0) * opacity};
    return true;
}

// PA4 -- docs/roadmap/PARITE-2026-11.md. The words of one line, as
// CaptionWordCount counts them: byte ranges between spaces and tabs.
struct WordSpan {
    size_t begin = 0;
    size_t end = 0;
};

std::vector<WordSpan> WordsOf(const std::string& line) {
    std::vector<WordSpan> words;
    size_t index = 0;
    while (index < line.size()) {
        const auto space = [&](size_t at) {
            return line[at] == ' ' || line[at] == '\t' || line[at] == '\r';
        };
        while (index < line.size() && space(index)) ++index;
        if (index >= line.size()) break;
        WordSpan word;
        word.begin = index;
        while (index < line.size() && !space(index)) ++index;
        word.end = index;
        words.push_back(word);
    }
    return words;
}

// Where the pen stands in `line` before byte `byte` of `text`, the text it
// was made from: a piece of the line drawn from there falls on the glyphs
// the whole line would have drawn, the kerning before it included.
double PenAt(CTLineRef line, const std::string& text, size_t byte) {
    const CFRef<CFStringRef> prefix = MakeString(text.substr(0, byte));
    if (!prefix) return 0.0;
    return CTLineGetOffsetForStringIndex(line, CFStringGetLength(prefix.get()),
                                         nullptr);
}

// One carton into `context`, whose origin is the picture's top left.
bool DrawCarton(CGContextRef context, const CaptionDrawing& drawing,
                const CaptionCarton& carton, Fonts& fonts,
                CaptionCartonLayout& layout, std::string& error) {
    const CaptionStyle& style = carton.style;
    const double size =
        caption_layout::FontSize(style, drawing.width, drawing.sequence_width);
    Fonts::Resolved resolved;
    if (!fonts.Find(style, size, resolved, error)) return false;
    layout.family = resolved.family;
    layout.face = resolved.face;
    layout.substituted = resolved.substituted;

    // The whole lines place the carton, animated or not: a carton that
    // fills word by word stands where it will stand full.
    const std::vector<std::string> texts =
        caption_layout::SplitLines(carton.text);
    std::vector<CFRef<CTLineRef>> whole;
    std::vector<double> advances;
    for (const std::string& text : texts) {
        whole.push_back(MakeLine(text, resolved.font));
        if (!whole.back()) {
            error = "caption text is not UTF-8";
            return false;
        }
        advances.push_back(Advance(whole.back().get()));
    }
    // PA4 -- what is drawn of them. "reveal" draws each line up to its
    // last word started; "highlight" draws every line, and the word being
    // said a second time over the letters, in its own colour, cut out of
    // the line so that no edge is drawn twice.
    const bool animated =
        carton.words_spoken >= 0 && !style.word_animation.empty();
    const bool reveal =
        animated && style.word_animation == kCaptionWordAnimationReveal;
    const bool highlight =
        animated && style.word_animation == kCaptionWordAnimationHighlight;
    std::vector<CFRef<CTLineRef>> lines;
    struct Piece {
        CFRef<CTLineRef> line;
        double x = 0.0;
        bool lit = false;
    };
    // The pieces of the line that holds the word being said; `litLine` is
    // that line, or texts.size() when no word is lit.
    std::vector<Piece> pieces;
    size_t litLine = texts.size();
    int32_t counted = 0;
    for (size_t index = 0; index < texts.size(); ++index) {
        const std::string& text = texts[index];
        const std::vector<WordSpan> words = WordsOf(text);
        if (reveal) {
            const int32_t started =
                std::clamp<int32_t>(carton.words_spoken - counted, 0,
                                    static_cast<int32_t>(words.size()));
            const size_t end =
                started == 0 ? 0 : words[static_cast<size_t>(started) - 1].end;
            lines.push_back(MakeLine(text.substr(0, end), resolved.font));
        } else {
            lines.push_back(MakeLine(text, resolved.font));
        }
        if (!lines.back()) {
            error = "caption text is not UTF-8";
            return false;
        }
        const int32_t said = carton.words_spoken - counted;
        if (highlight && said >= 1 &&
            said <= static_cast<int32_t>(words.size())) {
            const WordSpan word = words[static_cast<size_t>(said) - 1];
            litLine = index;
            const auto piece = [&](size_t begin, size_t end, bool lit) {
                Piece made;
                made.line =
                    MakeLine(text.substr(begin, end - begin), resolved.font);
                made.x = PenAt(whole[index].get(), text, begin);
                made.lit = lit;
                pieces.push_back(std::move(made));
            };
            piece(0, word.begin, false);
            piece(word.begin, word.end, true);
            piece(word.end, text.size(), false);
        }
        counted += static_cast<int32_t>(words.size());
    }
    caption_layout::FontMetrics metrics;
    metrics.ascent = CTFontGetAscent(resolved.font);
    metrics.descent = CTFontGetDescent(resolved.font);
    metrics.leading = CTFontGetLeading(resolved.font);
    layout.placement =
        caption_layout::Place(style, drawing.width, drawing.height,
                              drawing.sequence_width, metrics, advances);
    const caption_layout::Placement& placement = layout.placement;

    double opacity = 1.0;
    if (carton.opacity.den > 0) {
        opacity = std::clamp(
            static_cast<double>(carton.opacity.num) / carton.opacity.den, 0.0,
            1.0);
    }
    const auto drawLines = [&](double dx, double dy) {
        for (size_t index = 0; index < lines.size(); ++index) {
            const caption_layout::Line& line = placement.lines[index];
            CGContextSetTextPosition(context, line.x + dx, line.baseline + dy);
            CTLineDraw(lines[index].get(), context);
        }
    };
    const auto setFill = [&](const Colour& colour) {
        CGContextSetRGBFillColor(context, colour.red, colour.green, colour.blue,
                                 colour.alpha);
    };
    const auto setStroke = [&](const Colour& colour) {
        CGContextSetRGBStrokeColor(context, colour.red, colour.green,
                                   colour.blue, colour.alpha);
    };

    if (drawing.glyphs_only) {
        CGContextSetTextDrawingMode(context, kCGTextFill);
        setFill({1.0, 1.0, 1.0, 1.0});
        drawLines(0.0, 0.0);
        return true;
    }
    // drawtext's order: the box, the shadow, the outline, the letters.
    if (placement.box) {
        Colour box;
        if (!ReadColour(style.background_color, opacity, box, error)) {
            return false;
        }
        setFill(box);
        const caption_layout::Rect& rect = placement.box_rect;
        CGContextFillRect(context,
                          CGRectMake(rect.x, rect.y, rect.width, rect.height));
    }
    // The outline is a stroke twice its width, centred on the glyph edge:
    // the letters drawn over it hide the inner half, as drawtext's are
    // drawn over its border. Round joins, as FreeType's stroker draws it.
    CGContextSetLineJoin(context, kCGLineJoinRound);
    CGContextSetLineWidth(context, 2.0 * placement.outline);
    if (placement.shadow > 0.0) {
        Colour shadow;
        if (!ReadColour(style.shadow_color, opacity, shadow, error)) {
            return false;
        }
        setFill(shadow);
        setStroke(shadow);
        CGContextSetTextDrawingMode(
            context, placement.outline > 0.0 ? kCGTextFillStroke : kCGTextFill);
        drawLines(placement.shadow, placement.shadow);
    }
    if (placement.outline > 0.0) {
        Colour outline;
        if (!ReadColour(style.outline_color, opacity, outline, error)) {
            return false;
        }
        setStroke(outline);
        CGContextSetTextDrawingMode(context, kCGTextStroke);
        drawLines(0.0, 0.0);
    }
    Colour fill;
    if (!ReadColour(style.color, opacity, fill, error)) return false;
    CGContextSetTextDrawingMode(context, kCGTextFill);
    if (litLine == texts.size()) {
        setFill(fill);
        drawLines(0.0, 0.0);
        return true;
    }
    // PA4 -- the letters of the line being said, piece by piece.
    Colour lit;
    if (!ReadColour(style.highlight_color, opacity, lit, error)) return false;
    for (size_t index = 0; index < lines.size(); ++index) {
        const caption_layout::Line& line = placement.lines[index];
        if (index != litLine) {
            setFill(fill);
            CGContextSetTextPosition(context, line.x, line.baseline);
            CTLineDraw(lines[index].get(), context);
            continue;
        }
        for (const Piece& piece : pieces) {
            if (!piece.line) continue;
            setFill(piece.lit ? lit : fill);
            CGContextSetTextPosition(context, line.x + piece.x, line.baseline);
            CTLineDraw(piece.line.get(), context);
        }
    }
    return true;
}

bool Draw(const CaptionDrawing& drawing,
          const std::vector<std::vector<CaptionCarton>>& images,
          const std::function<bool(size_t, CaptionImage&)>& sink,
          std::string& error) {
    if (drawing.width <= 0 || drawing.height <= 0 ||
        drawing.sequence_width <= 0) {
        error = "caption picture has no size";
        return false;
    }
    const size_t row = static_cast<size_t>(drawing.width) * 4;
    if (static_cast<size_t>(drawing.height) >
        std::numeric_limits<size_t>::max() / row) {
        error = "caption picture is too large";
        return false;
    }
    // The colours are written as they are given: a caption colour is a
    // code value of the delivered signal, and sRGB on both sides converts
    // nothing.
    const CFRef<CGColorSpaceRef> space(
        CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    Fonts fonts;
    for (size_t index = 0; index < images.size(); ++index) {
        CaptionImage image;
        image.width = drawing.width;
        image.height = drawing.height;
        image.pixels.assign(row * static_cast<size_t>(drawing.height), 0);
        const CFRef<CGContextRef> context(CGBitmapContextCreate(
            image.pixels.data(), static_cast<size_t>(drawing.width),
            static_cast<size_t>(drawing.height), 8, row, space.get(),
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
        if (!context) {
            error = "cannot create the caption picture";
            return false;
        }
        // Top-left origin, y down, as CaptionLayout.h measures; glyphs
        // flipped back upright.
        CGContextTranslateCTM(context.get(), 0.0, drawing.height);
        CGContextScaleCTM(context.get(), 1.0, -1.0);
        CGContextSetTextMatrix(context.get(),
                               CGAffineTransformMakeScale(1.0, -1.0));
        CGContextSetShouldSmoothFonts(context.get(), false);
        CGContextSetAllowsFontSubpixelPositioning(context.get(), true);
        CGContextSetShouldSubpixelPositionFonts(context.get(), true);
        CGContextSetShouldSubpixelQuantizeFonts(context.get(), false);
        for (const CaptionCarton& carton : images[index]) {
            CaptionCartonLayout layout;
            if (!DrawCarton(context.get(), drawing, carton, fonts, layout,
                            error)) {
                return false;
            }
            image.cartons.push_back(std::move(layout));
        }
        CGContextFlush(context.get());
        if (!sink(index, image)) return true;
    }
    return true;
}

bool HasFamily(const std::string& family) {
    MotionCoreTextMeasurer measurer;
    MotionTextStyle request;
    request.font_family = family;
    request.font_weight = 400.0;
    request.font_size = 12.0;
    MotionError missing;
    return MotionCoreTextFont(measurer, request, missing) != nullptr;
}

bool EncodePng(const CaptionImage& image, std::string& png,
               std::string& error) {
    MotionRaster raster;
    raster.width = image.width;
    raster.height = image.height;
    raster.pixels = image.pixels;
    MotionError failure;
    if (!EncodeMotionPng(raster, png, failure)) {
        error = failure.message;
        return false;
    }
    return true;
}

const CaptionRenderer kRenderer{&Measure, &Draw, &HasFamily, &EncodePng};

}  // namespace

void InstallCaptionRenderer() { SetCaptionRenderer(&kRenderer); }
