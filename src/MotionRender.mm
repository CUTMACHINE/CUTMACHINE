#include "MotionRender.h"
#include "MotionCoreTextFont.h"

#include "MotionEffects.h"
#include "MotionGeometry.h"
#include "MotionMedia.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

namespace {

constexpr double kPi = 3.14159265358979323846;

bool RasterSize(int32_t width, int32_t height, size_t& bytes) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    const size_t row = static_cast<size_t>(width) * 4;
    if (static_cast<size_t>(height) >
        static_cast<size_t>(std::numeric_limits<CFIndex>::max()) / row) {
        return false;
    }
    bytes = row * static_cast<size_t>(height);
    return true;
}

// Owns one Core Foundation reference.
template <typename T>
class CFRef {
public:
    CFRef() = default;
    explicit CFRef(T reference) : reference_(reference) {}
    ~CFRef() {
        if (reference_ != nullptr) {
            CFRelease(reference_);
        }
    }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;
    CFRef(CFRef&& other) noexcept : reference_(other.reference_) {
        other.reference_ = nullptr;
    }
    CFRef& operator=(CFRef&& other) noexcept {
        if (this != &other) {
            if (reference_ != nullptr) {
                CFRelease(reference_);
            }
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

bool Fail(MotionError& error, std::string code, std::string path,
          std::string message) {
    error = {std::move(code), std::move(path), std::move(message)};
    return false;
}

CFRef<CFStringRef> MakeString(const std::string& text) {
    return CFRef<CFStringRef>(CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
        static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, 0u));
}

std::string ToUtf8(CFStringRef text) {
    if (text == nullptr) {
        return {};
    }
    const CFIndex length = CFStringGetLength(text);
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string buffer(static_cast<size_t>(capacity), '\0');
    if (CFStringGetCString(text, buffer.data(), capacity,
                           kCFStringEncodingUTF8) == 0u) {
        return {};
    }
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    return buffer;
}

CFRef<CFURLRef> MakeFileUrl(const std::string& path) {
    const CFRef<CFStringRef> text = MakeString(path);
    return CFRef<CFURLRef>(CFURLCreateWithFileSystemPath(
        kCFAllocatorDefault, text.get(), kCFURLPOSIXPathStyle, 0u));
}

// The OS/2 usWeightClass of a static face: the number CSS font-weight
// matching speaks. Core Text's normalised weight trait is only a fallback,
// because its mapping from design weights is not specified.
int WeightClass(CTFontRef font, CTFontDescriptorRef descriptor) {
    const CFRef<CFDataRef> table(
        font != nullptr ? CTFontCopyTable(font, kCTFontTableOS2,
                                          kCTFontTableOptionNoOptions)
                        : nullptr);
    if (table && CFDataGetLength(table.get()) >= 6) {
        const UInt8* bytes = CFDataGetBytePtr(table.get());
        return (bytes[4] << 8) | bytes[5];
    }
    double weight = 0.0;
    const CFRef<CFDictionaryRef> traits(static_cast<CFDictionaryRef>(
        CTFontDescriptorCopyAttribute(descriptor, kCTFontTraitsAttribute)));
    if (traits) {
        const auto number = static_cast<CFNumberRef>(
            CFDictionaryGetValue(traits.get(), kCTFontWeightTrait));
        if (number != nullptr) {
            CFNumberGetValue(number, kCFNumberDoubleType, &weight);
        }
    }
    static const std::pair<double, int> kScale[] = {
        {-0.8, 100}, {-0.6, 200}, {-0.4, 300}, {0.0, 400}, {0.23, 500},
        {0.3, 600},  {0.4, 700},  {0.56, 800}, {0.62, 900}};
    int best = 400;
    double distance = 10.0;
    for (const auto& entry : kScale) {
        if (std::fabs(entry.first - weight) < distance) {
            distance = std::fabs(entry.first - weight);
            best = entry.second;
        }
    }
    return best;
}

// MOTION-WEB-2026-10 MW7 -- ROADMAP.md. The 'wght' variation axis.
constexpr int64_t kWeightAxis = 0x77676874;

// MW7 -- the weights a face covers, as CSS Fonts 4 font matching reads
// them: a static face has one, its OS/2 usWeightClass; a variable face
// covers its wght axis. Every named instance of a variable file carries the
// OS/2 class of the default instance -- Outfit's nine all say Thin, 100 --
// so read from there, any weight asked of a variable family matched the
// instance Core Text lists first, its Regular.
struct FaceWeights {
    int low = 400;
    int high = 400;
    bool variable = false;
    // This descriptor's own coordinate on the axis (its weight class when
    // static): of a variable file's instances, the nearest to the weight
    // drawn is the one the face is made from, so a named instance keeps its
    // name.
    double instance = 400.0;
    // The weight this face is drawn at when `desired` is asked: the nearest
    // it covers (CSS clamps the desired weight to a face's range).
    int Drawn(int desired) const { return std::clamp(desired, low, high); }
};

// MW7 -- the wght axis of `font`, false when it has none.
bool WeightAxis(CTFontRef font, double& minimum, double& maximum,
                double& fallback) {
    const CFRef<CFArrayRef> axes(CTFontCopyVariationAxes(font));
    const CFIndex count = axes ? CFArrayGetCount(axes.get()) : 0;
    for (CFIndex index = 0; index < count; ++index) {
        const auto axis = static_cast<CFDictionaryRef>(
            CFArrayGetValueAtIndex(axes.get(), index));
        const auto identifier = static_cast<CFNumberRef>(
            CFDictionaryGetValue(axis, kCTFontVariationAxisIdentifierKey));
        int64_t tag = 0;
        if (identifier == nullptr ||
            CFNumberGetValue(identifier, kCFNumberSInt64Type, &tag) == 0 ||
            tag != kWeightAxis) {
            continue;
        }
        const auto read = [&](CFStringRef key, double& value) {
            const auto number =
                static_cast<CFNumberRef>(CFDictionaryGetValue(axis, key));
            return number != nullptr &&
                   CFNumberGetValue(number, kCFNumberDoubleType, &value) != 0;
        };
        return read(kCTFontVariationAxisMinimumValueKey, minimum) &&
               read(kCTFontVariationAxisMaximumValueKey, maximum) &&
               read(kCTFontVariationAxisDefaultValueKey, fallback);
    }
    return false;
}

// MW7 -- the coordinate of `font` on the wght axis. A named instance
// carries it in its variation; the default instance carries none, and sits
// at the axis default.
double WeightCoordinate(CTFontRef font, double fallback) {
    const CFRef<CFDictionaryRef> variation(CTFontCopyVariation(font));
    const CFIndex count = variation ? CFDictionaryGetCount(variation.get()) : 0;
    std::vector<const void*> keys(static_cast<size_t>(count));
    std::vector<const void*> values(static_cast<size_t>(count));
    if (count > 0) {
        CFDictionaryGetKeysAndValues(variation.get(), keys.data(),
                                     values.data());
    }
    for (CFIndex index = 0; index < count; ++index) {
        int64_t tag = 0;
        double value = 0.0;
        if (CFGetTypeID(keys[index]) == CFNumberGetTypeID() &&
            CFGetTypeID(values[index]) == CFNumberGetTypeID() &&
            CFNumberGetValue(static_cast<CFNumberRef>(keys[index]),
                             kCFNumberSInt64Type, &tag) != 0 &&
            tag == kWeightAxis &&
            CFNumberGetValue(static_cast<CFNumberRef>(values[index]),
                             kCFNumberDoubleType, &value) != 0) {
            return value;
        }
    }
    return fallback;
}

FaceWeights ReadFaceWeights(CTFontDescriptorRef descriptor) {
    const CFRef<CTFontRef> font(
        CTFontCreateWithFontDescriptor(descriptor, 12.0, nullptr));
    FaceWeights weights;
    double minimum = 0.0;
    double maximum = 0.0;
    double fallback = 0.0;
    if (font && WeightAxis(font.get(), minimum, maximum, fallback)) {
        weights.variable = true;
        weights.instance = WeightCoordinate(font.get(), fallback);
        weights.low = static_cast<int>(std::ceil(minimum));
        weights.high =
            std::max(weights.low, static_cast<int>(std::floor(maximum)));
        return weights;
    }
    weights.low = WeightClass(font.get(), descriptor);
    weights.high = weights.low;
    weights.instance = weights.low;
    return weights;
}

// The font files registered by RegisterMotionFontDirectory, which win over
// another copy of the same face. MA7 -- docs/roadmap/MOTION-AFTER-2026-09.md.
// Read by every frame that chooses a face, frames painted on every core
// included (MotionParallelRender.h), and written when a tool registers a
// directory, possibly during an export: guarded.
class RegisteredFontFiles {
public:
    static bool Has(const std::string& file) {
        const std::lock_guard<std::mutex> lock(Mutex());
        return Files().count(file) != 0;
    }
    static void Add(std::string file) {
        const std::lock_guard<std::mutex> lock(Mutex());
        Files().insert(std::move(file));
    }

private:
    static std::mutex& Mutex() {
        static std::mutex mutex;
        return mutex;
    }
    static std::set<std::string>& Files() {
        static std::set<std::string> files;
        return files;
    }
};

std::string FontFile(CTFontDescriptorRef descriptor) {
    // Descriptors returned by a family match do not always carry their URL;
    // the instantiated font does.
    const CFRef<CTFontRef> font(
        CTFontCreateWithFontDescriptor(descriptor, 12.0, nullptr));
    const CFRef<CFURLRef> url(static_cast<CFURLRef>(
        CTFontCopyAttribute(font.get(), kCTFontURLAttribute)));
    if (!url) {
        return {};
    }
    const CFRef<CFStringRef> path(
        CFURLCopyFileSystemPath(url.get(), kCFURLPOSIXPathStyle));
    std::error_code status;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(ToUtf8(path.get()), status);
    return canonical.string();
}

bool IsItalic(CTFontDescriptorRef descriptor) {
    const CFRef<CFDictionaryRef> traits(static_cast<CFDictionaryRef>(
        CTFontDescriptorCopyAttribute(descriptor, kCTFontTraitsAttribute)));
    if (!traits) {
        return false;
    }
    const auto symbolic = static_cast<CFNumberRef>(
        CFDictionaryGetValue(traits.get(), kCTFontSymbolicTrait));
    int64_t value = 0;
    if (symbolic != nullptr) {
        CFNumberGetValue(symbolic, kCFNumberSInt64Type, &value);
    }
    return (value & kCTFontItalicTrait) != 0;
}

// CSS Fonts 4 font-weight matching: from 400 to 500 look up to 500 first,
// then down, then up; below 400 look down first; above 500 look up first.
int MatchWeight(const std::vector<int>& available, int desired) {
    std::vector<int> weights = available;
    std::sort(weights.begin(), weights.end());
    const auto closestAtOrAbove = [&](int from, int limit) -> int {
        for (int weight : weights) {
            if (weight >= from && weight <= limit) {
                return weight;
            }
        }
        return -1;
    };
    const auto closestBelow = [&](int from) -> int {
        for (auto it = weights.rbegin(); it != weights.rend(); ++it) {
            if (*it < from) {
                return *it;
            }
        }
        return -1;
    };
    if (std::find(weights.begin(), weights.end(), desired) != weights.end()) {
        return desired;
    }
    int match = -1;
    if (desired >= 400 && desired <= 500) {
        match = closestAtOrAbove(desired, 500);
        if (match < 0) {
            match = closestBelow(desired);
        }
        if (match < 0) {
            match = closestAtOrAbove(501, 1000);
        }
    } else if (desired < 400) {
        match = closestBelow(desired);
        if (match < 0) {
            match = closestAtOrAbove(desired, 1000);
        }
    } else {
        match = closestAtOrAbove(desired, 1000);
        if (match < 0) {
            match = closestBelow(desired);
        }
    }
    return match;
}

}  // namespace

struct MotionCoreTextMeasurer::Impl {
    std::string media_directory;
    // REM2 -- the project's media by ULID.
    std::map<std::string, std::string> media_paths;
    // REM3 -- the italic face is its own entry: the fourth member.
    using FontKey = std::tuple<std::string, int, double, bool>;
    std::map<FontKey, CFRef<CTFontRef>> fonts;
    std::map<std::string, CFRef<CGImageRef>> images;
    std::map<FontKey, std::string> files;
    // MW7 -- the faces drawn from a variable font: the weight set on its
    // axis, and whether the font names an instance there.
    struct VariableFace {
        int weight = 0;
        bool named = false;
    };
    std::map<FontKey, VariableFace> variable;
    // PERF11 -- the video pictures of the frame being painted, by file and
    // picture index, and what they are charged to the picture budget. Filled
    // before the first instant is painted and emptied after the last
    // (RenderMotionFrame): every shutter instant and every node that shows
    // the same picture draws the one image.
    using VideoKey = std::pair<std::string, int64_t>;
    std::map<VideoKey, CFRef<CGImageRef>> videos;
    size_t video_bytes = 0;
    // PERF11 -- when frames are rendered at the same time, the order this
    // one decodes its pictures in (MotionMediaTurns); null otherwise.
    MotionMediaTurns* turns = nullptr;
    size_t turn = 0;

    // MM2 -- the face with the OpenType features font-variant-numeric asks
    // for (lnum or onum, tnum or pnum), made once from the plain face: the
    // layout and the drawing both take it, so glyphs land where the boxes
    // say.
    using FeaturedKey =
        std::tuple<std::string, int, double, bool, std::string, std::string>;
    std::map<FeaturedKey, CFRef<CTFontRef>> featured;

    CTFontRef Font(const MotionTextStyle& style, MotionError& error) {
        if (!style.numeric_figures.empty() || !style.numeric_spacing.empty()) {
            MotionTextStyle plain = style;
            plain.numeric_figures.clear();
            plain.numeric_spacing.clear();
            const CTFontRef base = Font(plain, error);
            if (base == nullptr) {
                return nullptr;
            }
            const FeaturedKey key = std::make_tuple(
                style.font_family,
                static_cast<int>(std::lround(style.font_weight)),
                style.font_size, style.italic, style.numeric_figures,
                style.numeric_spacing);
            const auto cached = featured.find(key);
            if (cached != featured.end()) {
                return cached->second.get();
            }
            const CFRef<CFMutableArrayRef> settings(CFArrayCreateMutable(
                kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks));
            const auto feature = [&](const char* tag) {
                const CFRef<CFStringRef> name = MakeString(tag);
                const int on = 1;
                const CFRef<CFNumberRef> value(
                    CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &on));
                CFTypeRef keys[] = {kCTFontOpenTypeFeatureTag,
                                    kCTFontOpenTypeFeatureValue};
                CFTypeRef values[] = {name.get(), value.get()};
                const CFRef<CFDictionaryRef> setting(
                    CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                       &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks));
                CFArrayAppendValue(settings.get(), setting.get());
            };
            if (style.numeric_figures == "lining") {
                feature("lnum");
            } else if (style.numeric_figures == "oldstyle") {
                feature("onum");
            }
            if (style.numeric_spacing == "tabular") {
                feature("tnum");
            } else if (style.numeric_spacing == "proportional") {
                feature("pnum");
            }
            CFTypeRef keys[] = {kCTFontFeatureSettingsAttribute};
            CFTypeRef values[] = {settings.get()};
            const CFRef<CFDictionaryRef> attributes(
                CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                   &kCFTypeDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks));
            const CFRef<CTFontDescriptorRef> descriptor(
                CTFontDescriptorCreateWithAttributes(attributes.get()));
            CFRef<CTFontRef> font(CTFontCreateCopyWithAttributes(
                base, style.font_size, nullptr, descriptor.get()));
            if (!font) {
                font = CFRef<CTFontRef>(static_cast<CTFontRef>(CFRetain(base)));
            }
            return featured.emplace(key, std::move(font)).first->second.get();
        }
        const int desired = static_cast<int>(std::lround(style.font_weight));
        const auto key = std::make_tuple(style.font_family, desired,
                                         style.font_size, style.italic);
        const auto cached = fonts.find(key);
        if (cached != fonts.end()) {
            return cached->second.get();
        }
        CFRef<CTFontRef> font;
        if (style.font_family == "system-ui") {
            font = CFRef<CTFontRef>(CTFontCreateUIFontForLanguage(
                desired >= 600 ? kCTFontUIFontEmphasizedSystem
                               : kCTFontUIFontSystem,
                style.font_size, nullptr));
            if (font && style.italic) {
                // The system family's own italic, or nothing.
                font = CFRef<CTFontRef>(CTFontCreateCopyWithSymbolicTraits(
                    font.get(), style.font_size, nullptr, kCTFontItalicTrait,
                    kCTFontItalicTrait));
            }
        } else {
            const CFRef<CFStringRef> family = MakeString(style.font_family);
            CFTypeRef keys[] = {kCTFontFamilyNameAttribute};
            CFTypeRef values[] = {family.get()};
            const CFRef<CFDictionaryRef> attributes(
                CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                   &kCFTypeDictionaryKeyCallBacks,
                                   &kCFTypeDictionaryValueCallBacks));
            const CFRef<CTFontDescriptorRef> request(
                CTFontDescriptorCreateWithAttributes(attributes.get()));
            const CFRef<CFSetRef> mandatory(CFSetCreate(
                kCFAllocatorDefault, keys, 1, &kCFTypeSetCallBacks));
            const CFRef<CFArrayRef> faces(
                CTFontDescriptorCreateMatchingFontDescriptors(request.get(),
                                                              mandatory.get()));
            std::vector<CTFontDescriptorRef> upright;
            std::vector<FaceWeights> weights;
            const CFIndex count = faces ? CFArrayGetCount(faces.get()) : 0;
            for (CFIndex index = 0; index < count; ++index) {
                const auto face = static_cast<CTFontDescriptorRef>(
                    CFArrayGetValueAtIndex(faces.get(), index));
                const CFRef<CFStringRef> faceFamily(
                    static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(
                        face, kCTFontFamilyNameAttribute)));
                // Core Text may answer a near name; a composition asked for
                // this exact family, so anything else counts as missing.
                // REM3 -- the style's own faces: upright for normal, the
                // family's italics for italic; never one for the other.
                if (!faceFamily ||
                    CFStringCompare(faceFamily.get(), family.get(),
                                    kCFCompareCaseInsensitive) !=
                        kCFCompareEqualTo ||
                    IsItalic(face) != style.italic) {
                    continue;
                }
                upright.push_back(face);
                weights.push_back(ReadFaceWeights(face));
            }
            // MW7 -- CSS Fonts 4 searches the weights in its order and stops
            // at the first a face covers: for a range, the end nearest the
            // weight asked, or that weight itself when the range holds it.
            // That is the search run on the weight each face is drawn at.
            std::vector<int> drawn;
            drawn.reserve(weights.size());
            for (const FaceWeights& face : weights) {
                drawn.push_back(face.Drawn(desired));
            }
            const int chosen = MatchWeight(drawn, desired);
            // Several files can provide the same face -- a composition's own
            // font next to a copy activated by a font service, in another
            // revision with other metrics. The composition's fonts win, then
            // the path order: never whichever Core Text happens to list first.
            // MW7 -- the instances of one variable file are one face: the
            // instance nearest the weight drawn is the base.
            size_t selected = upright.size();
            std::string selectedFile;
            bool selectedRegistered = false;
            double selectedDistance = 0.0;
            for (size_t index = 0; index < upright.size() && chosen > 0;
                 ++index) {
                if (drawn[index] != chosen) {
                    continue;
                }
                const std::string file = FontFile(upright[index]);
                const bool registered = RegisteredFontFiles::Has(file);
                const double distance =
                    std::fabs(weights[index].instance - chosen);
                if (selected == upright.size() ||
                    (registered && !selectedRegistered) ||
                    (registered == selectedRegistered &&
                     (file < selectedFile ||
                      (file == selectedFile && distance < selectedDistance)))) {
                    selected = index;
                    selectedFile = file;
                    selectedRegistered = registered;
                    selectedDistance = distance;
                }
            }
            if (selected != upright.size()) {
                // MW7 -- a variable face is set on its wght axis to the
                // weight drawn, unless the instance already stands there.
                const FaceWeights& face = weights[selected];
                CFRef<CTFontDescriptorRef> instance;
                if (face.variable &&
                    face.instance != static_cast<double>(chosen)) {
                    const int64_t axis = kWeightAxis;
                    const CFRef<CFNumberRef> identifier(CFNumberCreate(
                        kCFAllocatorDefault, kCFNumberSInt64Type, &axis));
                    instance = CFRef<CTFontDescriptorRef>(
                        CTFontDescriptorCreateCopyWithVariation(
                            upright[selected], identifier.get(),
                            static_cast<CGFloat>(chosen)));
                }
                font = CFRef<CTFontRef>(CTFontCreateWithFontDescriptor(
                    instance ? instance.get() : upright[selected],
                    style.font_size, nullptr));
                files[key] = selectedFile;
                if (face.variable) {
                    variable[key] = {chosen, !instance};
                }
            }
        }
        if (!font) {
            Fail(error, "FontMissing", "",
                 "font family '" + style.font_family + "'" +
                     (style.italic ? " has no italic face installed or "
                                     "registered; refusing to slant another"
                                   : " is not installed or registered; "
                                     "refusing to substitute another font"));
            return nullptr;
        }
        return fonts.emplace(key, std::move(font)).first->second.get();
    }

    CFRef<CFAttributedStringRef> Attributed(CFStringRef text, CTFontRef font,
                                            const MotionTextStyle& style) {
        const CGFloat kern = style.letter_spacing;
        const CFRef<CFNumberRef> kernNumber(
            CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &kern));
        // M1b: a text cut into characters is drawn one glyph at a time, so
        // it is laid out without the optional ligatures it could not split.
        const int ligature = style.ligatures ? 1 : 0;
        const CFRef<CFNumberRef> ligatureNumber(
            CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ligature));
        CFTypeRef keys[] = {kCTFontAttributeName, kCTKernAttributeName,
                            kCTForegroundColorFromContextAttributeName,
                            kCTLigatureAttributeName};
        CFTypeRef values[] = {font, kernNumber.get(), kCFBooleanTrue,
                              ligatureNumber.get()};
        const CFRef<CFDictionaryRef> attributes(CFDictionaryCreate(
            kCFAllocatorDefault, keys, values, 4,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        CFRef<CFMutableAttributedStringRef> attributed(
            CFAttributedStringCreateMutable(kCFAllocatorDefault, 0));
        CFAttributedStringReplaceString(attributed.get(), CFRangeMake(0, 0),
                                        text);
        CFAttributedStringSetAttributes(
            attributed.get(), CFRangeMake(0, CFStringGetLength(text)),
            attributes.get(), static_cast<Boolean>(true));
        // M1b -- CSS word-spacing: added to the advance of every space and
        // no-break space, on top of the letter spacing.
        if (style.word_spacing != 0.0) {
            const CGFloat wide = style.letter_spacing + style.word_spacing;
            const CFRef<CFNumberRef> wideNumber(CFNumberCreate(
                kCFAllocatorDefault, kCFNumberCGFloatType, &wide));
            for (CFIndex index = 0; index < CFStringGetLength(text); ++index) {
                const UniChar character =
                    CFStringGetCharacterAtIndex(text, index);
                if (character == 0x20 || character == 0xA0) {
                    CFAttributedStringSetAttribute(
                        attributed.get(), CFRangeMake(index, 1),
                        kCTKernAttributeName, wideNumber.get());
                }
            }
        }
        return CFRef<CFAttributedStringRef>(CFAttributedStringCreateCopy(
            kCFAllocatorDefault, attributed.get()));
    }

    std::string ResolveMedia(const std::string& source) const {
        std::filesystem::path path(source);
        if (path.is_relative() && !media_directory.empty()) {
            path = std::filesystem::path(media_directory) / path;
        }
        return path.lexically_normal().string();
    }

    // REM2 -- where a video's or a sound's media is: a file named by `src`,
    // or a project media named by its ULID.
    bool MediaPath(const std::string& src, const std::string& media,
                   std::string& path, MotionError& error) const {
        if (media.empty()) {
            path = ResolveMedia(src);
            return true;
        }
        const auto found = media_paths.find(media);
        if (found == media_paths.end()) {
            return Fail(error, "MediaMissing", "",
                        "the project has no media '" + media + "'");
        }
        path = ResolveMedia(found->second);
        return true;
    }

    CGImageRef Image(const std::string& source, MotionError& error) {
        const std::string path = ResolveMedia(source);
        const auto cached = images.find(path);
        if (cached != images.end()) {
            return cached->second.get();
        }
        const CFRef<CFURLRef> url = MakeFileUrl(path);
        const CFRef<CGImageSourceRef> imageSource(
            url ? CGImageSourceCreateWithURL(url.get(), nullptr) : nullptr);
        CFRef<CGImageRef> image(
            imageSource && CGImageSourceGetCount(imageSource.get()) > 0
                ? CGImageSourceCreateImageAtIndex(imageSource.get(), 0, nullptr)
                : nullptr);
        // REMOTION-PARITY-2026-09 REM6 -- a picture is drawn, and measured
        // for objectFit, the way its file says to show it: a phone photo
        // stored on its side with an EXIF orientation comes back upright,
        // as a browser shows it (image-orientation: from-image). Only a
        // file that asks for a turn takes this path; the others keep their
        // exact decode.
        if (image) {
            int orientation = 1;
            const CFRef<CFDictionaryRef> properties(
                CGImageSourceCopyPropertiesAtIndex(imageSource.get(), 0,
                                                   nullptr));
            const CFNumberRef tag =
                properties ? static_cast<CFNumberRef>(CFDictionaryGetValue(
                                 properties.get(), kCGImagePropertyOrientation))
                           : nullptr;
            if (tag != nullptr) {
                CFNumberGetValue(tag, kCFNumberIntType, &orientation);
            }
            if (orientation > 1 && orientation <= 8) {
                const int longest =
                    static_cast<int>(std::max(CGImageGetWidth(image.get()),
                                              CGImageGetHeight(image.get())));
                const CFRef<CFNumberRef> size(
                    CFNumberCreate(nullptr, kCFNumberIntType, &longest));
                const void* keys[] = {
                    kCGImageSourceCreateThumbnailFromImageAlways,
                    kCGImageSourceCreateThumbnailWithTransform,
                    kCGImageSourceThumbnailMaxPixelSize};
                const void* values[] = {kCFBooleanTrue, kCFBooleanTrue,
                                        size.get()};
                const CFRef<CFDictionaryRef> options(CFDictionaryCreate(
                    nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                    &kCFTypeDictionaryValueCallBacks));
                image = CFRef<CGImageRef>(CGImageSourceCreateThumbnailAtIndex(
                    imageSource.get(), 0, options.get()));
            }
        }
        if (!image) {
            Fail(error, "MediaMissing", "", "cannot read image '" + path + "'");
            return nullptr;
        }
        return images.emplace(path, std::move(image)).first->second.get();
    }
};

bool RegisterMotionFontDirectory(const std::string& directory,
                                 MotionError& error) {
    std::error_code status;
    if (!std::filesystem::is_directory(directory, status)) {
        return Fail(error, "IoError", "fonts",
                    "font directory does not exist: '" + directory + "'");
    }
    std::vector<std::filesystem::path> files;
    std::filesystem::directory_iterator entry(directory, status), end;
    for (; !status && entry != end; entry.increment(status)) {
        std::string extension = entry->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (extension == ".ttf" || extension == ".otf" || extension == ".ttc") {
            files.push_back(std::filesystem::absolute(entry->path(), status));
        }
    }
    if (status) {
        return Fail(error, "IoError", "fonts", status.message());
    }
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files) {
        const CFRef<CFURLRef> url = MakeFileUrl(file.string());
        CFErrorRef registrationError = nullptr;
        if (!CTFontManagerRegisterFontsForURL(
                url.get(), kCTFontManagerScopeProcess, &registrationError)) {
            const CFRef<CFErrorRef> owned(registrationError);
            // Registering the same file twice in one process is not a fault.
            if (owned && CFErrorGetCode(owned.get()) ==
                             kCTFontManagerErrorAlreadyRegistered) {
                continue;
            }
            return Fail(error, "IoError", "fonts",
                        "cannot register font '" + file.string() + "'");
        }
    }
    for (const std::filesystem::path& file : files) {
        std::error_code canonicalStatus;
        RegisteredFontFiles::Add(
            std::filesystem::weakly_canonical(file, canonicalStatus).string());
    }
    return true;
}

bool ListMotionFontFamilies(std::vector<std::string>& families,
                            MotionError& error) {
    families.clear();
    const CFRef<CTFontCollectionRef> collection(
        CTFontCollectionCreateFromAvailableFonts(nullptr));
    const CFRef<CFArrayRef> descriptors(
        collection
            ? CTFontCollectionCreateMatchingFontDescriptors(collection.get())
            : nullptr);
    if (!descriptors) {
        return Fail(error, "IoError", "fonts", "Core Text listed no font");
    }
    std::set<std::string> seen;
    const CFIndex count = CFArrayGetCount(descriptors.get());
    for (CFIndex index = 0; index < count; ++index) {
        const auto descriptor = static_cast<CTFontDescriptorRef>(
            CFArrayGetValueAtIndex(descriptors.get(), index));
        const CFRef<CFStringRef> family(
            static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(
                descriptor, kCTFontFamilyNameAttribute)));
        std::string name = ToUtf8(family.get());
        if (!name.empty() && seen.insert(name).second) {
            families.push_back(std::move(name));
        }
    }
    return true;
}

// MB7 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. After names a text
// layer's face by its PostScript name; the format names a family, a weight
// and a slant. Core Text gives a substitute for a name it does not know, so
// the face found must carry the very name asked.
bool MotionFontForPostScriptName(const std::string& name, std::string& family,
                                 int& weight, bool& italic) {
    if (name.empty()) {
        return false;
    }
    const CFRef<CFStringRef> postScript = MakeString(name);
    const CFRef<CTFontRef> font(
        CTFontCreateWithName(postScript.get(), 12.0, nullptr));
    if (!font) {
        return false;
    }
    const CFRef<CFStringRef> found(CTFontCopyPostScriptName(font.get()));
    if (ToUtf8(found.get()) != name) {
        return false;
    }
    const CFRef<CTFontDescriptorRef> descriptor(
        CTFontCopyFontDescriptor(font.get()));
    const CFRef<CFStringRef> familyName(
        static_cast<CFStringRef>(CTFontDescriptorCopyAttribute(
            descriptor.get(), kCTFontFamilyNameAttribute)));
    family = ToUtf8(familyName.get());
    weight = WeightClass(font.get(), descriptor.get());
    italic = (CTFontGetSymbolicTraits(font.get()) & kCTFontTraitItalic) != 0;
    return !family.empty();
}

MotionCoreTextMeasurer::MotionCoreTextMeasurer(std::string media_directory)
    : impl_(std::make_unique<Impl>()) {
    impl_->media_directory = std::move(media_directory);
}

MotionCoreTextMeasurer::~MotionCoreTextMeasurer() = default;

void MotionCoreTextMeasurer::SetMediaPaths(
    std::map<std::string, std::string> paths) {
    impl_->media_paths = std::move(paths);
}

void MotionCoreTextMeasurer::SetMediaTurns(MotionMediaTurns* turns,
                                           size_t ordinal) {
    impl_->turns = turns;
    impl_->turn = ordinal;
}

bool MotionCoreTextMeasurer::MediaInfo(const std::string& src,
                                       const std::string& media,
                                       MotionMediaInfo& info,
                                       MotionError& error) {
    std::string path;
    if (!impl_->MediaPath(src, media, path, error)) {
        return false;
    }
    std::string code;
    std::string message;
    if (!ProbeMotionMedia(path, info, code, message)) {
        return Fail(error, code, "", message);
    }
    return true;
}

bool MotionCoreTextMeasurer::MediaSound(const std::string& src,
                                        const std::string& media,
                                        std::shared_ptr<const MotionPcm>& pcm,
                                        MotionError& error) {
    std::string path;
    if (!impl_->MediaPath(src, media, path, error)) {
        return false;
    }
    std::string code;
    std::string message;
    if (!ReadMotionAudio(path, pcm, code, message)) {
        return Fail(error, code, "", message);
    }
    return true;
}

bool MotionCoreTextMeasurer::LayoutText(const std::string& text,
                                        const MotionTextStyle& style,
                                        double max_width,
                                        MotionTextLayout& layout,
                                        MotionError& error) {
    const CTFontRef font = impl_->Font(style, error);
    if (font == nullptr) {
        return false;
    }
    const CFRef<CFStringRef> source = MakeString(text);
    if (!source) {
        return Fail(error, "InvalidValue", "", "text is not valid UTF-8");
    }
    const CFRef<CFMutableStringRef> transformed(
        CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source.get()));
    // Unlocalised case mapping: the result must not depend on the machine's
    // language settings.
    if (style.text_transform == "uppercase") {
        CFStringUppercase(transformed.get(), nullptr);
    } else if (style.text_transform == "lowercase") {
        CFStringLowercase(transformed.get(), nullptr);
    } else if (style.text_transform == "capitalize") {
        CFStringCapitalize(transformed.get(), nullptr);
    }

    // CSS line model: every line box is line-height tall, and the font's
    // ascent plus descent sits in its middle. "normal" is the font's own
    // ascent + descent + leading.
    const double ascent = CTFontGetAscent(font);
    const double descent = CTFontGetDescent(font);
    const double lineHeight = style.line_height > 0.0
                                  ? style.line_height
                                  : ascent + descent + CTFontGetLeading(font);
    const double baselineOffset =
        (lineHeight - (ascent + descent)) / 2.0 + ascent;

    layout = MotionTextLayout{};
    std::vector<MotionTextLine> lines;
    const CFIndex totalLength = CFStringGetLength(transformed.get());
    CFIndex paragraphStart = 0;
    while (paragraphStart <= totalLength) {
        CFRange newline;
        const bool found =
            CFStringFindWithOptions(
                transformed.get(), CFSTR("\n"),
                CFRangeMake(paragraphStart, totalLength - paragraphStart), 0,
                &newline) != 0u;
        const CFIndex paragraphEnd = found ? newline.location : totalLength;
        const CFRef<CFStringRef> paragraph(CFStringCreateWithSubstring(
            kCFAllocatorDefault, transformed.get(),
            CFRangeMake(paragraphStart, paragraphEnd - paragraphStart)));
        const CFRef<CFAttributedStringRef> attributed =
            impl_->Attributed(paragraph.get(), font, style);
        const CFRef<CTTypesetterRef> typesetter(
            CTTypesetterCreateWithAttributedString(attributed.get()));
        const CFIndex length = CFStringGetLength(paragraph.get());
        CFIndex start = 0;
        do {
            CFIndex count = length - start;
            if (max_width >= 0.0 && count > 0) {
                count = std::max<CFIndex>(
                    1, CTTypesetterSuggestLineBreak(typesetter.get(), start,
                                                    max_width));
            }
            const CFRef<CTLineRef> line(CTTypesetterCreateLine(
                typesetter.get(), CFRangeMake(start, count)));
            // Core Text can resolve the family yet silently switch fonts for
            // unsupported glyphs. That changes metrics just as a missing
            // family would, so reject fallback before publishing geometry.
            const CFArrayRef runs = CTLineGetGlyphRuns(line.get());
            const CFRef<CFStringRef> requestedName(
                CTFontCopyPostScriptName(font));
            for (CFIndex index = 0; index < CFArrayGetCount(runs); ++index) {
                const auto run =
                    static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, index));
                const auto runFont =
                    static_cast<CTFontRef>(CFDictionaryGetValue(
                        CTRunGetAttributes(run), kCTFontAttributeName));
                const CFRef<CFStringRef> runName(
                    CTFontCopyPostScriptName(runFont));
                if (CFEqual(requestedName.get(), runName.get()) == 0u) {
                    return Fail(
                        error, "FontMissing", "",
                        "font family '" + style.font_family +
                            "' does not cover this text; refusing glyph "
                            "substitution with '" +
                            ToUtf8(runName.get()) + "'");
                }
            }
            const CFRef<CFStringRef> lineText(CFStringCreateWithSubstring(
                kCFAllocatorDefault, paragraph.get(),
                CFRangeMake(start, count)));
            MotionTextLine placed;
            placed.text = ToUtf8(lineText.get());
            placed.width = CTLineGetTypographicBounds(line.get(), nullptr,
                                                      nullptr, nullptr) -
                           CTLineGetTrailingWhitespaceWidth(line.get());
            // M1b -- grapheme clusters with their caret offsets in the laid
            // out line: kerning included, so a word or a character drawn
            // alone lands where it was in the line.
            size_t byte = 0;
            for (CFIndex index = start; index < start + count;) {
                const CFRange composed =
                    CFStringGetRangeOfComposedCharactersAtIndex(paragraph.get(),
                                                                index);
                const CFIndex end = std::min(
                    composed.location + composed.length, start + count);
                const CFRef<CFStringRef> piece(CFStringCreateWithSubstring(
                    kCFAllocatorDefault, paragraph.get(),
                    CFRangeMake(index, end - index)));
                MotionTextCluster cluster;
                cluster.start = byte;
                cluster.length = ToUtf8(piece.get()).size();
                cluster.x =
                    CTLineGetOffsetForStringIndex(line.get(), index, nullptr);
                cluster.width =
                    CTLineGetOffsetForStringIndex(line.get(), end, nullptr) -
                    cluster.x;
                placed.clusters.push_back(cluster);
                byte += cluster.length;
                index = end;
            }
            lines.push_back(placed);
            layout.width = std::max(layout.width, placed.width);
            start += count;
        } while (start < length);
        if (!found) {
            break;
        }
        paragraphStart = paragraphEnd + 1;
    }

    const double frame = max_width >= 0.0 ? max_width : layout.width;
    for (size_t index = 0; index < lines.size(); ++index) {
        MotionTextLine& line = lines[index];
        line.height = lineHeight;
        line.y = lineHeight * static_cast<double>(index);
        line.baseline = line.y + baselineOffset;
        line.x = style.text_align == "center"  ? (frame - line.width) / 2.0
                 : style.text_align == "right" ? frame - line.width
                                               : 0.0;
    }
    layout.lines = std::move(lines);
    layout.height = lineHeight * static_cast<double>(layout.lines.size());
    return true;
}

CTFontRef MotionCoreTextFont(MotionCoreTextMeasurer& measurer,
                             const MotionTextStyle& style, MotionError& error) {
    return measurer.impl().Font(style, error);
}

bool MotionCoreTextMeasurer::NumericVariantChanges(
    const std::string& text, const MotionTextStyle& style) {
    if (style.numeric_figures.empty() && style.numeric_spacing.empty()) {
        return false;
    }
    if (std::none_of(text.begin(), text.end(),
                     [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    MotionTextStyle plain = style;
    plain.numeric_figures.clear();
    plain.numeric_spacing.clear();
    MotionError error;
    const CTFontRef variant = impl_->Font(style, error);
    const CTFontRef base = impl_->Font(plain, error);
    if (variant == nullptr || base == nullptr) {
        return true;
    }
    const CFRef<CFStringRef> string = MakeString(text);
    if (!string) {
        return true;
    }
    // Glyphs and advances of one line, as the layout sets it.
    const auto glyphs = [&](CTFontRef font, std::vector<CGGlyph>& ids,
                            std::vector<CGSize>& advances) {
        const CFRef<CFAttributedStringRef> attributed =
            impl_->Attributed(string.get(), font, style);
        const CFRef<CTLineRef> line(
            CTLineCreateWithAttributedString(attributed.get()));
        const CFArrayRef runs = CTLineGetGlyphRuns(line.get());
        for (CFIndex index = 0; index < CFArrayGetCount(runs); ++index) {
            const auto run =
                static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, index));
            const CFIndex count = CTRunGetGlyphCount(run);
            std::vector<CGGlyph> runGlyphs(static_cast<size_t>(count));
            std::vector<CGSize> runAdvances(static_cast<size_t>(count));
            CTRunGetGlyphs(run, CFRangeMake(0, count), runGlyphs.data());
            CTRunGetAdvances(run, CFRangeMake(0, count), runAdvances.data());
            ids.insert(ids.end(), runGlyphs.begin(), runGlyphs.end());
            advances.insert(advances.end(), runAdvances.begin(),
                            runAdvances.end());
        }
    };
    std::vector<CGGlyph> withIds;
    std::vector<CGGlyph> plainIds;
    std::vector<CGSize> withAdvances;
    std::vector<CGSize> plainAdvances;
    glyphs(variant, withIds, withAdvances);
    glyphs(base, plainIds, plainAdvances);
    if (withIds != plainIds || withAdvances.size() != plainAdvances.size()) {
        return true;
    }
    for (size_t index = 0; index < withAdvances.size(); ++index) {
        if (std::abs(withAdvances[index].width - plainAdvances[index].width) >
            1e-6) {
            return true;
        }
    }
    return false;
}

bool MotionCoreTextMeasurer::ResolveFontFile(const MotionTextStyle& style,
                                             std::string& file,
                                             MotionError& error) {
    if (impl_->Font(style, error) == nullptr) {
        return false;
    }
    const auto found = impl_->files.find(std::make_tuple(
        style.font_family, static_cast<int>(std::lround(style.font_weight)),
        style.font_size, style.italic));
    file = found != impl_->files.end() ? found->second : std::string();
    return true;
}

bool MotionCoreTextMeasurer::ResolveFontName(const MotionTextStyle& style,
                                             std::string& name,
                                             MotionError& error) {
    const CTFontRef font = impl_->Font(style, error);
    if (font == nullptr) {
        return false;
    }
    const CFRef<CFStringRef> postScript(CTFontCopyPostScriptName(font));
    name = ToUtf8(postScript.get());
    return true;
}

bool MotionCoreTextMeasurer::FontVariableWeight(const MotionTextStyle& style,
                                                int& weight, bool& named) {
    MotionError error;
    if (impl_->Font(style, error) == nullptr) {
        return false;
    }
    const auto found = impl_->variable.find(std::make_tuple(
        style.font_family, static_cast<int>(std::lround(style.font_weight)),
        style.font_size, style.italic));
    if (found == impl_->variable.end()) {
        return false;
    }
    weight = found->second.weight;
    named = found->second.named;
    return true;
}

bool MotionCoreTextMeasurer::ImageSize(const std::string& source, double& width,
                                       double& height, MotionError& error) {
    const CGImageRef image = impl_->Image(source, error);
    if (image == nullptr) {
        return false;
    }
    width = static_cast<double>(CGImageGetWidth(image));
    height = static_cast<double>(CGImageGetHeight(image));
    return true;
}

namespace {

void SetFill(CGContextRef context, const MotionColor& color) {
    CGContextSetRGBFillColor(context, color.red, color.green, color.blue,
                             color.alpha);
}

// Rotation and scale around the centre of a box, after a translation: the
// order of CSS individual transforms.
void Transform(CGContextRef context, const MotionBox& box, double translateX,
               double translateY, double rotate, double scaleX, double scaleY) {
    const double centerX = box.x + box.width / 2.0;
    const double centerY = box.y + box.height / 2.0;
    CGContextTranslateCTM(context, centerX + translateX, centerY + translateY);
    CGContextRotateCTM(context, rotate * kPi / 180.0);
    CGContextScaleCTM(context, scaleX, scaleY);
    CGContextTranslateCTM(context, -centerX, -centerY);
}

// A node's own transforms. MOTION-WEB-2026-10 MW5 -- about its transform
// origin, with its skew between the rotation and the scale; without
// either, the calls above, so that nothing else moves by a bit.
void TransformNode(CGContextRef context, const MotionEvaluatedNode& node) {
    if (node.origin.IsCentre() && node.skew_x == 0.0 && node.skew_y == 0.0) {
        Transform(context, node.box, node.translate_x, node.translate_y,
                  node.rotate, node.scale_x, node.scale_y);
        return;
    }
    const double pivotX = MotionPivotX(node);
    const double pivotY = MotionPivotY(node);
    CGContextTranslateCTM(context, pivotX + node.translate_x,
                          pivotY + node.translate_y);
    CGContextRotateCTM(context, node.rotate * kPi / 180.0);
    CGContextConcatCTM(
        context, CGAffineTransformMake(1.0, std::tan(node.skew_y * kPi / 180.0),
                                       std::tan(node.skew_x * kPi / 180.0), 1.0,
                                       0.0, 0.0));
    CGContextScaleCTM(context, node.scale_x, node.scale_y);
    CGContextTranslateCTM(context, -pivotX, -pivotY);
}

// M1b -- the path the evaluator already transformed and trimmed.
CFRef<CGPathRef> MakePath(const MotionPath& geometry) {
    CGMutablePathRef path = CGPathCreateMutable();
    bool open = false;
    for (const MotionPathSegment& segment : geometry) {
        switch (segment.verb) {
            case MotionPathVerb::Move:
                CGPathMoveToPoint(path, nullptr, segment.x, segment.y);
                open = true;
                break;
            case MotionPathVerb::Line:
                if (open) {
                    CGPathAddLineToPoint(path, nullptr, segment.x, segment.y);
                }
                break;
            case MotionPathVerb::Cubic:
                if (open) {
                    CGPathAddCurveToPoint(path, nullptr, segment.x1, segment.y1,
                                          segment.x2, segment.y2, segment.x,
                                          segment.y);
                }
                break;
            case MotionPathVerb::Close:
                if (open) {
                    CGPathCloseSubpath(path);
                }
                break;
        }
    }
    return CFRef<CGPathRef>(path);
}

// SVG paint order: fill, then stroke centred on the outline.
void DrawPath(CGContextRef context, const MotionPathPaint& paint) {
    if (paint.geometry.empty()) {
        return;
    }
    const CFRef<CGPathRef> path = MakePath(paint.geometry);
    if (paint.has_fill && paint.fill.alpha > 0.0) {
        SetFill(context, paint.fill);
        CGContextAddPath(context, path.get());
        if (paint.fill_rule == "evenodd") {
            CGContextEOFillPath(context);
        } else {
            CGContextFillPath(context);
        }
    }
    if (paint.has_stroke && paint.stroke.alpha > 0.0 &&
        paint.stroke_width > 0.0) {
        CGContextSetRGBStrokeColor(context, paint.stroke.red,
                                   paint.stroke.green, paint.stroke.blue,
                                   paint.stroke.alpha);
        CGContextSetLineWidth(context, paint.stroke_width);
        CGContextSetLineCap(context, paint.linecap == "round" ? kCGLineCapRound
                                     : paint.linecap == "square"
                                         ? kCGLineCapSquare
                                         : kCGLineCapButt);
        CGContextSetLineJoin(context,
                             paint.linejoin == "round"   ? kCGLineJoinRound
                             : paint.linejoin == "bevel" ? kCGLineJoinBevel
                                                         : kCGLineJoinMiter);
        // SVG's default stroke-miterlimit.
        CGContextSetMiterLimit(context, 4.0);
        if (!paint.dash.empty()) {
            const std::vector<CGFloat> lengths(paint.dash.begin(),
                                               paint.dash.end());
            CGContextSetLineDash(context, paint.dash_offset, lengths.data(),
                                 lengths.size());
        }
        CGContextAddPath(context, path.get());
        CGContextStrokePath(context);
        if (!paint.dash.empty()) {
            CGContextSetLineDash(context, 0.0, nullptr, 0);
        }
    }
}

// Glyphs of one run of text at its baseline, filled then outlined when the
// style has a -webkit-text-stroke.
void DrawGlyphs(CGContextRef context, MotionCoreTextMeasurer::Impl& impl,
                CTFontRef font, const MotionTextStyle& style,
                const std::string& text, const MotionColor& color, double x,
                double baseline) {
    SetFill(context, color);
    if (style.stroke_width > 0.0) {
        CGContextSetTextDrawingMode(context, kCGTextFillStroke);
        CGContextSetLineWidth(context, style.stroke_width);
        CGContextSetLineJoin(context, kCGLineJoinMiter);
        CGContextSetRGBStrokeColor(
            context, style.stroke_color.red, style.stroke_color.green,
            style.stroke_color.blue, style.stroke_color.alpha);
    } else {
        CGContextSetTextDrawingMode(context, kCGTextFill);
    }
    const CFRef<CFStringRef> string = MakeString(text);
    const CFRef<CFAttributedStringRef> attributed =
        impl.Attributed(string.get(), font, style);
    const CFRef<CTLineRef> drawn(
        CTLineCreateWithAttributedString(attributed.get()));
    CGContextSetTextPosition(context, x, baseline);
    CTLineDraw(drawn.get(), context);
}

// M1b -- a text painted unit by unit: each line, word or character at the
// position layout gave it, with its own colour, background, opacity and
// transform.
void DrawUnits(CGContextRef context, const MotionEvaluatedNode& node,
               MotionCoreTextMeasurer::Impl& impl, CTFontRef font) {
    for (const MotionTextUnitState& unit : node.units) {
        MotionBox box;
        std::string text;
        double baseline = 0.0;
        if (node.unit_kind == MotionTextUnit::Line) {
            const MotionTextLine& line = node.text_layout.lines[unit.index];
            box = {line.x, line.y, line.width, line.height};
            text = line.text;
            baseline = line.baseline;
        } else {
            const MotionTextSpan& span = node.unit_kind == MotionTextUnit::Word
                                             ? node.words[unit.index]
                                             : node.chars[unit.index];
            box = {span.x, span.y, span.width, span.height};
            text = span.text;
            baseline = span.baseline;
        }
        box.x += node.box.x;
        box.y += node.box.y;
        if (unit.opacity <= 0.0 || unit.scale_x == 0.0 || unit.scale_y == 0.0) {
            continue;
        }
        CGContextSaveGState(context);
        Transform(context, box, unit.translate_x, unit.translate_y, unit.rotate,
                  unit.scale_x, unit.scale_y);
        const bool group = unit.opacity < 1.0;
        if (group) {
            CGContextSetAlpha(context, unit.opacity);
            CGContextBeginTransparencyLayer(context, nullptr);
        }
        if (unit.has_background) {
            SetFill(context, unit.background);
            CGContextFillRect(context,
                              CGRectMake(box.x, box.y, box.width, box.height));
        }
        CGContextSetTextMatrix(context, CGAffineTransformMakeScale(1, -1));
        DrawGlyphs(context, impl, font, node.text_style, text, unit.color,
                   box.x, node.box.y + baseline);
        if (group) {
            CGContextEndTransparencyLayer(context);
        }
        CGContextRestoreGState(context);
    }
}

// REM2 -- the box, rounded by its borderRadius as CSS rounds it: never more
// than half the smaller side.
void AddRoundedBox(CGContextRef context, const MotionEvaluatedNode& node) {
    const CGRect box =
        CGRectMake(node.box.x, node.box.y, node.box.width, node.box.height);
    const double radius = std::min(
        {node.border_radius, node.box.width / 2.0, node.box.height / 2.0});
    if (radius > 0.0) {
        const CFRef<CGPathRef> path(
            CGPathCreateWithRoundedRect(box, radius, radius, nullptr));
        CGContextAddPath(context, path.get());
    } else {
        CGContextAddRect(context, box);
    }
}

// An image's or a video's picture in its box, by objectFit. A border radius
// clips the picture, as CSS clips a replaced element's content (REM2; M2
// only rounded the background).
void DrawPicture(CGContextRef context, const MotionEvaluatedNode& node,
                 CGImageRef image) {
    const CGRect box =
        CGRectMake(node.box.x, node.box.y, node.box.width, node.box.height);
    const double imageWidth = static_cast<double>(CGImageGetWidth(image));
    const double imageHeight = static_cast<double>(CGImageGetHeight(image));
    CGContextSaveGState(context);
    if (node.border_radius > 0.0) {
        CGContextBeginPath(context);
        AddRoundedBox(context, node);
        CGContextClip(context);
    }
    CGRect target = box;
    if (node.object_fit != "fill" && imageWidth > 0.0 && imageHeight > 0.0) {
        double scale = 1.0;
        if (node.object_fit == "contain") {
            scale = std::min(box.size.width / imageWidth,
                             box.size.height / imageHeight);
        } else if (node.object_fit == "cover") {
            scale = std::max(box.size.width / imageWidth,
                             box.size.height / imageHeight);
        }
        const double width = imageWidth * scale;
        const double height = imageHeight * scale;
        target = CGRectMake(node.box.x + (node.box.width - width) / 2.0,
                            node.box.y + (node.box.height - height) / 2.0,
                            width, height);
        CGContextClipToRect(context, box);
    }
    // Images draw bottom-up; undo the flip locally.
    CGContextTranslateCTM(context, target.origin.x,
                          target.origin.y + target.size.height);
    CGContextScaleCTM(context, 1, -1);
    CGContextDrawImage(context,
                       CGRectMake(0, 0, target.size.width, target.size.height),
                       image);
    CGContextRestoreGState(context);
}

// PERF11 -- docs/roadmap/PERF-2026-09.md. What the frames being painted
// may hold of converted video pictures, in the whole process: a 4K picture
// is 33 MB, a frame of four bands holds four, and twelve workers painting
// at once would hold 1.6 GB on a machine of 16. A frame asks for all of its
// pictures before it decodes the first; when the others hold too much it
// waits for one of them to finish. A frame alone always passes, whatever it
// asks for. Only the frames of a parallel render wait: a frame rendered on
// its own (the monitor, on the main thread) is counted and never held up
// behind an export running in the same process.
constexpr size_t kPictureBudgetBytes = size_t{1} << 30;

class PictureBudget {
public:
    void Acquire(size_t bytes) {
        std::unique_lock<std::mutex> lock(mutex_);
        freed_.wait(lock, [&] {
            return held_ == 0 || held_ + bytes <= kPictureBudgetBytes;
        });
        held_ += bytes;
    }
    // Counted, never waited for: a frame rendered on its own, and a picture
    // read while its frame is being painted, whose frame holds pictures the
    // others would be waiting for.
    void Charge(size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        held_ += bytes;
    }
    void Release(size_t bytes) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            held_ -= std::min(held_, bytes);
        }
        freed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable freed_;
    size_t held_ = 0;
};

PictureBudget& Budget() {
    static PictureBudget budget;
    return budget;
}

// The image of a converted picture. It takes the picture's bytes instead of
// copying them, and frees them when Core Graphics lets go of the image.
CFRef<CGImageRef> MakeVideoImage(MotionVideoPicture&& picture) {
    const auto width = static_cast<size_t>(picture.width);
    const auto height = static_cast<size_t>(picture.height);
    const bool opaque = picture.opaque;
    auto* held = new std::vector<uint8_t>(std::move(picture.rgba));
    const CFRef<CGDataProviderRef> provider(CGDataProviderCreateWithData(
        held, held->data(), held->size(), [](void* info, const void*, size_t) {
            delete static_cast<std::vector<uint8_t>*>(info);
        }));
    if (!provider) {
        delete held;
        return CFRef<CGImageRef>();
    }
    const CFRef<CGColorSpaceRef> space(
        CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    return CFRef<CGImageRef>(CGImageCreate(
        width, height, 8, 32, width * 4, space.get(),
        static_cast<CGBitmapInfo>(opaque ? kCGImageAlphaNoneSkipLast
                                         : kCGImageAlphaLast) |
            kCGBitmapByteOrder32Big,
        provider.get(), nullptr, false, kCGRenderingIntentDefault));
}

// REM2 -- the picture a video node shows at this instant, decoded as the
// evaluator named it. PERF11 -- found among the frame's pictures
// (PrepareVideos); read here only when it is not there.
bool DrawVideo(CGContextRef context, const MotionEvaluatedNode& node,
               MotionCoreTextMeasurer::Impl& impl, MotionError& error) {
    if (node.media.state != "playing") {
        return true;
    }
    std::string path;
    if (!impl.MediaPath(node.media.src, node.media.media, path, error)) {
        return false;
    }
    const MotionCoreTextMeasurer::Impl::VideoKey key(path, node.media.frame);
    auto found = impl.videos.find(key);
    if (found == impl.videos.end()) {
        MotionVideoPicture picture;
        std::string code;
        std::string message;
        if (!ReadMotionVideoFrame(path, node.media.frame, picture, code,
                                  message)) {
            return Fail(error, code, node.path, message);
        }
        const size_t bytes = picture.rgba.size();
        CFRef<CGImageRef> image = MakeVideoImage(std::move(picture));
        if (!image) {
            return Fail(error, "RenderFailed", node.path,
                        "cannot make an image of the video's picture");
        }
        Budget().Charge(bytes);
        impl.video_bytes += bytes;
        found = impl.videos.emplace(key, std::move(image)).first;
    }
    DrawPicture(context, node, found->second.get());
    return true;
}

bool DrawNode(CGContextRef context, const MotionEvaluatedNode& node,
              MotionCoreTextMeasurer::Impl& impl, MotionError& error,
              bool effects = true, bool blended = false);

// MOTION-DESIGN-2026-09 -- CSS mix-blend-mode keywords, as Core Graphics
// names them (the parser admits no other).
CGBlendMode BlendModeNamed(const std::string& name) {
    static const std::pair<const char*, CGBlendMode> kModes[] = {
        {"multiply", kCGBlendModeMultiply},
        {"screen", kCGBlendModeScreen},
        {"overlay", kCGBlendModeOverlay},
        {"darken", kCGBlendModeDarken},
        {"lighten", kCGBlendModeLighten},
        {"color-dodge", kCGBlendModeColorDodge},
        {"color-burn", kCGBlendModeColorBurn},
        {"hard-light", kCGBlendModeHardLight},
        {"soft-light", kCGBlendModeSoftLight},
        {"difference", kCGBlendModeDifference},
        {"exclusion", kCGBlendModeExclusion},
        {"plus-lighter", kCGBlendModePlusLighter},
        {"mask-in", kCGBlendModeDestinationIn},
        {"mask-out", kCGBlendModeDestinationOut},
        {"atop", kCGBlendModeSourceAtop}};
    for (const auto& [mode, value] : kModes) {
        if (name == mode) {
            return value;
        }
    }
    return kCGBlendModeNormal;
}

// REM2 -- an instance draws the other composition on its own canvas, mapped
// into its box, and clipped to that canvas as a precomposition is.
bool DrawInstance(CGContextRef context, const MotionEvaluatedNode& node,
                  MotionCoreTextMeasurer::Impl& impl, MotionError& error) {
    if (node.instance_root.empty()) {
        return true;
    }
    const MotionInstanceState& instance = node.instance;
    CGContextSaveGState(context);
    CGContextClipToRect(context,
                        CGRectMake(instance.origin_x, instance.origin_y,
                                   instance.width * instance.scale_x,
                                   instance.height * instance.scale_y));
    if (node.object_fit == "cover" || node.object_fit == "none") {
        CGContextClipToRect(context,
                            CGRectMake(node.box.x, node.box.y, node.box.width,
                                       node.box.height));
    }
    CGContextTranslateCTM(context, instance.origin_x, instance.origin_y);
    CGContextScaleCTM(context, instance.scale_x, instance.scale_y);
    const bool ok = DrawNode(context, node.instance_root.front(), impl, error);
    CGContextRestoreGState(context);
    return ok;
}

// REM3 -- a clip-path shape, or a transition's, as a path in the node's
// space.
void AddClipShape(CGContextRef context, const MotionResolvedClip& clip) {
    switch (clip.kind) {
        case MotionClipKind::Polygon:
            for (size_t index = 0; index < clip.points.size(); ++index) {
                if (index == 0) {
                    CGContextMoveToPoint(context, clip.points[index].first,
                                         clip.points[index].second);
                } else {
                    CGContextAddLineToPoint(context, clip.points[index].first,
                                            clip.points[index].second);
                }
            }
            CGContextClosePath(context);
            break;
        case MotionClipKind::Circle:
        case MotionClipKind::Ellipse:
            CGContextAddEllipseInRect(
                context, CGRectMake(clip.cx - clip.rx, clip.cy - clip.ry,
                                    2.0 * clip.rx, 2.0 * clip.ry));
            break;
        case MotionClipKind::Inset: {
            const CGRect rect = CGRectMake(clip.rect.x, clip.rect.y,
                                           clip.rect.width, clip.rect.height);
            const double radius = std::min(
                {clip.round, clip.rect.width / 2.0, clip.rect.height / 2.0});
            if (radius > 0.0) {
                const CFRef<CGPathRef> path(
                    CGPathCreateWithRoundedRect(rect, radius, radius, nullptr));
                CGContextAddPath(context, path.get());
            } else {
                CGContextAddRect(context, rect);
            }
            break;
        }
        case MotionClipKind::None:
            break;
    }
}

// The clips a node applies to itself and its descendants: its clip-path,
// then a transition's wipe or iris. Both move with its transforms.
void ClipNode(CGContextRef context, const MotionEvaluatedNode& node) {
    for (const MotionResolvedClip* clip :
         {node.has_clip ? &node.clip_shape : nullptr, &node.transition_clip}) {
        if (clip == nullptr || clip->kind == MotionClipKind::None) {
            continue;
        }
        CGContextBeginPath(context);
        AddClipShape(context, *clip);
        CGContextClip(context);
    }
}

// CSS mixes gradient colours with premultiplied alpha: from transparent to
// red, the middle is a translucent red, never a dark one. Core Graphics
// mixes straight components, so each segment is cut into sixteen steps
// mixed here.
CFRef<CGGradientRef> MakeGradient(const MotionResolvedGradient& gradient,
                                  bool coverageOnly) {
    std::vector<CGFloat> components;
    std::vector<CGFloat> locations;
    const auto push = [&](double position, const MotionColor& color) {
        locations.push_back(std::clamp(position, 0.0, 1.0));
        if (coverageOnly) {
            components.push_back(color.alpha);
            components.push_back(1.0);
        } else {
            components.insert(components.end(), {color.red, color.green,
                                                 color.blue, color.alpha});
        }
    };
    const auto& stops = gradient.stops;
    for (size_t index = 0; index < stops.size(); ++index) {
        if (index == 0) {
            push(stops[0].first, stops[0].second);
            continue;
        }
        const MotionColor& from = stops[index - 1].second;
        const MotionColor& to = stops[index].second;
        constexpr int kSteps = 16;
        for (int step = 1; step <= kSteps; ++step) {
            const double t = static_cast<double>(step) / kSteps;
            const double alpha = from.alpha + (to.alpha - from.alpha) * t;
            const auto channel = [&](double a, double b) {
                const double mixed =
                    a * from.alpha + (b * to.alpha - a * from.alpha) * t;
                return alpha > 0.0 ? std::clamp(mixed / alpha, 0.0, 1.0) : 0.0;
            };
            const MotionColor color{channel(from.red, to.red),
                                    channel(from.green, to.green),
                                    channel(from.blue, to.blue), alpha};
            push(stops[index - 1].first +
                     (stops[index].first - stops[index - 1].first) * t,
                 color);
        }
    }
    const CFRef<CGColorSpaceRef> space(CGColorSpaceCreateWithName(
        coverageOnly ? kCGColorSpaceGenericGrayGamma2_2 : kCGColorSpaceSRGB));
    return CFRef<CGGradientRef>(CGGradientCreateWithColorComponents(
        space.get(), components.data(), locations.data(), locations.size()));
}

// A resolved gradient over the whole current clip.
void DrawGradient(CGContextRef context, const MotionResolvedGradient& gradient,
                  bool coverageOnly) {
    const CFRef<CGGradientRef> made = MakeGradient(gradient, coverageOnly);
    if (!made) {
        return;
    }
    const CGGradientDrawingOptions extend =
        kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation;
    if (gradient.kind == MotionGradientKind::Linear) {
        CGContextDrawLinearGradient(
            context, made.get(), CGPointMake(gradient.x0, gradient.y0),
            CGPointMake(gradient.x1, gradient.y1), extend);
        return;
    }
    if (gradient.x1 <= 0.0 || gradient.y1 <= 0.0) {
        return;
    }
    CGContextSaveGState(context);
    CGContextTranslateCTM(context, gradient.x0, gradient.y0);
    CGContextScaleCTM(context, 1.0, gradient.y1 / gradient.x1);
    CGContextDrawRadialGradient(context, made.get(), CGPointZero, 0.0,
                                CGPointZero, gradient.x1, extend);
    CGContextRestoreGState(context);
}

// REM3 -- mask-image: the gradient's coverage multiplies everything the
// node paints; outside its box, nothing shows (CSS's mask painting area).
void MaskNode(CGContextRef context, const MotionEvaluatedNode& node) {
    const CGAffineTransform ctm = CGContextGetCTM(context);
    const double scale =
        std::max(1.0, std::sqrt(std::abs(ctm.a * ctm.d - ctm.b * ctm.c)));
    const size_t width =
        static_cast<size_t>(std::max(1.0, std::ceil(node.box.width * scale)));
    const size_t height =
        static_cast<size_t>(std::max(1.0, std::ceil(node.box.height * scale)));
    const CFRef<CGColorSpaceRef> gray(CGColorSpaceCreateDeviceGray());
    std::vector<uint8_t> pixels(width * height, 0);
    const CFRef<CGContextRef> bitmap(CGBitmapContextCreate(
        pixels.data(), width, height, 8, width, gray.get(), kCGImageAlphaNone));
    if (!bitmap) {
        return;
    }
    // Rows from the top, box pixels scaled to the mask's.
    CGContextTranslateCTM(bitmap.get(), 0, static_cast<CGFloat>(height));
    CGContextScaleCTM(bitmap.get(), width / node.box.width,
                      -(height / node.box.height));
    CGContextTranslateCTM(bitmap.get(), -node.box.x, -node.box.y);
    DrawGradient(bitmap.get(), node.mask, true);
    const CFRef<CGImageRef> mask(CGBitmapContextCreateImage(bitmap.get()));
    if (!mask) {
        return;
    }
    // The context is flipped: un-flip around the box for the image, then
    // flip back, keeping the clip.
    CGContextTranslateCTM(context, node.box.x, node.box.y + node.box.height);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextClipToMask(
        context, CGRectMake(0, 0, node.box.width, node.box.height), mask.get());
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextTranslateCTM(context, -node.box.x,
                          -(node.box.y + node.box.height));
}

MotionAffine FromCg(const CGAffineTransform& transform) {
    MotionAffine affine;
    affine.a = transform.a;
    affine.b = transform.b;
    affine.c = transform.c;
    affine.d = transform.d;
    affine.tx = transform.tx;
    affine.ty = transform.ty;
    return affine;
}

CGAffineTransform ToCg(const MotionAffine& affine) {
    return CGAffineTransformMake(affine.a, affine.b, affine.c, affine.d,
                                 affine.tx, affine.ty);
}

// REM3 -- a node with effects: drawn alone into a layer of frame pixels
// covering what its effects may write (its filter_domain, cut to the frame
// widened by their reach), the effects applied there (MotionEffects.h),
// then laid back through its clip, its mask and its opacity -- CSS's order:
// filter, clip-path, mask, opacity.
bool DrawFiltered(CGContextRef context, const MotionEvaluatedNode& node,
                  MotionCoreTextMeasurer::Impl& impl, MotionError& error) {
    // The context's own pixels, rows from the top: the frame's, or a layer's
    // when the node sits in one (REM4). The domain is worked out here from
    // them, by the evaluator's rule, so that it holds in either.
    const double frameWidth =
        static_cast<double>(CGBitmapContextGetWidth(context));
    const double frameHeight =
        static_cast<double>(CGBitmapContextGetHeight(context));
    const MotionAffine base =
        FromCg(CGAffineTransformMake(1, 0, 0, -1, 0, frameHeight));
    const MotionAffine userToFrame =
        base.Inverse().After(FromCg(CGContextGetCTM(context)));
    const MotionAffine nodeToFrame =
        userToFrame.After(MotionNodeTransform(node));
    const double reach =
        MotionFilterReach(node.filters) * nodeToFrame.LengthScale();
    const MotionBox bounds = MotionSubtreeBounds(node, userToFrame);
    const MotionBox domain{bounds.x - reach, bounds.y - reach,
                           bounds.width + 2.0 * reach,
                           bounds.height + 2.0 * reach};
    const double left = std::floor(std::max(domain.x, -reach));
    const double top = std::floor(std::max(domain.y, -reach));
    const double right =
        std::ceil(std::min(domain.x + domain.width, frameWidth + reach));
    const double bottom =
        std::ceil(std::min(domain.y + domain.height, frameHeight + reach));
    if (right <= left || bottom <= top) {
        return true;
    }
    const auto width = static_cast<size_t>(right - left);
    const auto height = static_cast<size_t>(bottom - top);
    if (width * height > static_cast<size_t>(16) * 4096 * 4096) {
        return Fail(error, "RenderFailed", node.path,
                    "the effects of this node cover too many pixels");
    }
    std::vector<uint8_t> pixels(width * height * 4, 0);
    const CFRef<CGColorSpaceRef> space(
        CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    const CFRef<CGContextRef> layer(CGBitmapContextCreate(
        pixels.data(), width, height, 8, width * 4, space.get(),
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
            kCGBitmapByteOrder32Big));
    if (!layer) {
        return Fail(error, "RenderFailed", node.path,
                    "cannot create the layer of the node's effects");
    }
    // The node's parent space onto the layer: into frame pixels, shifted to
    // the layer's corner, rows from the top.
    MotionAffine shift;
    shift.tx = -left;
    shift.ty = -top;
    MotionAffine flip;
    flip.d = -1.0;
    flip.ty = static_cast<double>(height);
    CGContextConcatCTM(layer.get(), ToCg(flip.After(shift).After(userToFrame)));
    CGContextSetShouldAntialias(layer.get(), true);
    CGContextSetAllowsFontSmoothing(layer.get(), false);
    CGContextSetShouldSmoothFonts(layer.get(), false);
    CGContextSetShouldSubpixelPositionFonts(layer.get(), true);
    CGContextSetShouldSubpixelQuantizeFonts(layer.get(), false);
    MotionEvaluatedNode bare = node;
    bare.opacity = 1.0;
    bare.has_clip = false;
    bare.clip_shape = MotionResolvedClip{};
    bare.transition_clip = MotionResolvedClip{};
    bare.has_mask = false;
    bare.blend_plus_lighter = false;
    if (!DrawNode(layer.get(), bare, impl, error, false)) {
        return false;
    }
    MotionLayer effected;
    effected.width = static_cast<int32_t>(width);
    effected.height = static_cast<int32_t>(height);
    // MA7 -- the corner of the node's painted area, which the frame's left
    // or top edge may have cut off the layer.
    // A node so large that the corner lies past 2^53 pixels is held there.
    const auto corner = [](double at, double cut) {
        return static_cast<int64_t>(
            std::clamp(std::floor(at) - cut, -9007199254740992.0, 0.0));
    };
    effected.origin_x = corner(domain.x, left);
    effected.origin_y = corner(domain.y, top);
    effected.rgba.resize(pixels.size());
    for (size_t at = 0; at < pixels.size(); ++at) {
        effected.rgba[at] = pixels[at] / 255.0f;
    }
    // Into the layer's own pixels, as MotionEffects.h asks: the effects that
    // only scale lengths read the linear part, mesh() needs the whole map.
    ApplyMotionFilters(effected, node.filters, shift.After(nodeToFrame));
    for (size_t at = 0; at < pixels.size(); at += 4) {
        const float alpha = std::clamp(effected.rgba[at + 3], 0.0f, 1.0f);
        pixels[at + 3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
        for (int channel = 0; channel < 3; ++channel) {
            // Premultiplied: a channel never exceeds its alpha.
            const float value =
                std::clamp(effected.rgba[at + channel], 0.0f, alpha);
            pixels[at + channel] =
                static_cast<uint8_t>(std::lround(value * 255.0f));
        }
    }
    const CFRef<CGImageRef> image(CGBitmapContextCreateImage(layer.get()));
    if (!image) {
        return Fail(error, "RenderFailed", node.path,
                    "cannot make an image of the node's effects");
    }
    CGContextSaveGState(context);
    TransformNode(context, node);
    ClipNode(context, node);
    if (node.has_mask) {
        MaskNode(context, node);
    }
    // Back to frame pixels to lay the layer where it was drawn.
    const MotionAffine now =
        base.Inverse().After(FromCg(CGContextGetCTM(context)));
    CGContextConcatCTM(context, ToCg(now.Inverse()));
    CGContextSetAlpha(context, node.opacity);
    if (node.blend_plus_lighter) {
        CGContextSetBlendMode(context, kCGBlendModePlusLighter);
    }
    CGContextTranslateCTM(context, left, bottom);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextDrawImage(context,
                       CGRectMake(0, 0, static_cast<CGFloat>(width),
                                  static_cast<CGFloat>(height)),
                       image.get());
    CGContextRestoreGState(context);
    return true;
}

// REM4 -- a node turned in depth: drawn flat, in its own plane, into a
// layer (effects, clip, mask and opacity included, since they live in that
// plane), then laid onto the context through the projective map of its
// plane -- each pixel covered fetched back through the inverse map,
// bilinear, as CSS flattens a transformed element.
bool DrawProjected(CGContextRef context, const MotionEvaluatedNode& node,
                   MotionCoreTextMeasurer::Impl& impl, MotionError& error) {
    const double contextWidth =
        static_cast<double>(CGBitmapContextGetWidth(context));
    const double contextHeight =
        static_cast<double>(CGBitmapContextGetHeight(context));
    const MotionAffine base =
        FromCg(CGAffineTransformMake(1, 0, 0, -1, 0, contextHeight));
    const MotionAffine userToPixels =
        base.Inverse().After(FromCg(CGContextGetCTM(context)));
    const MotionHomography plane =
        MotionHomography::FromAffine(userToPixels)
            .After(MotionPlaneHomography(node.matrix3d));
    // The node in its own plane, descendants and effects included.
    MotionEvaluatedNode flat = node;
    flat.translate_x = flat.translate_y = 0.0;
    flat.rotate = flat.rotate_x = flat.rotate_y = 0.0;
    flat.skew_x = flat.skew_y = 0.0;
    flat.scale_x = flat.scale_y = 1.0;
    flat.has_3d = false;
    flat.blend_plus_lighter = false;
    // MA7 -- docs/roadmap/MOTION-AFTER-2026-09.md. Its mode is applied by
    // DrawNode around this call, once. Inside this layer there is nothing
    // under the node to blend with: a mask-in there kept nothing, and laid
    // back, erased the whole group under the matte.
    flat.blend_mode.clear();
    const MotionBox own = MotionSubtreeBounds(flat, MotionAffine{});
    const double reach = MotionFilterReach(node.filters);
    const MotionBox local{own.x - reach - 1.0, own.y - reach - 1.0,
                          own.width + 2.0 * reach + 2.0,
                          own.height + 2.0 * reach + 2.0};
    // As many layer pixels per unit as the projection stretches it most.
    double density = 1.0;
    const double xs[] = {local.x, local.x + local.width};
    const double ys[] = {local.y, local.y + local.height};
    double left = contextWidth;
    double top = contextHeight;
    double right = 0.0;
    double bottom = 0.0;
    for (const double x : xs) {
        for (const double y : ys) {
            double px = 0.0;
            double py = 0.0;
            density = std::max(density, plane.LocalScale(x, y));
            if (plane.Apply(x, y, px, py)) {
                left = std::min(left, px);
                top = std::min(top, py);
                right = std::max(right, px);
                bottom = std::max(bottom, py);
            } else {
                // A corner behind the viewer: the whole context may show it.
                left = top = 0.0;
                right = contextWidth;
                bottom = contextHeight;
            }
        }
    }
    density = std::min(density, 4.0);
    left = std::max(0.0, std::floor(left));
    top = std::max(0.0, std::floor(top));
    right = std::min(contextWidth, std::ceil(right));
    bottom = std::min(contextHeight, std::ceil(bottom));
    if (right <= left || bottom <= top) {
        return true;
    }
    const auto layerWidth =
        static_cast<size_t>(std::ceil(local.width * density));
    const auto layerHeight =
        static_cast<size_t>(std::ceil(local.height * density));
    if (layerWidth * layerHeight > static_cast<size_t>(16) * 4096 * 4096) {
        return Fail(error, "RenderFailed", node.path,
                    "this node turned in depth covers too many pixels");
    }
    std::vector<uint8_t> flatPixels(layerWidth * layerHeight * 4, 0);
    const CFRef<CGColorSpaceRef> space(
        CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    const CFRef<CGContextRef> layer(CGBitmapContextCreate(
        flatPixels.data(), layerWidth, layerHeight, 8, layerWidth * 4,
        space.get(),
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
            kCGBitmapByteOrder32Big));
    if (!layer) {
        return Fail(error, "RenderFailed", node.path,
                    "cannot create the layer of a node turned in depth");
    }
    CGContextTranslateCTM(layer.get(), 0, static_cast<CGFloat>(layerHeight));
    CGContextScaleCTM(layer.get(), density, -density);
    CGContextTranslateCTM(layer.get(), -local.x, -local.y);
    CGContextSetShouldAntialias(layer.get(), true);
    CGContextSetAllowsFontSmoothing(layer.get(), false);
    CGContextSetShouldSmoothFonts(layer.get(), false);
    CGContextSetShouldSubpixelPositionFonts(layer.get(), true);
    CGContextSetShouldSubpixelQuantizeFonts(layer.get(), false);
    if (!DrawNode(layer.get(), flat, impl, error)) {
        return false;
    }
    // Each covered pixel, fetched back into the plane.
    const MotionHomography inverse = plane.Inverse();
    const auto outWidth = static_cast<size_t>(right - left);
    const auto outHeight = static_cast<size_t>(bottom - top);
    std::vector<uint8_t> warped(outWidth * outHeight * 4, 0);
    const auto fetch = [&](int64_t x, int64_t y, int channel) -> double {
        if (x < 0 || y < 0 || x >= static_cast<int64_t>(layerWidth) ||
            y >= static_cast<int64_t>(layerHeight)) {
            return 0.0;
        }
        return flatPixels[(static_cast<size_t>(y) * layerWidth +
                           static_cast<size_t>(x)) *
                              4 +
                          static_cast<size_t>(channel)];
    };
    for (size_t row = 0; row < outHeight; ++row) {
        for (size_t column = 0; column < outWidth; ++column) {
            double u = 0.0;
            double v = 0.0;
            if (!inverse.Apply(left + column + 0.5, top + row + 0.5, u, v)) {
                continue;
            }
            const double lx = (u - local.x) * density - 0.5;
            const double ly = (v - local.y) * density - 0.5;
            const double fx = std::floor(lx);
            const double fy = std::floor(ly);
            const double tx = lx - fx;
            const double ty = ly - fy;
            const auto ix = static_cast<int64_t>(fx);
            const auto iy = static_cast<int64_t>(fy);
            uint8_t* out = &warped[(row * outWidth + column) * 4];
            for (int channel = 0; channel < 4; ++channel) {
                const double value =
                    fetch(ix, iy, channel) * (1 - tx) * (1 - ty) +
                    fetch(ix + 1, iy, channel) * tx * (1 - ty) +
                    fetch(ix, iy + 1, channel) * (1 - tx) * ty +
                    fetch(ix + 1, iy + 1, channel) * tx * ty;
                out[channel] = static_cast<uint8_t>(
                    std::lround(std::clamp(value, 0.0, 255.0)));
            }
        }
    }
    const CFRef<CFDataRef> data(
        CFDataCreate(kCFAllocatorDefault, warped.data(),
                     static_cast<CFIndex>(warped.size())));
    const CFRef<CGDataProviderRef> provider(
        CGDataProviderCreateWithCFData(data.get()));
    const CFRef<CGImageRef> image(CGImageCreate(
        outWidth, outHeight, 8, 32, outWidth * 4, space.get(),
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
            kCGBitmapByteOrder32Big,
        provider.get(), nullptr, false, kCGRenderingIntentDefault));
    if (!image) {
        return Fail(error, "RenderFailed", node.path,
                    "cannot make an image of a node turned in depth");
    }
    CGContextSaveGState(context);
    CGContextConcatCTM(context, ToCg(userToPixels.Inverse()));
    if (node.blend_plus_lighter) {
        CGContextSetBlendMode(context, kCGBlendModePlusLighter);
    }
    CGContextTranslateCTM(context, left, bottom);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextDrawImage(context,
                       CGRectMake(0, 0, static_cast<CGFloat>(outWidth),
                                  static_cast<CGFloat>(outHeight)),
                       image.get());
    CGContextRestoreGState(context);
    return true;
}

bool DrawNode(CGContextRef context, const MotionEvaluatedNode& node,
              MotionCoreTextMeasurer::Impl& impl, MotionError& error,
              bool effects, bool blended) {
    if (node.opacity <= 0.0 || node.scale_x == 0.0 || node.scale_y == 0.0) {
        return true;
    }
    // MOTION-DESIGN-2026-09 -- mix-blend-mode: the node, by whichever path
    // draws it (effects, depth, plain), as one group composited onto what
    // lies under it by its mode. Not again inside an effect's offscreen
    // (effects false), where there is nothing under it to blend with.
    if (effects && !blended && !node.blend_mode.empty()) {
        CGContextSaveGState(context);
        CGContextSetBlendMode(context, BlendModeNamed(node.blend_mode));
        CGContextBeginTransparencyLayer(context, nullptr);
        CGContextSetBlendMode(context, kCGBlendModeNormal);
        const bool ok = DrawNode(context, node, impl, error, true, true);
        CGContextEndTransparencyLayer(context);
        CGContextRestoreGState(context);
        return ok;
    }
    if (node.has_3d) {
        return DrawProjected(context, node, impl, error);
    }
    if (effects && !node.filters.empty()) {
        return DrawFiltered(context, node, impl, error);
    }
    CGContextSaveGState(context);
    // CSS individual transforms apply translate, then rotate, then scale,
    // around the centre of the box (MW5: skew before the scale, about the
    // transform origin).
    TransformNode(context, node);

    // M1b -- clip-path moves with the node's transforms and clips it and its
    // descendants. REM3 -- every shape, and a transition's wipe or iris.
    ClipNode(context, node);
    // REM3 -- then the mask, then opacity.
    if (node.has_mask) {
        MaskNode(context, node);
    }

    // CSS opacity composites the element and its descendants as one group.
    // REM3 -- so does a fade's entering scene, added onto the exiting one
    // (plus-lighter), and a series running a transition isolates the two
    // so that nothing under it is added to.
    const bool group = node.opacity < 1.0 || node.blend_plus_lighter ||
                       node.transition.active || node.isolate;
    if (group) {
        CGContextSetAlpha(context, node.opacity);
        if (node.blend_plus_lighter) {
            CGContextSetBlendMode(context, kCGBlendModePlusLighter);
        }
        CGContextBeginTransparencyLayer(context, nullptr);
        CGContextSetBlendMode(context, kCGBlendModeNormal);
        CGContextSetAlpha(context, 1.0);
    }
    if (node.has_background) {
        SetFill(context, node.background);
        CGContextBeginPath(context);
        AddRoundedBox(context, node);
        CGContextFillPath(context);
    }
    // REM3 -- a background gradient, over the colour, inside the rounded box.
    if (node.has_gradient) {
        CGContextSaveGState(context);
        CGContextBeginPath(context);
        AddRoundedBox(context, node);
        CGContextClip(context);
        DrawGradient(context, node.gradient, false);
        CGContextRestoreGState(context);
    }
    // REM2 -- overflow: hidden clips what the box holds to its rounded box.
    if (node.overflow_hidden) {
        CGContextBeginPath(context);
        AddRoundedBox(context, node);
        CGContextClip(context);
    }

    bool ok = true;
    if (node.type == MotionNodeType::Text && !node.text_layout.lines.empty()) {
        // MB3 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. Highlighter
        // bands under the glyphs, over the box's background, each filled
        // from its left up to the width its progress reached.
        for (const MotionHighlightBand& band : node.highlights) {
            if (band.painted <= 0.0 || band.box.height <= 0.0) {
                continue;
            }
            SetFill(context, band.color);
            CGContextFillRect(
                context,
                CGRectMake(node.box.x + band.box.x, node.box.y + band.box.y,
                           band.painted, band.box.height));
        }
        const CTFontRef font = impl.Font(node.text_style, error);
        ok = font != nullptr;
        if (ok && node.unit_kind != MotionTextUnit::None) {
            DrawUnits(context, node, impl, font);
        } else if (ok) {
            // The context is flipped to rows-from-the-top; glyphs are not.
            CGContextSetTextMatrix(context, CGAffineTransformMakeScale(1, -1));
            for (const MotionTextLine& line : node.text_layout.lines) {
                DrawGlyphs(context, impl, font, node.text_style, line.text,
                           node.text_style.color, node.box.x + line.x,
                           node.box.y + line.baseline);
            }
        }
    } else if (node.type == MotionNodeType::Path) {
        DrawPath(context, node.path_paint);
    } else if (node.type == MotionNodeType::Image) {
        const CGImageRef image = impl.Image(node.image_source, error);
        ok = image != nullptr;
        if (ok) {
            DrawPicture(context, node, image);
        }
    } else if (node.type == MotionNodeType::Video) {
        ok = DrawVideo(context, node, impl, error);
    } else if (node.type == MotionNodeType::Composition) {
        ok = DrawInstance(context, node, impl, error);
    }
    if (!ok && error.path.empty()) {
        error.path = node.path;
    }
    for (const MotionEvaluatedNode& child : node.children) {
        if (!ok) {
            break;
        }
        ok = DrawNode(context, child, impl, error);
    }
    if (group) {
        CGContextEndTransparencyLayer(context);
    }
    CGContextRestoreGState(context);
    return ok;
}

}  // namespace

namespace {

// One tree of a frame into premultiplied RGBA of the frame's size.
bool PaintTree(const MotionFrame& frame, const MotionEvaluatedNode& root,
               MotionCoreTextMeasurer& measurer, size_t bytes,
               std::vector<uint8_t>& pixels, MotionError& error) {
    pixels.assign(bytes, 0);
    const CFRef<CGColorSpaceRef> space(
        CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    const CFRef<CGContextRef> context(CGBitmapContextCreate(
        pixels.data(), static_cast<size_t>(frame.width),
        static_cast<size_t>(frame.height), 8,
        static_cast<size_t>(frame.width) * 4, space.get(),
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
            kCGBitmapByteOrder32Big));
    if (!context) {
        return Fail(error, "RenderFailed", "frame",
                    "cannot create a bitmap context");
    }
    // Rows from the top, like the evaluated boxes. Font smoothing depends on
    // the display; a rendered frame must not.
    CGContextTranslateCTM(context.get(), 0, frame.height);
    CGContextScaleCTM(context.get(), 1, -1);
    CGContextSetShouldAntialias(context.get(), true);
    CGContextSetAllowsFontSmoothing(context.get(), false);
    CGContextSetShouldSmoothFonts(context.get(), false);
    CGContextSetShouldSubpixelPositionFonts(context.get(), true);
    CGContextSetShouldSubpixelQuantizeFonts(context.get(), false);
    return DrawNode(context.get(), root, measurer.impl(), error);
}

// PERF11 -- the pictures DrawNode will ask DrawVideo for, under this node:
// the same nodes, skipped for the same reasons.
void CollectVideos(const MotionEvaluatedNode& node,
                   const MotionCoreTextMeasurer::Impl& impl,
                   std::set<MotionCoreTextMeasurer::Impl::VideoKey>& wanted) {
    if (node.opacity <= 0.0 || node.scale_x == 0.0 || node.scale_y == 0.0) {
        return;
    }
    if (node.type == MotionNodeType::Video && node.media.state == "playing") {
        std::string path;
        MotionError ignored;
        if (impl.MediaPath(node.media.src, node.media.media, path, ignored)) {
            wanted.emplace(std::move(path), node.media.frame);
        }
    } else if (node.type == MotionNodeType::Composition &&
               !node.instance_root.empty()) {
        CollectVideos(node.instance_root.front(), impl, wanted);
    }
    for (const MotionEvaluatedNode& child : node.children) {
        CollectVideos(child, impl, wanted);
    }
}

// PERF11 -- the frame's video pictures, each decoded and converted once
// before anything is painted. Painting used to read a video node's picture
// every time it drew it: a frame with motion blur draws it at each instant
// of the shutter, and three bands of one rush draw it three times, so a 4K
// picture was converted up to fifty times for one frame.
//
// Decoding is done in this frame's turn, in the order of the file; the
// conversions follow once the turn is given up, so that the frames rendered
// at the same time convert together. A picture that cannot be read is left
// to DrawVideo, which reports it as it always did.
void PrepareVideos(const MotionFrame& frame,
                   MotionCoreTextMeasurer::Impl& impl) {
    using VideoKey = MotionCoreTextMeasurer::Impl::VideoKey;
    std::set<VideoKey> wanted;
    CollectVideos(frame.root, impl, wanted);
    for (const MotionEvaluatedNode& root : frame.motion_blur) {
        CollectVideos(root, impl, wanted);
    }
    std::vector<std::pair<VideoKey, MotionDecodedPicture>> decoded;
    if (!wanted.empty()) {
        size_t bytes = 0;
        std::string code;
        std::string message;
        for (const VideoKey& key : wanted) {
            MotionMediaInfo info;
            if (ProbeMotionMedia(key.first, info, code, message)) {
                bytes += static_cast<size_t>(info.width) *
                         static_cast<size_t>(info.height) * 4;
            }
        }
        if (impl.turns != nullptr) {
            impl.turns->Enter(impl.turn);
            Budget().Acquire(bytes);
        } else {
            Budget().Charge(bytes);
        }
        impl.video_bytes += bytes;
        // Sorted by file, then by picture: each file is read forward.
        for (const VideoKey& key : wanted) {
            MotionDecodedPicture picture;
            if (DecodeMotionVideoFrame(key.first, key.second, picture, code,
                                       message)) {
                decoded.emplace_back(key, std::move(picture));
            }
        }
    }
    if (impl.turns != nullptr) {
        impl.turns->Leave(impl.turn);
    }
    for (auto& entry : decoded) {
        MotionVideoPicture picture;
        std::string code;
        std::string message;
        if (!ConvertMotionVideoFrame(entry.second, picture, code, message)) {
            continue;
        }
        entry.second.pixels.reset();
        CFRef<CGImageRef> image = MakeVideoImage(std::move(picture));
        if (image) {
            impl.videos.emplace(entry.first, std::move(image));
        }
    }
}

// Lets go of the frame's pictures however the frame ends.
struct FrameVideos {
    MotionCoreTextMeasurer::Impl& impl;
    ~FrameVideos() {
        impl.videos.clear();
        Budget().Release(impl.video_bytes);
        impl.video_bytes = 0;
    }
};

}  // namespace

bool RenderMotionFrame(const MotionFrame& frame,
                       MotionCoreTextMeasurer& measurer, MotionRaster& raster,
                       MotionError& error) {
    size_t bytes = 0;
    if (!RasterSize(frame.width, frame.height, bytes)) {
        return Fail(error, "InvalidValue", "frame", "frame has no size");
    }
    const FrameVideos videos{measurer.impl()};
    PrepareVideos(frame, measurer.impl());
    MotionRaster output;
    output.width = frame.width;
    output.height = frame.height;
    if (!PaintTree(frame, frame.root, measurer, bytes, output.pixels, error)) {
        return false;
    }
    // MOTION-DESIGN-2026-09 -- motion blur: every instant of the shutter
    // painted and averaged in premultiplied light, as film exposes it.
    if (!frame.motion_blur.empty()) {
        std::vector<uint32_t> sums(output.pixels.begin(), output.pixels.end());
        std::vector<uint8_t> instant;
        for (const MotionEvaluatedNode& root : frame.motion_blur) {
            if (!PaintTree(frame, root, measurer, bytes, instant, error)) {
                return false;
            }
            for (size_t index = 0; index < bytes; ++index) {
                sums[index] += instant[index];
            }
        }
        const auto count = static_cast<uint32_t>(frame.motion_blur.size() + 1);
        for (size_t index = 0; index < bytes; ++index) {
            output.pixels[index] =
                static_cast<uint8_t>((sums[index] + count / 2) / count);
        }
    }
    // MOTION-DESIGN-2026-09 -- the composition's grain, once, over the
    // finished frame; its own every frame.
    if (frame.grain_amount > 0.0) {
        const double cell = std::max(1.0, frame.grain_size);
        for (int32_t y = 0; y < output.height; ++y) {
            for (int32_t x = 0; x < output.width; ++x) {
                uint8_t* pixel =
                    &output.pixels[(static_cast<size_t>(y) * output.width + x) *
                                   4];
                if (pixel[3] == 0) {
                    continue;
                }
                const double noise =
                    frame.grain_amount * pixel[3] *
                    MotionNoiseCell(static_cast<int64_t>(x / cell),
                                    static_cast<int64_t>(y / cell), frame.frame,
                                    frame.grain_seed);
                for (int channel = 0; channel < 3; ++channel) {
                    pixel[channel] = static_cast<uint8_t>(std::clamp<long>(
                        std::lround(pixel[channel] + noise), 0L, pixel[3]));
                }
            }
        }
    }
    raster = std::move(output);
    return true;
}

// MOTION-2026-09 M3 -- the PNG in memory, for a tool that returns the
// picture rather than a file (render_motion_frame). WriteMotionPng writes the
// same bytes.
bool EncodeMotionPng(const MotionRaster& raster, std::string& png,
                     MotionError& error) {
    size_t bytes = 0;
    if (!RasterSize(raster.width, raster.height, bytes) ||
        raster.pixels.size() != bytes) {
        return Fail(error, "InvalidValue", "raster",
                    "raster dimensions do not match its RGBA pixels");
    }
    const CFRef<CGColorSpaceRef> space(
        CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    const CFRef<CFDataRef> data(
        CFDataCreate(kCFAllocatorDefault, raster.pixels.data(),
                     static_cast<CFIndex>(raster.pixels.size())));
    const CFRef<CGDataProviderRef> provider(
        CGDataProviderCreateWithCFData(data.get()));
    const CFRef<CGImageRef> image(CGImageCreate(
        static_cast<size_t>(raster.width), static_cast<size_t>(raster.height),
        8, 32, static_cast<size_t>(raster.width) * 4, space.get(),
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
            kCGBitmapByteOrder32Big,
        provider.get(), nullptr, false, kCGRenderingIntentDefault));
    const CFRef<CFMutableDataRef> encoded(
        CFDataCreateMutable(kCFAllocatorDefault, 0));
    const CFRef<CGImageDestinationRef> destination(
        image && encoded ? CGImageDestinationCreateWithData(
                               encoded.get(), CFSTR("public.png"), 1, nullptr)
                         : nullptr);
    if (!destination) {
        return Fail(error, "IoError", "image", "cannot create a PNG");
    }
    CGImageDestinationAddImage(destination.get(), image.get(), nullptr);
    if (!CGImageDestinationFinalize(destination.get())) {
        return Fail(error, "IoError", "image", "cannot encode the PNG");
    }
    png.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(encoded.get())),
               static_cast<size_t>(CFDataGetLength(encoded.get())));
    return true;
}

bool WriteMotionPng(const MotionRaster& raster, const std::string& path,
                    MotionError& error) {
    size_t bytes = 0;
    if (!RasterSize(raster.width, raster.height, bytes) ||
        raster.pixels.size() != bytes) {
        return Fail(error, "InvalidValue", "raster",
                    "raster dimensions do not match its RGBA pixels");
    }
    std::error_code status;
    const std::filesystem::path target(path);
    if (std::filesystem::exists(target, status)) {
        return Fail(error, "IoError", "image_path",
                    "refusing to overwrite existing file '" + path + "'");
    }
    const std::filesystem::path parent =
        target.has_parent_path() ? target.parent_path() : ".";
    if (!std::filesystem::is_directory(parent, status)) {
        return Fail(error, "IoError", "image_path",
                    "directory does not exist for '" + path + "'");
    }
    // Encode before opening the file. Exclusive creation also protects
    // against concurrent renders and dangling symlinks, unlike exists().
    std::string png;
    if (!EncodeMotionPng(raster, png, error)) {
        error.path = "image_path";
        error.message += " for '" + path + "'";
        return false;
    }
    const int file =
        open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (file < 0) {
        return Fail(
            error, "IoError", "image_path",
            errno == EEXIST
                ? "refusing to overwrite existing file '" + path + "'"
                : "cannot create PNG '" + path + "': " + std::strerror(errno));
    }
    const auto* dataBytes = reinterpret_cast<const UInt8*>(png.data());
    const size_t length = png.size();
    size_t offset = 0;
    int failure = 0;
    while (offset < length) {
        const ssize_t count = write(file, dataBytes + offset, length - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            failure = count < 0 ? errno : EIO;
            break;
        }
        offset += static_cast<size_t>(count);
    }
    if (close(file) != 0 && failure == 0) {
        failure = errno;
    }
    if (failure != 0) {
        unlink(path.c_str());
        return Fail(
            error, "IoError", "image_path",
            "cannot write PNG '" + path + "': " + std::strerror(failure));
    }
    return true;
}
