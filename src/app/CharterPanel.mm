// CH6 -- docs/roadmap/CHARTE-2026-11.md. The Charte panel (CharterPanel.h):
// the window, its three columns, the reads and writes of the library on a
// queue of their own, the previews rendered off the main thread, the
// gestures and the fields relayed to the pure policies of Charter*.h, and
// the watch on kits/. Frames laid out by hand on every resize (no Auto
// Layout), colours from UiTheme.h or from the charter itself.

#import "CharterPanel.h"

#import "CharterPreviewView.h"
#import "FormPanel.h"
#import "UiComponents.h"
#import "UiPreferences.h"
#import "UiThemeAppKit.h"

#include "CaptionRenderer.h"
#include "CharterPanelModel.h"
#include "CharterPreview.h"
#include "CharterWindow.h"
#include "Json.h"
#include "MotionCanvas.h"
#include "MotionFrameRenderer.h"
#include "MotionKitEdit.h"
#include "MotionSafeArea.h"
#include "MotionTemplates.h"
#include "UiTheme.h"

#import <ImageIO/ImageIO.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using ui::charter::AreaKind;
using ui::charter::Drag;
using ui::charter::DragContext;
using ui::charter::DragKind;
using ui::charter::Field;
using ui::charter::FieldKind;
using ui::charter::HitArea;
using ui::charter::PiecePreview;
using ui::charter::PreviewFrame;
using ui::charter::PreviewFrames;
using ui::charter::Quad;

namespace {

namespace fs = std::filesystem;

// The words of the subtitle the preview shows: a line of the length a
// subtitle has, in the charter's style.
constexpr char kSampleCaption[] = "Un sous-titre d’exemple";
// What a write is kept under in the base (MotionLibraryBase.h, source).
constexpr char kPanelSource[] = "panel";
// A handle is grabbed this near, in points.
constexpr double kHandleGrab = 6.0;
// The columns, in points.
constexpr CGFloat kLeftWidth = 204.0;
constexpr CGFloat kRightWidth = 330.0;
constexpr CGFloat kJsonHeight = 170.0;
constexpr CGFloat kNoticeHeight = 46.0;
// The label column of a field row.
constexpr CGFloat kFieldLabelWidth = 128.0;

NSString* Str(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

std::string Utf8(NSString* text) {
    return text.UTF8String != nullptr ? std::string(text.UTF8String)
                                      : std::string();
}

// A PNG the motion renderer wrote, as a CGImage held by ARC.
id ImageFromPng(const std::string& png) {
    if (png.empty()) return nil;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                  reinterpret_cast<const UInt8*>(png.data()),
                                  static_cast<CFIndex>(png.size()));
    CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (source == nullptr) return nil;
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    return image != nullptr ? (__bridge_transfer id)image : nil;
}

// The caption renderer's picture (premultiplied RGBA, rows from the top),
// as main.mm makes it for Record.
id ImageFromCaption(const CaptionImage& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.pixels.size() != static_cast<size_t>(image.width) *
                                   static_cast<size_t>(image.height) * 4)
        return nil;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, image.pixels.data(),
                                  static_cast<CFIndex>(image.pixels.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef picture = CGImageCreate(
        static_cast<size_t>(image.width), static_cast<size_t>(image.height), 8,
        32, static_cast<size_t>(image.width) * 4, space,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big, provider,
        nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);
    return picture != nullptr ? (__bridge_transfer id)picture : nil;
}

// A colour of the charter as AppKit draws it: a caption colour read as the
// caption renderer reads it ("#rrggbb@a"), any other as the motion format
// reads it.
NSColor* ColorOf(const std::string& text, const std::string& path) {
    if (text.empty()) return nil;
    if (path.rfind("captions", 0) == 0) return CMCaptionColor(text, nil);
    MotionColor color;
    if (!ParseMotionColor(text, color)) return nil;
    return [NSColor colorWithSRGBRed:color.red
                               green:color.green
                                blue:color.blue
                               alpha:color.alpha];
}

// The stand-in for a logo this machine does not have: drawn once, in the
// theme's greys, into the temporary folder (CharterPreview.h).
std::string PlaceholderLogo() {
    static std::string path;
    if (!path.empty()) return path;
    NSString* file = [NSTemporaryDirectory()
        stringByAppendingPathComponent:@"cutmachine-charte-substitut.png"];
    NSBitmapImageRep* bitmap =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                pixelsWide:480
                                                pixelsHigh:240
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSDeviceRGBColorSpace
                                               bytesPerRow:0
                                              bitsPerPixel:0];
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext =
        [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
    [[NSColor clearColor] setFill];
    NSRectFill(NSMakeRect(0, 0, 480, 240));
    [CMThemeColor(ui::theme::kTextTertiary) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(8, 8, 464, 224)
                                     xRadius:24
                                     yRadius:24] fill];
    NSDictionary* attributes = @{
        NSFontAttributeName : [NSFont systemFontOfSize:96
                                                weight:NSFontWeightBold],
        NSForegroundColorAttributeName : CMThemeColor(ui::theme::kSurfaceBase),
    };
    const NSSize size = [@"LOGO" sizeWithAttributes:attributes];
    [@"LOGO" drawAtPoint:NSMakePoint((480 - size.width) / 2.0,
                                     (240 - size.height) / 2.0)
          withAttributes:attributes];
    [NSGraphicsContext restoreGraphicsState];
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                       properties:@{}];
    if ([png writeToFile:file atomically:YES]) path = Utf8(file);
    return path;
}

bool ImageExists(const std::string& path) {
    if (path.empty() || path[0] != '/') return false;
    std::error_code error;
    return fs::is_regular_file(path, error);
}

// The kit files of kits/ by name, dot files left out (the engine's
// temporary files start with a dot): what a reload saw, to tell a new or
// removed charter from a rewrite of the one shown.
std::set<std::string> KitFileNames(const MotionLibraryLocation& location) {
    std::set<std::string> names;
    if (location.inbox.empty()) return names;
    std::error_code error;
    for (fs::directory_iterator
             entry(MotionKitDirectory(location.inbox), error),
         end;
         !error && entry != end; entry.increment(error)) {
        const std::string name = entry->path().filename().string();
        if (!name.empty() && name[0] != '.' &&
            entry->path().extension() == ".json")
            names.insert(name);
    }
    return names;
}

// -- The previews, rendered off the main thread.

struct RenderedPiece {
    std::string piece;
    // Kept alive and unmoved for the render and the drags after it: the
    // request and the drag context point into it (CharterPreview.h).
    std::unique_ptr<PiecePreview> preview;
    id image = nil;
    Quad quad{};
    bool anchored = false;
    std::string error;
};

struct RenderedFrame {
    size_t index = 0;
    PreviewFrame frame;
    double scale = 0.0;
    std::vector<RenderedPiece> pieces;
    id caption = nil;
    Quad caption_quad{};
    bool caption_shown = false;
    // The charter's caption format of that size; empty without one.
    std::string caption_format;
    // What the frame says instead of, or beside, its pictures.
    std::vector<std::string> notices;
};

using RenderedFrames = std::vector<RenderedFrame>;

struct RenderJob {
    MotionKitInfo kit;
    // In drawing order.
    std::vector<MotionTemplateInfo> pieces;
    bool captions = false;
    // PreviewFrames() index, and the scale it is drawn at.
    std::vector<std::pair<size_t, double>> frames;
    std::string placeholder;
};

void RenderCaption(const RenderJob& job, RenderedFrame& out) {
    const PreviewFrame& frame = out.frame;
    CaptionStyle style;
    std::vector<std::string> set;
    std::string error;
    if (!MotionKitCaptionStyle(job.kit, frame.width, frame.height, style,
                               out.caption_format, set, error)) {
        out.caption_format.clear();
        const mcp_json::Value* formats = job.kit.captions.IsObject()
                                             ? job.kit.captions.Find("formats")
                                             : nullptr;
        if (formats != nullptr)
            out.notices.push_back(
                "Pas de sous-titre en " + frame.label +
                " : la charte a des formats de sous-titre, et aucun de "
                "cette taille.");
        return;
    }
    const CaptionRenderer* renderer = CurrentCaptionRenderer();
    if (renderer == nullptr || renderer->draw == nullptr) {
        out.notices.push_back(kNoCaptionRenderer);
        return;
    }
    CaptionDrawing drawing;
    drawing.width = std::max<int32_t>(
        1, static_cast<int32_t>(std::lround(frame.width * out.scale)));
    drawing.height = std::max<int32_t>(
        1, static_cast<int32_t>(std::lround(frame.height * out.scale)));
    drawing.sequence_width = frame.width;
    CaptionCarton carton;
    carton.text = kSampleCaption;
    carton.style = style;
    const double drawn = static_cast<double>(drawing.width) / frame.width;
    const bool ok = renderer->draw(
        drawing, {{carton}},
        [&](size_t, CaptionImage& image) {
            out.caption = ImageFromCaption(image);
            if (!image.cartons.empty()) {
                const caption_layout::Rect& block =
                    image.cartons.front().placement.block;
                const double left = block.x / drawn;
                const double top = block.y / drawn;
                const double right = (block.x + block.width) / drawn;
                const double bottom = (block.y + block.height) / drawn;
                out.caption_quad = {{{left, top},
                                     {right, top},
                                     {right, bottom},
                                     {left, bottom}}};
                out.caption_shown = true;
            }
            return true;
        },
        error);
    if (!ok)
        out.notices.push_back("Le sous-titre ne se dessine pas : " + error);
}

// `stale` says a newer render was asked for: what is left is not drawn.
std::shared_ptr<RenderedFrames> Render(const RenderJob& job,
                                       const std::function<bool()>& stale) {
    auto frames = std::make_shared<RenderedFrames>();
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    for (const auto& [index, scale] : job.frames) {
        if (stale()) break;
        RenderedFrame out;
        out.index = index;
        out.frame = PreviewFrames()[index];
        out.scale = scale;
        for (const MotionTemplateInfo& piece : job.pieces) {
            RenderedPiece rendered;
            rendered.piece = piece.name;
            rendered.preview = std::make_unique<PiecePreview>();
            std::string error;
            if (!ui::charter::BuildPieceRequest(job.kit, piece, out.frame,
                                                job.placeholder, ImageExists,
                                                *rendered.preview, error)) {
                rendered.error = error;
                out.notices.push_back(error);
                out.pieces.push_back(std::move(rendered));
                continue;
            }
            for (const auto& [prop, path] : rendered.preview->substituted)
                out.notices.push_back(
                    "« " + prop + " » de la pièce « " + piece.name + " » : " +
                    (path.empty() ? std::string("aucun fichier")
                                  : "« " + path + " » absent de ce Mac") +
                    ", un substitut le remplace.");
            if (render == nullptr) {
                rendered.error = std::string("Pas de moteur de rendu : ") +
                                 kMotionRendererHolder + ".";
                out.notices.push_back(rendered.error);
                out.pieces.push_back(std::move(rendered));
                continue;
            }
            MotionFrameRequest request =
                ui::charter::PieceFrameRequest(*rendered.preview, scale);
            MotionFrameImage image;
            MotionError motion;
            if (!render(request, image, motion)) {
                rendered.error = "La pièce « " + piece.name +
                                 " » ne se dessine pas (" + motion.code +
                                 ") : " + motion.message;
                out.notices.push_back(rendered.error);
                out.pieces.push_back(std::move(rendered));
                continue;
            }
            rendered.image = ImageFromPng(image.png);
            rendered.anchored = ui::charter::AnchorQuad(
                image.evaluated, piece.name, scale, rendered.quad);
            out.pieces.push_back(std::move(rendered));
        }
        if (job.captions) RenderCaption(job, out);
        frames->push_back(std::move(out));
    }
    return frames;
}

// -- The library, read off the main thread.

struct ReadResult {
    std::vector<std::string> names;
    std::set<std::string> broken;
    std::string selected;
    bool has_state = false;
    MotionKitState state;
    std::vector<MotionKitVersion> versions;
    std::string error;
    std::set<std::string> kit_files;
};

// CH6 (review) -- the charter and the version a gesture began on: a press
// on the preview, the first key typed in a field, a colour well opened, a
// version restored. The write it ends in carries them, never what the
// panel shows by the time it ends: if the charter moved meanwhile, the
// engine says StaleKit.
struct GestureBase {
    std::string name;
    int64_t version = 0;
};

// The panel's own writes, in the order the queue makes them (on the queue
// only). A gesture that began on a version one of them replaced, before
// the panel showed the result, began on what the panel itself wrote: it is
// carried on to the version that write made, through as many of its own
// writes as followed. Nothing else is: a version the agent or a hand edit
// made in between breaks the chain, and the write is stale.
struct WriteChain {
    std::string name;
    // from -> to, oldest first.
    std::vector<std::pair<int64_t, int64_t>> links;
};

constexpr size_t kChainLinks = 64;

int64_t ExpectedFor(const WriteChain& chain, const std::string& name,
                    int64_t expected) {
    if (chain.name != name) return expected;
    int64_t used = expected;
    // Versions only grow: each step moves forward, and the walk ends.
    for (bool moved = true; moved;) {
        moved = false;
        for (const auto& [from, to] : chain.links) {
            if (from == used && to > used) {
                used = to;
                moved = true;
                break;
            }
        }
    }
    return used;
}

void RecordWrite(WriteChain& chain, const std::string& name, int64_t from,
                 int64_t to) {
    if (chain.name != name) {
        chain.name = name;
        chain.links.clear();
    }
    chain.links.emplace_back(from, to);
    if (chain.links.size() > kChainLinks)
        chain.links.erase(chain.links.begin());
}

// "2026-11-16T10:00:00.000Z" as the panel lists it, in local time.
NSString* VersionDate(const std::string& createdAt) {
    static NSISO8601DateFormatter* reader = [] {
        NSISO8601DateFormatter* made = [NSISO8601DateFormatter new];
        made.formatOptions = NSISO8601DateFormatWithInternetDateTime |
                             NSISO8601DateFormatWithFractionalSeconds;
        return made;
    }();
    static NSDateFormatter* earlier = [] {
        NSDateFormatter* made = [NSDateFormatter new];
        made.dateFormat = @"dd/MM HH:mm";
        return made;
    }();
    static NSDateFormatter* today = [] {
        NSDateFormatter* made = [NSDateFormatter new];
        made.dateFormat = @"HH:mm";
        return made;
    }();
    NSDate* date = [reader dateFromString:Str(createdAt)];
    if (date == nil) return Str(createdAt);
    // Today's versions by their hour alone: the list is narrow.
    return [NSCalendar.currentCalendar isDateInToday:date]
               ? [today stringFromDate:date]
               : [earlier stringFromDate:date];
}

NSString* VersionSource(const std::string& source) {
    if (source == kPanelSource) return @"panneau";
    if (source == "edit_motion_kit" || source == "save_motion_kit")
        return @"agent";
    if (source.rfind("restore v", 0) == 0)
        return [NSString
            stringWithFormat:@"restaurée de %@", Str(source.substr(8))];
    return @"fichier";
}

bool SameShape(const std::vector<Field>& left,
               const std::vector<Field>& right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const Field& a = left[index];
        const Field& b = right[index];
        if (a.path != b.path || a.kind != b.kind || a.label != b.label ||
            a.section != b.section || a.options != b.options)
            return false;
    }
    return true;
}

bool IsFontPath(const std::string& path) {
    const auto endsWith = [&path](const std::string& suffix) {
        return path.size() >= suffix.size() &&
               path.compare(path.size() - suffix.size(), suffix.size(),
                            suffix) == 0;
    };
    return path == "props.police" || endsWith(".police") ||
           endsWith("font_family");
}

}  // namespace

@class CMCharterFieldRow;

@interface CMCharterPanel () <NSWindowDelegate,
                              NSTableViewDataSource,
                              NSTableViewDelegate,
                              CMCharterFrameViewDelegate>
- (void)fieldRow:(CMCharterFieldRow*)row
    committedText:(NSString*)text
          gesture:(const GestureBase&)gesture;
// The charter shown and its version, for a gesture beginning now.
- (GestureBase)cmGestureBase;
@end

// CH6 (review) -- a colour well that says when it is opened: the gesture
// of choosing a colour begins there, not when the colour panel answers.
@interface CMCharterColorWell : NSColorWell
@property(nonatomic, copy) void (^activated)(void);
@end

@implementation CMCharterColorWell
- (void)activate:(BOOL)exclusive {
    if (self.activated) self.activated();
    [super activate:exclusive];
}
@end

// A view laid out by its owner on every resize, rows from the top.
@interface CMCharterLayoutView : NSView
@property(nonatomic, copy) void (^layoutHandler)(void);
@end

@implementation CMCharterLayoutView
- (BOOL)isFlipped {
    return YES;
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    if (self.layoutHandler) self.layoutHandler();
}
@end

// -- One field of the charter: its label, where its value comes from, its
// control, and its error line.

@interface CMCharterFieldRow : NSView <NSTextViewDelegate, NSTextFieldDelegate>
- (instancetype)initWithField:(const Field&)field panel:(CMCharterPanel*)panel;
- (const Field&)field;
- (void)updateWithField:(const Field&)field;
- (void)showError:(NSString*)error;
- (CGFloat)layoutForWidth:(CGFloat)width;
- (BOOL)isEditing;
- (void)setEditable:(BOOL)editable;
- (NSControl*)control;
@end

@implementation CMCharterFieldRow {
    Field _field;
    __weak CMCharterPanel* _panel;
    NSTextField* _label;
    NSTextField* _origin;
    NSTextField* _error;
    NSTextField* _text;
    NSColorWell* _well;
    NSPopUpButton* _popup;
    NSButton* _check;
    NSScrollView* _scroll;
    NSTextView* _textView;
    // The gesture its control began, until its value is committed.
    std::optional<GestureBase> _gesture;
}

- (BOOL)isFlipped {
    return YES;
}

// The first key typed, the colour well opened: what the commit will carry.
- (void)cmBeginGesture {
    if (!_gesture) _gesture = [_panel cmGestureBase];
}

// The gesture to commit with: the one begun, or -- a pop-up, a checkbox,
// whose gesture is the click itself -- now. The next begins afresh.
- (GestureBase)cmTakeGesture {
    const GestureBase taken = _gesture ? *_gesture : [_panel cmGestureBase];
    _gesture.reset();
    return taken;
}

- (void)cmCommit:(NSString*)text {
    const GestureBase gesture = [self cmTakeGesture];
    [_panel fieldRow:self committedText:text gesture:gesture];
}

- (void)controlTextDidBeginEditing:(NSNotification*)notification {
    (void)notification;
    [self cmBeginGesture];
}

- (void)textDidBeginEditing:(NSNotification*)notification {
    (void)notification;
    [self cmBeginGesture];
}

- (instancetype)initWithField:(const Field&)field panel:(CMCharterPanel*)panel {
    if ((self = [super initWithFrame:NSZeroRect])) {
        _field = field;
        _panel = panel;
        _label = [NSTextField labelWithString:Str(field.label)];
        _label.font = CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
        _label.textColor = CMTextPrimaryColor();
        _label.lineBreakMode = NSLineBreakByTruncatingTail;
        _label.toolTip = Str(field.path);
        [self addSubview:_label];
        _origin = [NSTextField labelWithString:@""];
        _origin.font = CMFont(ui::theme::kFontSizeCaption, NSFontWeightRegular);
        _origin.textColor = CMThemeColor(ui::theme::kTextTertiary);
        _origin.lineBreakMode = NSLineBreakByTruncatingTail;
        [self addSubview:_origin];
        _error = [NSTextField wrappingLabelWithString:@""];
        _error.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
        _error.textColor = CMThemeColor(ui::theme::kError);
        _error.hidden = YES;
        [self addSubview:_error];
        [self cmMakeControls];
        [self updateWithField:field];
    }
    return self;
}

- (NSTextField*)cmTextField {
    NSTextField* field = [NSTextField textFieldWithString:@""];
    field.font = CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
    field.controlSize = NSControlSizeSmall;
    field.target = self;
    field.action = @selector(cmTextCommitted:);
    field.cell.sendsActionOnEndEditing = YES;
    field.delegate = self;
    field.accessibilityLabel = Str(_field.label);
    [self addSubview:field];
    return field;
}

- (void)cmMakeControls {
    switch (_field.kind) {
        case FieldKind::Color: {
            CMCharterColorWell* well =
                [[CMCharterColorWell alloc] initWithFrame:NSZeroRect];
            __weak CMCharterFieldRow* weakRow = self;
            well.activated = ^{
              [weakRow cmBeginGesture];
            };
            _well = well;
            // One write when the colour is chosen, not one per step of the
            // colour panel's drag.
            _well.continuous = NO;
            _well.target = self;
            _well.action = @selector(cmWellChanged:);
            _well.accessibilityLabel = Str(_field.label);
            [self addSubview:_well];
            _text = [self cmTextField];
            break;
        }
        case FieldKind::Enum: {
            _popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                                pullsDown:NO];
            _popup.controlSize = NSControlSizeSmall;
            _popup.font =
                CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
            [_popup addItemWithTitle:@"—"];
            _popup.lastItem.representedObject = @"";
            for (const std::string& option : _field.options) {
                [_popup addItemWithTitle:Str(option)];
                _popup.lastItem.representedObject = Str(option);
            }
            _popup.target = self;
            _popup.action = @selector(cmPopupChanged:);
            _popup.accessibilityLabel = Str(_field.label);
            [self addSubview:_popup];
            break;
        }
        case FieldKind::Bool:
            _check = [NSButton checkboxWithTitle:@""
                                          target:self
                                          action:@selector(cmCheckChanged:)];
            _check.controlSize = NSControlSizeSmall;
            _check.accessibilityLabel = Str(_field.label);
            [self addSubview:_check];
            break;
        case FieldKind::LongText:
        case FieldKind::List: {
            _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
            _scroll.hasVerticalScroller = YES;
            _scroll.borderType = NSBezelBorder;
            _textView = [[NSTextView alloc] initWithFrame:NSZeroRect];
            _textView.font =
                CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
            _textView.textColor = CMTextPrimaryColor();
            _textView.backgroundColor = CMSurfaceControlColor();
            _textView.insertionPointColor = CMTextPrimaryColor();
            _textView.richText = NO;
            _textView.allowsUndo = YES;
            _textView.verticallyResizable = YES;
            _textView.horizontallyResizable = NO;
            _textView.autoresizingMask = NSViewWidthSizable;
            _textView.textContainer.widthTracksTextView = YES;
            _textView.automaticQuoteSubstitutionEnabled = NO;
            _textView.automaticDashSubstitutionEnabled = NO;
            _textView.automaticTextReplacementEnabled = NO;
            _textView.delegate = self;
            _textView.accessibilityLabel = Str(_field.label);
            _scroll.documentView = _textView;
            [self addSubview:_scroll];
            break;
        }
        case FieldKind::Text:
            if (IsFontPath(_field.path)) {
                _popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                                    pullsDown:NO];
                _popup.controlSize = NSControlSizeSmall;
                _popup.font =
                    CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
                CMFillCaptionFontPopup(_popup);
                if (_field.path.rfind("captions", 0) != 0) {
                    // "system" is a caption's word for Helvetica Neue; the
                    // motion format names a family, never it.
                    [_popup removeItemAtIndex:0];
                    [_popup removeItemAtIndex:0];
                }
                _popup.target = self;
                _popup.action = @selector(cmFontChanged:);
                _popup.accessibilityLabel = Str(_field.label);
                [self addSubview:_popup];
            } else {
                _text = [self cmTextField];
            }
            break;
        case FieldKind::Number:
        case FieldKind::Integer: {
            _text = [self cmTextField];
            _text.alignment = NSTextAlignmentRight;
            _text.font =
                [NSFont monospacedDigitSystemFontOfSize:ui::theme::kFontSizeBody
                                                 weight:NSFontWeightRegular];
            std::string bounds;
            if (_field.min && _field.max)
                bounds = MotionKitNumber(*_field.min, 0.0).Dump() + " à " +
                         MotionKitNumber(*_field.max, 0.0).Dump();
            _text.placeholderString = Str(bounds);
            _text.toolTip = Str(bounds);
            break;
        }
    }
}

- (const Field&)field {
    return _field;
}

- (NSControl*)control {
    if (_well) return _well;
    if (_popup) return _popup;
    if (_check) return _check;
    return _text;
}

- (BOOL)isEditing {
    if (_text != nil && _text.currentEditor != nil) return YES;
    return _textView != nil && self.window.firstResponder == _textView;
}

- (void)updateWithField:(const Field&)field {
    _field = field;
    _origin.stringValue = Str(field.origin);
    if ([self isEditing]) return;
    NSString* value = Str(field.value);
    if (_text) _text.stringValue = value;
    if (_well) {
        NSColor* color = ColorOf(field.value, field.path);
        _well.color = color ?: CMThemeColor(ui::theme::kSurfaceControl);
    }
    if (_popup && field.kind == FieldKind::Enum) {
        const NSInteger index = [_popup indexOfItemWithRepresentedObject:value];
        [_popup selectItemAtIndex:std::max<NSInteger>(0, index)];
    } else if (_popup) {
        if (field.value.empty())
            [_popup selectItem:nil];
        else
            CMSelectCaptionFont(_popup, field.value);
    }
    if (_check)
        _check.state = field.value == "true" ? NSControlStateValueOn
                                             : NSControlStateValueOff;
    if (_textView) _textView.string = value;
}

- (void)setEditable:(BOOL)editable {
    _text.enabled = editable;
    _well.enabled = editable;
    _popup.enabled = editable;
    _check.enabled = editable;
    _textView.editable = editable;
}

- (void)showError:(NSString*)error {
    _error.stringValue = error ?: @"";
    _error.hidden = error.length == 0;
}

- (CGFloat)layoutForWidth:(CGFloat)width {
    const CGFloat inset = ui::theme::kSpaceM;
    const CGFloat gap = ui::theme::kSpaceS;
    const CGFloat controlX = inset + kFieldLabelWidth + gap;
    const CGFloat controlWidth =
        std::max<CGFloat>(40.0, width - controlX - inset);
    const CGFloat rowHeight = 22.0;
    _label.frame = NSMakeRect(inset, 3.0, kFieldLabelWidth, 16.0);
    _origin.frame = NSMakeRect(inset, 19.0, kFieldLabelWidth, 13.0);
    CGFloat y = 36.0;
    if (_well) {
        _well.frame = NSMakeRect(controlX, 3.0, 40.0, rowHeight);
        _text.frame =
            NSMakeRect(controlX + 46.0, 3.0,
                       std::max<CGFloat>(30.0, controlWidth - 46.0), rowHeight);
    } else if (_scroll) {
        const NSUInteger lines = std::max<NSUInteger>(
            3,
            std::min<NSUInteger>(
                8, [_textView.string componentsSeparatedByString:@"\n"].count +
                       1));
        const CGFloat height = 8.0 + 16.0 * static_cast<CGFloat>(lines);
        _scroll.frame = NSMakeRect(
            inset, 34.0, std::max<CGFloat>(40.0, width - 2 * inset), height);
        y = 34.0 + height + 4.0;
    } else if (_popup) {
        _popup.frame =
            NSMakeRect(controlX - 3.0, 1.0, controlWidth + 6.0, 24.0);
    } else if (_check) {
        _check.frame = NSMakeRect(controlX, 3.0, controlWidth, rowHeight);
    } else if (_text) {
        _text.frame = NSMakeRect(controlX, 3.0, controlWidth, rowHeight);
    }
    if (!_error.hidden) {
        const CGFloat errorWidth = std::max<CGFloat>(40.0, width - 2 * inset);
        _error.preferredMaxLayoutWidth = errorWidth;
        const NSSize needed =
            [_error.cell cellSizeForBounds:NSMakeRect(0, 0, errorWidth, 400)];
        _error.frame = NSMakeRect(inset, y, errorWidth, needed.height);
        y += needed.height + 4.0;
    }
    return y;
}

- (void)cmTextCommitted:(NSTextField*)sender {
    [self cmCommit:sender.stringValue];
}

- (void)cmWellChanged:(NSColorWell*)sender {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    // The well shows the stored colour through NSColor, which rounds: only
    // a colour actually chosen replaces the stored spelling.
    if (!CMChosenCaptionChannels(sender.color, _field.value, red, green, blue))
        return;
    NSString* text =
        [NSString stringWithFormat:@"#%02x%02x%02x", red, green, blue];
    const size_t at = _field.value.find('@');
    if (_field.path.rfind("captions", 0) == 0 && at != std::string::npos)
        text = [text stringByAppendingString:Str(_field.value.substr(at))];
    _text.stringValue = text;
    [self cmCommit:text];
}

- (void)cmPopupChanged:(NSPopUpButton*)sender {
    NSString* value = sender.selectedItem.representedObject;
    [self cmCommit:value ?: @""];
}

- (void)cmFontChanged:(NSPopUpButton*)sender {
    NSString* value = sender.selectedItem.representedObject;
    if (value.length == 0) return;
    [self cmCommit:value];
}

- (void)cmCheckChanged:(NSButton*)sender {
    [self cmCommit:sender.state == NSControlStateValueOn ? @"true" : @"false"];
}

- (void)textDidEndEditing:(NSNotification*)notification {
    (void)notification;
    [self cmCommit:_textView.string];
}

@end

// -- The panel.

typedef NS_ENUM(NSInteger, CMCharterRestore) {
    CMCharterRestorePlain = 0,
    CMCharterRestoreUndo,
    CMCharterRestoreRedo,
};

@implementation CMCharterPanel {
    NSWindow* _editor;
    NSPanel* _panel;
    BOOL _placing;
    BOOL _placed;
    // Opened by the person: the panel reads and watches while it is, even
    // hidden with the application (hidesOnDeactivate).
    BOOL _open;
    CMCharterLayoutView* _content;
    // Left.
    NSTextField* _namesHeader;
    NSScrollView* _namesScroll;
    NSTableView* _namesTable;
    NSButton* _newButton;
    NSTextField* _versionsHeader;
    NSScrollView* _versionsScroll;
    NSTableView* _versionsTable;
    NSButton* _restoreButton;
    // Centre.
    NSSegmentedControl* _frameSelector;
    NSPopUpButton* _backgroundPopup;
    NSButton* _logoToggle;
    NSButton* _nameToggle;
    NSButton* _captionToggle;
    NSButton* _cardToggle;
    NSButton* _perFormatButton;
    NSArray<CMCharterFrameView*>* _frameViews;
    NSArray<NSTextField*>* _frameLabels;
    NSTextField* _notice;
    NSButton* _noticeReveal;
    // Right.
    NSScrollView* _fieldsScroll;
    CMCharterLayoutView* _fieldsBody;
    NSMutableArray<NSView*>* _fieldItems;
    NSTextField* _jsonHeader;
    NSScrollView* _jsonScroll;
    NSTextView* _jsonView;
    NSButton* _revealButton;

    // The library and the charter shown (main thread only).
    MotionLibraryLocation _location;
    std::vector<std::string> _names;
    std::set<std::string> _broken;
    std::set<std::string> _kitFiles;
    // The charter asked for (the list's selection), and the one the state
    // below describes: they differ from a click in the list until its read
    // is shown, and nothing is written meanwhile.
    std::string _selected;
    std::string _shown;
    // The read that last changed the selection, and the last read shown.
    uint64_t _selectionGeneration;
    uint64_t _shownGeneration;
    bool _hasState;
    MotionKitState _state;
    std::vector<MotionKitVersion> _versions;
    mcp_json::Value _kitFile;
    bool _kitReadable;
    bool _writable;
    std::string _readError;
    std::vector<MotionTemplateInfo> _catalog;
    std::vector<Field> _fields;
    std::vector<std::pair<std::string, std::string>> _fieldErrors;
    ui::charter::History _history;
    NSInteger _selectedFrame;
    BOOL _allFrames;
    NSString* _writeNotice;
    // The code of the refusal _writeNotice says, empty for a write done.
    std::string _writeRefusal;
    std::vector<std::string> _renderNotices;
    std::shared_ptr<RenderedFrames> _rendered;

    // The queues, and what is pending on them.
    dispatch_queue_t _ioQueue;
    dispatch_queue_t _renderQueue;
    std::shared_ptr<WriteChain> _chain;
    uint64_t _readGeneration;
    uint64_t _renderGeneration;
    // The last render asked for, read by the render queue to skip a job a
    // newer one replaced (a window being resized asks for many).
    std::shared_ptr<std::atomic<uint64_t>> _latestRender;
    NSInteger _pendingIo;
    NSInteger _pendingRender;
    BOOL _renderScheduled;

    // A gesture, mouse down to mouse up, and what it began on.
    std::optional<Drag> _drag;
    GestureBase _dragGesture;
    DragContext _dragContext;
    double _dragSnap;
    NSInteger _dragFrame;

    // The watch on kits/.
    dispatch_source_t _folderWatch;
    dispatch_source_t _fileWatch;
    uint64_t _watchGeneration;
    id _activation;
}

- (instancetype)initWithEditorWindow:(NSWindow*)editor {
    if ((self = [super init])) {
        _editor = editor;
        _selectionGeneration = 0;
        _shownGeneration = 0;
        _hasState = false;
        _kitReadable = false;
        _writable = false;
        _selectedFrame = 0;
        _allFrames = NO;
        _rememberFrame = YES;
        _chain = std::make_shared<WriteChain>();
        _latestRender = std::make_shared<std::atomic<uint64_t>>(0);
        _ioQueue = dispatch_queue_create("cutmachine.charter.library",
                                         DISPATCH_QUEUE_SERIAL);
        _renderQueue = dispatch_queue_create("cutmachine.charter.render",
                                             DISPATCH_QUEUE_SERIAL);
        std::string error;
        std::vector<MotionTemplateInfo> shipped;
        if (MotionTemplateCatalog(shipped, error)) {
            for (const char* name : {"logo", "nom", "carton"})
                for (const MotionTemplateInfo& piece : shipped)
                    if (piece.name == name) _catalog.push_back(piece);
        }
        [self cmBuildWindow];
        __weak CMCharterPanel* weakSelf = self;
        _activation = [NSNotificationCenter.defaultCenter
            addObserverForName:NSApplicationDidBecomeActiveNotification
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification* note) {
                      (void)note;
                      CMCharterPanel* strongSelf = weakSelf;
                      if (strongSelf.shown) [strongSelf cmReload];
                    }];
    }
    return self;
}

- (void)dealloc {
    if (_activation)
        [NSNotificationCenter.defaultCenter removeObserver:_activation];
    if (_folderWatch) dispatch_source_cancel(_folderWatch);
    if (_fileWatch) dispatch_source_cancel(_fileWatch);
}

- (NSPanel*)window {
    return _panel;
}

// -- The window and its controls.

- (NSTableView*)cmTableWithIdentifier:(NSString*)identifier
                               scroll:(NSScrollView* __strong*)scroll {
    NSTableView* table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    NSTableColumn* column =
        [[NSTableColumn alloc] initWithIdentifier:identifier];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [table addTableColumn:column];
    table.headerView = nil;
    table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    table.rowHeight = 20.0;
    table.backgroundColor = CMThemeColor(ui::theme::kSurfaceSunken);
    table.dataSource = self;
    table.delegate = self;
    table.identifier = identifier;
    *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    (*scroll).hasVerticalScroller = YES;
    (*scroll).autohidesScrollers = YES;
    (*scroll).borderType = NSNoBorder;
    (*scroll).documentView = table;
    [_content addSubview:*scroll];
    return table;
}

- (NSButton*)cmCheckbox:(NSString*)title on:(BOOL)on {
    NSButton* box = [NSButton checkboxWithTitle:title
                                         target:self
                                         action:@selector(cmTogglesChanged:)];
    box.controlSize = NSControlSizeSmall;
    box.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
    box.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    [box sizeToFit];
    [_content addSubview:box];
    return box;
}

- (void)cmBuildWindow {
    using namespace ui::charter;
    _panel = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, kWindowDefaultWidth,
                                       kWindowDefaultHeight)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable |
                            NSWindowStyleMaskUtilityWindow
                    backing:NSBackingStoreBuffered
                      defer:YES];
    _panel.title = @"Charte";
    _panel.floatingPanel = YES;
    _panel.hidesOnDeactivate = YES;
    // It takes the keyboard when clicked: Cmd+Z then undoes its gestures.
    _panel.becomesKeyOnlyIfNeeded = NO;
    _panel.releasedWhenClosed = NO;
    _panel.minSize = NSMakeSize(kWindowMinimumWidth, kWindowMinimumHeight);
    _panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    _panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    _panel.backgroundColor = CMSurfacePanelColor();
    _panel.delegate = self;

    _content = [[CMCharterLayoutView alloc]
        initWithFrame:NSMakeRect(0, 0, kWindowDefaultWidth,
                                 kWindowDefaultHeight)];
    _content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _panel.contentView = _content;

    _namesHeader = CMMakeSectionHeader(@"Chartes");
    [_content addSubview:_namesHeader];
    _namesTable = [self cmTableWithIdentifier:@"charter.names"
                                       scroll:&_namesScroll];
    _newButton =
        CMMakeStyledButton(@"Nouvelle charte…", self, @selector(cmNewCharter:));
    [_content addSubview:_newButton];
    _versionsHeader = CMMakeSectionHeader(@"Versions");
    [_content addSubview:_versionsHeader];
    _versionsTable = [self cmTableWithIdentifier:@"charter.versions"
                                          scroll:&_versionsScroll];
    _restoreButton =
        CMMakeStyledButton(@"Restaurer", self, @selector(cmRestore:));
    _restoreButton.toolTip =
        @"Écrit le contenu de la version choisie comme une nouvelle version";
    [_content addSubview:_restoreButton];

    _frameSelector = [NSSegmentedControl
        segmentedControlWithLabels:@[ @"9:16", @"16:9", @"1:1", @"Les trois" ]
                      trackingMode:NSSegmentSwitchTrackingSelectOne
                            target:self
                            action:@selector(cmFrameChosen:)];
    _frameSelector.controlSize = NSControlSizeSmall;
    _frameSelector.font =
        CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
    _frameSelector.selectedSegment = 0;
    [_frameSelector sizeToFit];
    [_content addSubview:_frameSelector];
    _backgroundPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                                  pullsDown:NO];
    _backgroundPopup.controlSize = NSControlSizeSmall;
    _backgroundPopup.font =
        CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
    [_backgroundPopup addItemsWithTitles:@[
        @"Sur un plan", @"Sur le fond", @"Sur l’accent", @"Sur un damier"
    ]];
    _backgroundPopup.target = self;
    _backgroundPopup.action = @selector(cmBackgroundChosen:);
    _backgroundPopup.toolTip = @"Ce que l’aperçu montre derrière les pièces";
    [_backgroundPopup sizeToFit];
    [_content addSubview:_backgroundPopup];
    _logoToggle = [self cmCheckbox:@"Logo" on:YES];
    _nameToggle = [self cmCheckbox:@"Nom" on:YES];
    _captionToggle = [self cmCheckbox:@"Sous-titre" on:YES];
    _cardToggle = [self cmCheckbox:@"Carton" on:NO];
    _cardToggle.toolTip = @"Le carton seul : il occupe toute l’image";
    _perFormatButton = CMMakeStyledButton(@"Sous-titres par format…", self,
                                          @selector(cmPerFormat:));
    _perFormatButton.toolTip =
        @"Un format de sous-titre pour chaque taille de l’aperçu";
    [_content addSubview:_perFormatButton];

    NSMutableArray<CMCharterFrameView*>* views = [NSMutableArray array];
    NSMutableArray<NSTextField*>* labels = [NSMutableArray array];
    for (size_t index = 0; index < PreviewFrames().size(); ++index) {
        CMCharterFrameView* view =
            [[CMCharterFrameView alloc] initWithFrame:NSZeroRect];
        view.frameIndex = static_cast<NSInteger>(index);
        view.previewFrame = PreviewFrames()[index];
        view.delegate = self;
        [_content addSubview:view];
        [views addObject:view];
        NSTextField* label =
            [NSTextField labelWithString:Str(PreviewFrames()[index].label)];
        label.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightMedium);
        label.textColor = CMTextSecondaryColor();
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        [_content addSubview:label];
        [labels addObject:label];
    }
    _frameViews = views;
    _frameLabels = labels;
    _notice = [NSTextField wrappingLabelWithString:@""];
    _notice.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
    _notice.textColor = CMTextSecondaryColor();
    _notice.selectable = YES;
    [_content addSubview:_notice];
    _noticeReveal = CMMakeStyledButton(@"Révéler dans le Finder", self,
                                       @selector(cmReveal:));
    _noticeReveal.hidden = YES;
    [_content addSubview:_noticeReveal];

    _fieldsScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _fieldsScroll.hasVerticalScroller = YES;
    _fieldsScroll.autohidesScrollers = YES;
    _fieldsScroll.borderType = NSNoBorder;
    _fieldsScroll.drawsBackground = NO;
    _fieldsBody = [[CMCharterLayoutView alloc] initWithFrame:NSZeroRect];
    _fieldsScroll.documentView = _fieldsBody;
    [_content addSubview:_fieldsScroll];
    _fieldItems = [NSMutableArray array];
    _jsonHeader = CMMakeSectionHeader(@"Fichier");
    [_content addSubview:_jsonHeader];
    _jsonScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _jsonScroll.hasVerticalScroller = YES;
    _jsonScroll.hasHorizontalScroller = YES;
    _jsonScroll.borderType = NSNoBorder;
    _jsonView = [[NSTextView alloc] initWithFrame:NSZeroRect];
    _jsonView.editable = NO;
    _jsonView.selectable = YES;
    _jsonView.richText = NO;
    _jsonView.font =
        [NSFont monospacedSystemFontOfSize:ui::theme::kFontSizeCaption
                                    weight:NSFontWeightRegular];
    _jsonView.textColor = CMTextSecondaryColor();
    _jsonView.backgroundColor = CMThemeColor(ui::theme::kSurfaceSunken);
    _jsonView.textContainerInset =
        NSMakeSize(ui::theme::kSpaceXs, ui::theme::kSpaceXs);
    _jsonView.verticallyResizable = YES;
    _jsonView.horizontallyResizable = YES;
    _jsonView.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    _jsonView.textContainer.widthTracksTextView = NO;
    _jsonView.textContainer.containerSize =
        NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    _jsonView.accessibilityLabel = @"Le fichier de la charte, tel qu’écrit";
    _jsonScroll.documentView = _jsonView;
    [_content addSubview:_jsonScroll];
    _revealButton = CMMakeStyledButton(@"Révéler dans le Finder", self,
                                       @selector(cmReveal:));
    [_content addSubview:_revealButton];

    __weak CMCharterPanel* weakSelf = self;
    _content.layoutHandler = ^{
      [weakSelf cmLayout];
    };
    // The keyboard goes to the list of charters, never to a field: Cmd+Z
    // then undoes the last gesture, not the typing of a field.
    _panel.initialFirstResponder = _namesTable;
    [self cmLayout];
    [self cmRefreshAll];
}

// The three columns: the charters and their versions, the preview, the
// fields and the file.
- (void)cmLayout {
    const NSSize size = _content.bounds.size;
    const CGFloat s = ui::theme::kSpaceS;
    const CGFloat m = ui::theme::kSpaceM;
    const CGFloat button = 24.0;
    const CGFloat header = 16.0;

    // Left column.
    const CGFloat leftInner = kLeftWidth - 2 * m;
    CGFloat y = m;
    _namesHeader.frame = NSMakeRect(m, y, leftInner, header);
    y += header + s;
    const CGFloat lists = std::max<CGFloat>(
        80.0, size.height - y - (header + s) - 2 * (button + s) - m - s);
    const CGFloat namesHeight = std::floor(lists * 0.42);
    _namesScroll.frame = NSMakeRect(m, y, leftInner, namesHeight);
    y += namesHeight + s;
    _newButton.frame = NSMakeRect(m, y, leftInner, button);
    y += button + m;
    _versionsHeader.frame = NSMakeRect(m, y, leftInner, header);
    y += header + s;
    const CGFloat versionsHeight =
        std::max<CGFloat>(40.0, size.height - y - button - s - m);
    _versionsScroll.frame = NSMakeRect(m, y, leftInner, versionsHeight);
    y += versionsHeight + s;
    _restoreButton.frame = NSMakeRect(m, y, leftInner, button);

    // Right column.
    const CGFloat rightX = size.width - kRightWidth;
    const CGFloat rightInner = kRightWidth - 2 * m;
    const CGFloat jsonTop =
        size.height - m - button - s - kJsonHeight - s - header;
    _fieldsScroll.frame = NSMakeRect(rightX, 0, kRightWidth,
                                     std::max<CGFloat>(60.0, jsonTop - s));
    _jsonHeader.frame = NSMakeRect(rightX + m, jsonTop, rightInner, header);
    _jsonScroll.frame =
        NSMakeRect(rightX + m, jsonTop + header + s, rightInner, kJsonHeight);
    _revealButton.frame =
        NSMakeRect(rightX + m, size.height - m - button, rightInner, button);
    [self cmLayoutFields];

    // Centre.
    const CGFloat centreX = kLeftWidth + s;
    const CGFloat centreWidth = std::max<CGFloat>(80.0, rightX - s - centreX);
    // The controls flow left to right, a row down when they do not fit.
    CGFloat x = centreX;
    y = m;
    CGFloat rowHeight = 0.0;
    const auto flow = [&](NSView* view, CGFloat width, CGFloat height) {
        if (x > centreX && x + width > centreX + centreWidth) {
            x = centreX;
            y += rowHeight + s;
            rowHeight = 0.0;
        }
        view.frame = NSMakeRect(x, y, width, height);
        x += width + m;
        rowHeight = std::max(rowHeight, height);
    };
    flow(_frameSelector, _frameSelector.frame.size.width, 22.0);
    flow(_backgroundPopup, _backgroundPopup.frame.size.width, 22.0);
    x = centreX;
    y += rowHeight + s;
    rowHeight = 0.0;
    for (NSButton* box in
         @[ _logoToggle, _nameToggle, _captionToggle, _cardToggle ])
        flow(box, box.frame.size.width, 20.0);
    flow(_perFormatButton, 170.0, 22.0);
    const CGFloat framesTop = y + rowHeight + m;
    const CGFloat noticeTop = size.height - m - kNoticeHeight;
    _notice.frame =
        NSMakeRect(centreX, noticeTop, centreWidth - 170.0 - s, kNoticeHeight);
    _notice.preferredMaxLayoutWidth = centreWidth - 170.0 - s;
    _noticeReveal.frame =
        NSMakeRect(centreX + centreWidth - 170.0, noticeTop, 170.0, button);
    const CGFloat label = 16.0;
    const CGFloat areaHeight =
        std::max<CGFloat>(40.0, noticeTop - s - framesTop - label - 4.0);
    const std::vector<PreviewFrame>& frames = PreviewFrames();
    if (_allFrames) {
        double aspects = 0.0;
        for (const PreviewFrame& frame : frames)
            aspects += static_cast<double>(frame.width) / frame.height;
        const CGFloat height = std::min<CGFloat>(
            areaHeight, (centreWidth - 2 * m) / static_cast<CGFloat>(aspects));
        CGFloat total = 2 * m;
        for (const PreviewFrame& frame : frames)
            total += std::floor(height * frame.width / frame.height);
        x = centreX + std::max<CGFloat>(0.0, (centreWidth - total) / 2.0);
        // Side by side they are as wide as the column allows: the room
        // left over goes above and below, not all below.
        const CGFloat top =
            framesTop +
            std::floor(std::max<CGFloat>(0.0, areaHeight - height) / 2.0);
        for (size_t index = 0; index < frames.size(); ++index) {
            const CGFloat width =
                std::floor(height * frames[index].width / frames[index].height);
            _frameViews[index].hidden = NO;
            _frameLabels[index].hidden = NO;
            _frameLabels[index].frame =
                NSMakeRect(x, top, std::max<CGFloat>(width, 60.0), label);
            _frameViews[index].frame =
                NSMakeRect(x, top + label + 4.0, width, std::floor(height));
            x += width + m;
        }
    } else {
        for (size_t index = 0; index < frames.size(); ++index) {
            const bool shown = static_cast<NSInteger>(index) == _selectedFrame;
            _frameViews[index].hidden = !shown;
            _frameLabels[index].hidden = !shown;
            if (!shown) continue;
            const double aspect =
                static_cast<double>(frames[index].width) / frames[index].height;
            CGFloat height = areaHeight;
            CGFloat width = std::floor(height * aspect);
            if (width > centreWidth) {
                width = centreWidth;
                height = std::floor(width / aspect);
            }
            x = centreX + (centreWidth - width) / 2.0;
            _frameLabels[index].frame = NSMakeRect(
                x, framesTop, std::max<CGFloat>(width, 160.0), label);
            _frameViews[index].frame =
                NSMakeRect(x, framesTop + label + 4.0, width, height);
        }
    }
    [self cmScheduleRender];
}

- (void)cmLayoutFields {
    const CGFloat width = _fieldsScroll.contentSize.width;
    const CGFloat m = ui::theme::kSpaceM;
    CGFloat y = ui::theme::kSpaceS;
    for (NSView* item in _fieldItems) {
        if ([item isKindOfClass:CMCharterFieldRow.class]) {
            const CGFloat height =
                [(CMCharterFieldRow*)item layoutForWidth:width];
            item.frame = NSMakeRect(0, y, width, height);
            y += height;
        } else {
            y += ui::theme::kSpaceS;
            item.frame = NSMakeRect(m, y, width - 2 * m, 16.0);
            y += 16.0 + ui::theme::kSpaceXs;
        }
    }
    _fieldsBody.frame =
        NSMakeRect(0, 0, width,
                   std::max<CGFloat>(y + ui::theme::kSpaceM,
                                     _fieldsScroll.contentSize.height));
}

// -- Showing it.

- (BOOL)shown {
    return _open;
}

- (void)toggle {
    if (self.shown)
        [self hidePanel];
    else
        [self showPanel];
}

static ui::charter::Rect CMCharterRect(NSRect rect) {
    return {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height};
}

- (void)showPanel {
    if (!self.shown) {
        std::optional<ui::charter::Rect> remembered;
        if (_placed)
            remembered = CMCharterRect(_panel.frame);
        else if (self.rememberFrame)
            remembered = [CMUiPreferences.sharedPreferences charterWindowFrame];
        std::vector<ui::charter::Rect> screens;
        for (NSScreen* screen in NSScreen.screens)
            screens.push_back(CMCharterRect(screen.visibleFrame));
        const NSRect editor =
            _editor != nil ? _editor.frame : NSScreen.mainScreen.visibleFrame;
        const ui::charter::Rect place = ui::charter::PlaceCharterWindow(
            remembered, CMCharterRect(editor), screens);
        _placing = YES;
        [_panel setFrame:NSMakeRect(place.x, place.y, place.width, place.height)
                 display:NO];
        _placing = NO;
        _placed = YES;
    }
    _open = YES;
    [_panel makeKeyAndOrderFront:nil];
    [self cmReload];
}

- (void)hidePanel {
    const BOOL hadKeyboard = _panel.isKeyWindow;
    _open = NO;
    [_panel orderOut:nil];
    [self cmStopWatching];
    if (hadKeyboard) [_editor makeKeyWindow];
}

- (void)cmRememberFrame {
    if (_placing || !self.shown || !self.rememberFrame) return;
    [CMUiPreferences.sharedPreferences
        setCharterWindowFrame:CMCharterRect(_panel.frame)];
}

- (void)windowDidMove:(NSNotification*)notification {
    (void)notification;
    [self cmRememberFrame];
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    [self cmRememberFrame];
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    _open = NO;
    [self cmStopWatching];
}

// -- Reading the library.

- (void)cmReload {
    const uint64_t generation = ++_readGeneration;
    _location = CurrentMotionLibraryLocation();
    const MotionLibraryLocation location = _location;
    const std::string wanted = _selected;
    ++_pendingIo;
    __weak CMCharterPanel* weakSelf = self;
    dispatch_async(_ioQueue, ^{
      auto result = std::make_shared<ReadResult>();
      MotionKitError error;
      if (location.base.empty()) {
          result->error =
              "Pas de bibliothèque de motion sur ce Mac : aucun dossier "
              "personnel.";
      } else if (!ListMotionKitNames(location, result->names, error)) {
          result->error = "La bibliothèque ne se lit pas : " + error.message;
      }
      for (const std::string& name : result->names) {
          MotionKitState state;
          MotionKitError stateError;
          if (!ReadMotionKitState(location, name, state, stateError) ||
              !state.latest.issue.empty() ||
              state.file == MotionKitFileState::Diverged)
              result->broken.insert(name);
      }
      if (std::find(result->names.begin(), result->names.end(), wanted) !=
          result->names.end())
          result->selected = wanted;
      else if (!result->names.empty())
          result->selected = result->names.front();
      if (!result->selected.empty()) {
          MotionKitError stateError;
          result->has_state = ReadMotionKitState(location, result->selected,
                                                 result->state, stateError);
          if (!result->has_state && result->error.empty())
              result->error = stateError.message;
          MotionKitError versionsError;
          ListMotionKitVersions(location, result->selected, 0, result->versions,
                                versionsError);
      }
      result->kit_files = KitFileNames(location);
      dispatch_async(dispatch_get_main_queue(), ^{
        CMCharterPanel* strongSelf = weakSelf;
        if (strongSelf == nil) return;
        [strongSelf cmFinishRead:result generation:generation];
      });
    });
}

- (void)cmFinishRead:(const std::shared_ptr<ReadResult>&)result
          generation:(uint64_t)generation {
    --_pendingIo;
    if (generation != _readGeneration) return;
    if (result->selected != _shown) {
        // Another charter shown: its own undo, its own field errors.
        _history = {};
        _fieldErrors.clear();
    }
    // The charter asked for is gone, another shown in its place: what was
    // said of it goes too.
    if (result->selected != _selected && !_selected.empty()) _writeNotice = nil;
    _names = result->names;
    _broken = result->broken;
    _selected = result->selected;
    _shown = result->selected;
    _shownGeneration = generation;
    _kitFiles = result->kit_files;
    _readError = result->error;
    _hasState = result->has_state;
    _state = result->has_state ? result->state : MotionKitState{};
    _versions = result->versions;
    _kitFile = mcp_json::Value();
    std::string parseError;
    const bool parsed =
        _hasState && _state.latest.version > 0 &&
        mcp_json::Value::Parse(_state.latest.content, _kitFile, parseError) &&
        _kitFile.IsObject();
    _kitReadable = parsed && _state.latest.issue.empty();
    _writable = _kitReadable && _state.file != MotionKitFileState::Diverged;
    // A write refused on a diverged file: once the file reads again, the
    // refusal is history.
    if (_writeRefusal == kMotionKitFileDiverged &&
        _state.file != MotionKitFileState::Diverged) {
        _writeNotice = nil;
        _writeRefusal.clear();
    }
    [self cmRefreshAll];
    [self cmWatch];
}

// -- What the panel shows of the charter read.

- (void)cmRefreshAll {
    [self cmRefreshTables];
    [self cmRefreshFields];
    [self cmRefreshJson];
    [self cmRefreshFrameChrome];
    [self cmRefreshNotice];
    [self cmScheduleRender];
}

- (void)cmRefreshTables {
    [_namesTable reloadData];
    const auto at = std::find(_names.begin(), _names.end(), _selected);
    if (at != _names.end())
        [_namesTable
                selectRowIndexes:[NSIndexSet
                                     indexSetWithIndex:static_cast<NSUInteger>(
                                                           at - _names.begin())]
            byExtendingSelection:NO];
    else
        [_namesTable deselectAll:nil];
    [_versionsTable reloadData];
    [_versionsTable deselectAll:nil];
    _newButton.enabled = !_location.base.empty();
    [self cmRefreshRestore];
}

- (void)cmRefreshRestore {
    const NSInteger row = _versionsTable.selectedRow;
    _restoreButton.enabled =
        row >= 0 && static_cast<size_t>(row) < _versions.size() &&
        _versions[static_cast<size_t>(row)].version != _state.latest.version &&
        _versions[static_cast<size_t>(row)].issue.empty() &&
        _state.file != MotionKitFileState::Diverged;
}

- (std::vector<MotionTemplateInfo>)cmShownPieces {
    std::vector<MotionTemplateInfo> shown;
    const BOOL card = _cardToggle.state == NSControlStateValueOn;
    for (const MotionTemplateInfo& piece : _catalog) {
        const bool wanted =
            card ? piece.name == "carton"
                 : (piece.name == "logo" &&
                    _logoToggle.state == NSControlStateValueOn) ||
                       (piece.name == "nom" &&
                        _nameToggle.state == NSControlStateValueOn);
        if (wanted) shown.push_back(piece);
    }
    return shown;
}

- (void)cmRefreshFields {
    std::vector<Field> fields;
    if (_kitReadable)
        fields = ui::charter::KitFields(
            _kitFile, [self cmShownPieces],
            PreviewFrames()[static_cast<size_t>(_selectedFrame)]);
    const bool same = SameShape(fields, _fields) &&
                      (_fieldItems.count > 0) == !fields.empty();
    if (same) {
        size_t index = 0;
        for (NSView* item in _fieldItems)
            if ([item isKindOfClass:CMCharterFieldRow.class])
                [(CMCharterFieldRow*)item updateWithField:fields[index++]];
    } else {
        for (NSView* item in _fieldItems) [item removeFromSuperview];
        [_fieldItems removeAllObjects];
        std::string section;
        for (const Field& field : fields) {
            if (field.section != section) {
                section = field.section;
                NSTextField* header = CMMakeSectionHeader(Str(section));
                [_fieldsBody addSubview:header];
                [_fieldItems addObject:header];
            }
            CMCharterFieldRow* row =
                [[CMCharterFieldRow alloc] initWithField:field panel:self];
            [_fieldsBody addSubview:row];
            [_fieldItems addObject:row];
        }
    }
    _fields = std::move(fields);
    for (NSView* item in _fieldItems) {
        if (![item isKindOfClass:CMCharterFieldRow.class]) continue;
        CMCharterFieldRow* row = (CMCharterFieldRow*)item;
        NSString* error = nil;
        for (const auto& [path, message] : _fieldErrors)
            if (path == row.field.path) error = Str(message);
        [row showError:error];
        [row setEditable:_writable];
    }
    [self cmLayoutFields];
}

- (void)cmRefreshJson {
    std::string text;
    if (_kitReadable)
        text = DumpMotionKit(_kitFile);
    else if (_hasState)
        text = _state.latest.content;
    _jsonView.string = Str(text);
    _revealButton.enabled = !_location.inbox.empty();
}

- (void)cmRefreshFrameChrome {
    NSColor* fond = nil;
    NSColor* accent = nil;
    if (_kitReadable && _state.kit.props.IsObject()) {
        if (const mcp_json::Value* value = _state.kit.props.Find("fond");
            value != nullptr && value->IsString())
            fond = ColorOf(value->AsString(), "props.fond");
        if (const mcp_json::Value* value = _state.kit.props.Find("accent");
            value != nullptr && value->IsString())
            accent = ColorOf(value->AsString(), "props.accent");
    }
    for (CMCharterFrameView* view in _frameViews) {
        view.fondColor = fond;
        view.accentColor = accent;
        view.background = static_cast<CMCharterBackground>(
            _backgroundPopup.indexOfSelectedItem);
        view.selected = _allFrames && view.frameIndex == _selectedFrame;
    }
    const BOOL card = _cardToggle.state == NSControlStateValueOn;
    _logoToggle.enabled = !card;
    _nameToggle.enabled = !card;
    _captionToggle.enabled = !card;
    _perFormatButton.enabled =
        _writable &&
        !ui::charter::CaptionFormatEdits(_kitFile, PreviewFrames()).empty();
}

- (void)cmRefreshNotice {
    NSString* text = @"";
    BOOL alarm = NO;
    _noticeReveal.hidden = YES;
    if (!_readError.empty()) {
        text = Str(_readError);
        alarm = YES;
    } else if (_names.empty() && !_location.base.empty()) {
        text = @"Aucune charte dans la bibliothèque. « Nouvelle charte… » en "
               @"crée une.";
    } else if (_hasState && _state.file == MotionKitFileState::Diverged) {
        text = Str(
            ui::charter::KitFileDivergedMessage(_selected, _state.file_issue));
        alarm = YES;
        _noticeReveal.hidden = NO;
    } else if (_hasState && !_state.latest.issue.empty()) {
        text = [NSString
            stringWithFormat:
                @"La version %lld de « %@ » ne se lit plus : %@. Son JSON est "
                @"à droite ; « Restaurer » écrit une version plus ancienne "
                @"comme la nouvelle.",
                static_cast<long long>(_state.latest.version), Str(_selected),
                Str(_state.latest.issue)];
        alarm = YES;
    } else if (_writeNotice.length > 0) {
        text = _writeNotice;
    }
    std::vector<std::string> extra;
    if (!alarm)
        for (const std::string& notice : _renderNotices)
            if (std::find(extra.begin(), extra.end(), notice) == extra.end())
                extra.push_back(notice);
    for (const std::string& notice : extra)
        text = text.length > 0
                   ? [NSString stringWithFormat:@"%@\n%@", text, Str(notice)]
                   : Str(notice);
    _notice.stringValue = text;
    _notice.textColor =
        alarm ? CMThemeColor(ui::theme::kError) : CMTextSecondaryColor();
    for (NSUInteger index = 0; index < _frameLabels.count; ++index) {
        NSString* label = Str(PreviewFrames()[index].label);
        if (_rendered)
            for (const RenderedFrame& frame : *_rendered)
                if (frame.index == index && !frame.caption_format.empty())
                    label = [NSString
                        stringWithFormat:@"%@ · sous-titres : format « %@ »",
                                         label, Str(frame.caption_format)];
        _frameLabels[index].stringValue = label;
        _frameLabels[index].textColor =
            _allFrames && static_cast<NSInteger>(index) == _selectedFrame
                ? CMAccentColor()
                : CMTextSecondaryColor();
    }
}

- (NSString*)noticeText {
    return _notice.stringValue;
}

- (NSString*)selectedName {
    return Str(_selected);
}

- (int64_t)shownVersion {
    return _hasState ? _state.latest.version : 0;
}

- (BOOL)writable {
    return _writable;
}

// -- What a gesture begins on, and when a write is refused for it.

// The list asks for another charter: it is read, and nothing is written
// until it is shown -- the state, the fields and the file still describe
// the one before.
- (void)cmSelectCharter:(const std::string&)name {
    _selected = name;
    _history = {};
    _fieldErrors.clear();
    _writeNotice = nil;
    _writeRefusal.clear();
    [self cmReload];
    _selectionGeneration = _readGeneration;
}

- (BOOL)cmSelectionPending {
    return _selected != _shown || _shownGeneration < _selectionGeneration;
}

- (GestureBase)cmGestureBase {
    return {_shown, _hasState ? _state.latest.version : 0};
}

// CH6 (review) -- refused, with a beep and a word on the notice line: a
// gesture while the list's choice is not yet the charter shown, and one
// that began on another charter than the one shown now. Its edits were
// made on the charter it began on, which is not the one a write would
// reach.
- (BOOL)cmRefusesGesture:(const GestureBase&)gesture {
    NSString* why = nil;
    if ([self cmSelectionPending])
        why = [NSString
            stringWithFormat:@"« %@ » se lit encore : rien n’est écrit. Refais "
                             @"le geste une fois la charte affichée.",
                             Str(_selected)];
    else if (gesture.name != _shown)
        why = [NSString
            stringWithFormat:@"Le geste a commencé sur « %@ », qui n’est plus "
                             @"la charte affichée : rien n’est écrit.",
                             Str(gesture.name)];
    if (why == nil) return NO;
    NSBeep();
    _writeNotice = why;
    _writeRefusal.clear();
    [self cmRefreshNotice];
    return YES;
}

// -- The previews.

- (void)cmScheduleRender {
    if (_renderScheduled) return;
    _renderScheduled = YES;
    __weak CMCharterPanel* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      CMCharterPanel* strongSelf = weakSelf;
      if (strongSelf == nil) return;
      strongSelf->_renderScheduled = NO;
      [strongSelf cmRender];
    });
}

- (void)cmRender {
    const uint64_t generation = ++_renderGeneration;
    _latestRender->store(generation);
    if (!_kitReadable) {
        _rendered.reset();
        _renderNotices.clear();
        NSString* message = _names.empty() ? @""
                            : _hasState
                                ? @"La charte ne se lit pas : rien à montrer."
                                : @"";
        for (CMCharterFrameView* view in _frameViews) {
            [view setPictures:@[]];
            view.message = message;
        }
        [self cmRefreshNotice];
        return;
    }
    RenderJob job;
    job.kit = _state.kit;
    job.pieces = [self cmShownPieces];
    job.captions = _captionToggle.state == NSControlStateValueOn &&
                   _cardToggle.state != NSControlStateValueOn;
    job.placeholder = PlaceholderLogo();
    for (CMCharterFrameView* view in _frameViews) {
        if (view.hidden || view.bounds.size.width < 8.0) continue;
        const double backing = std::max<CGFloat>(
            1.0, [view convertSizeToBacking:NSMakeSize(1.0, 1.0)].width);
        const double scale = std::min(
            1.0, view.bounds.size.width * backing / view.previewFrame.width);
        job.frames.emplace_back(static_cast<size_t>(view.frameIndex), scale);
    }
    if (job.frames.empty()) return;
    ++_pendingRender;
    const std::shared_ptr<std::atomic<uint64_t>> latest = _latestRender;
    __weak CMCharterPanel* weakSelf = self;
    dispatch_async(_renderQueue, ^{
      std::shared_ptr<RenderedFrames> frames = Render(
          job, [&latest, generation] { return latest->load() != generation; });
      dispatch_async(dispatch_get_main_queue(), ^{
        CMCharterPanel* strongSelf = weakSelf;
        if (strongSelf == nil) return;
        --strongSelf->_pendingRender;
        if (generation != strongSelf->_renderGeneration) return;
        [strongSelf cmFinishRender:frames];
      });
    });
}

- (void)cmFinishRender:(const std::shared_ptr<RenderedFrames>&)frames {
    _rendered = frames;
    _renderNotices.clear();
    for (CMCharterFrameView* view in _frameViews) {
        NSMutableArray* pictures = [NSMutableArray array];
        std::vector<std::string> failures;
        bool anything = false;
        for (const RenderedFrame& frame : *frames) {
            if (static_cast<NSInteger>(frame.index) != view.frameIndex)
                continue;
            for (const RenderedPiece& piece : frame.pieces) {
                if (piece.image != nil) {
                    [pictures addObject:piece.image];
                    anything = true;
                }
                if (!piece.error.empty()) failures.push_back(piece.error);
            }
            if (frame.caption != nil) {
                [pictures addObject:frame.caption];
                anything = true;
            }
            for (const std::string& notice : frame.notices)
                _renderNotices.push_back(notice);
        }
        [view setPictures:pictures];
        std::string message;
        if (!anything)
            for (const std::string& failure : failures)
                message += (message.empty() ? "" : "\n") + failure;
        view.message = Str(message);
    }
    [self cmRefreshNotice];
}

- (const RenderedFrame*)cmRenderedFrame:(NSInteger)index {
    if (!_rendered) return nullptr;
    for (const RenderedFrame& frame : *_rendered)
        if (static_cast<NSInteger>(frame.index) == index) return &frame;
    return nullptr;
}

// What can be grabbed in a frame: the logo and the name by their anchors,
// the subtitle last (HitTest takes the last match).
- (std::vector<HitArea>)cmAreasOf:(const RenderedFrame&)frame {
    std::vector<HitArea> areas;
    for (const RenderedPiece& piece : frame.pieces) {
        if (!piece.anchored || piece.preview == nullptr ||
            ui::charter::AnchorId(piece.piece).empty())
            continue;
        HitArea area;
        area.kind = AreaKind::Piece;
        area.piece = piece.piece;
        area.format = piece.preview->format;
        area.quad = piece.quad;
        areas.push_back(area);
    }
    if (frame.caption_shown) {
        HitArea area;
        area.kind = AreaKind::Captions;
        area.format = frame.caption_format;
        area.quad = frame.caption_quad;
        areas.push_back(area);
    }
    return areas;
}

- (const RenderedPiece*)cmPiece:(const std::string&)name
                             of:(const RenderedFrame&)frame {
    for (const RenderedPiece& piece : frame.pieces)
        if (piece.piece == name) return &piece;
    return nullptr;
}

// The logo's handle under `point`, -1 for none (only the logo resizes).
- (int)cmHandleAt:(ui::charter::Point)point
             area:(const HitArea&)area
             view:(CMCharterFrameView*)view {
    if (area.kind != AreaKind::Piece || area.piece != "logo") return -1;
    const double scale = [view pointsPerPixel];
    const std::vector<std::pair<double, double>> handles =
        MotionSelectionHandles(area.quad.data());
    for (size_t index = 0; index < handles.size(); ++index) {
        const double dx = (handles[index].first - point.first) * scale;
        const double dy = (handles[index].second - point.second) * scale;
        if (std::hypot(dx, dy) <= kHandleGrab) return static_cast<int>(index);
    }
    return -1;
}

- (std::vector<ui::charter::Point>)cmSafeZoneOf:(const DragContext&)context {
    const MotionSafeZone& zone = context.safe_zone;
    const double w = context.frame_width;
    const double h = context.frame_height;
    const auto part = [](const MotionSafeMargin& margin, double whole) {
        return margin.den > 0 ? whole * static_cast<double>(margin.num) /
                                    static_cast<double>(margin.den)
                              : 0.0;
    };
    const double left = part(zone.left, w);
    const double right = w - part(zone.right, w);
    const double top = part(zone.top, h);
    const double bottom = h - part(zone.bottom, h);
    return {{left, top}, {right, top}, {right, bottom}, {left, bottom}};
}

- (void)charterFrame:(CMCharterFrameView*)view
         mouseDownAt:(ui::charter::Point)point
           modifiers:(NSEventModifierFlags)modifiers {
    if (_allFrames && view.frameIndex != _selectedFrame) {
        _selectedFrame = view.frameIndex;
        [self cmRefreshFields];
        [self cmRefreshFrameChrome];
        [self cmRefreshNotice];
    }
    _drag.reset();
    const RenderedFrame* frame = [self cmRenderedFrame:view.frameIndex];
    if (frame == nullptr) return;
    const std::vector<HitArea> areas = [self cmAreasOf:*frame];
    Drag drag;
    const HitArea* grabbed = nullptr;
    for (const HitArea& area : areas) {
        const int handle = [self cmHandleAt:point area:area view:view];
        if (handle >= 0) {
            grabbed = &area;
            drag.kind = DragKind::Resize;
            drag.handle = handle;
        }
    }
    if (grabbed == nullptr) {
        grabbed = ui::charter::HitTest(areas, point.first, point.second);
        drag.kind = DragKind::Move;
    }
    if (grabbed == nullptr) return;
    if (!_writable) {
        NSBeep();
        return;
    }
    const GestureBase gesture = [self cmGestureBase];
    if ([self cmRefusesGesture:gesture]) return;
    drag.area = *grabbed;
    drag.start = point;
    drag.current = point;
    drag.fine = (modifiers & NSEventModifierFlagOption) != 0;
    if (grabbed->kind == AreaKind::Piece) {
        const RenderedPiece* piece = [self cmPiece:grabbed->piece of:*frame];
        if (piece == nullptr || piece->preview == nullptr) return;
        _dragContext = ui::charter::PieceDragContext(*piece->preview, _kitFile,
                                                     [view pointsPerPixel]);
    } else {
        _dragContext = DragContext{};
        _dragContext.frame_width = frame->frame.width;
        _dragContext.frame_height = frame->frame.height;
        _dragContext.safe_zone =
            TimelineMotionSafeZone(frame->frame.width, frame->frame.height);
        _dragContext.kit = _kitFile;
        if (!ui::charter::CaptionDragContextFor(_state.kit, frame->frame,
                                                _dragContext.captions))
            return;
    }
    _dragSnap = _dragContext.snap_distance;
    _drag = drag;
    _dragGesture = gesture;
    _dragFrame = view.frameIndex;
    std::vector<ui::charter::Point> outline(drag.area.quad.begin(),
                                            drag.area.quad.end());
    [view setHover:{} handles:{}];
    [view setDragOutline:outline safeZone:[self cmSafeZoneOf:_dragContext]];
}

// Cmd held: no snapping -- at 8 points it dominates the fine moves of a
// small preview.
- (void)cmUpdateDrag:(ui::charter::Point)point
           modifiers:(NSEventModifierFlags)modifiers {
    _drag->current = point;
    _drag->fine = (modifiers & NSEventModifierFlagOption) != 0;
    _dragContext.snap_distance =
        (modifiers & NSEventModifierFlagCommand) != 0 ? 0.0 : _dragSnap;
}

- (void)charterFrame:(CMCharterFrameView*)view
      mouseDraggedTo:(ui::charter::Point)point
           modifiers:(NSEventModifierFlags)modifiers {
    if (!_drag || view.frameIndex != _dragFrame) return;
    [self cmUpdateDrag:point modifiers:modifiers];
    const Quad outline = ui::charter::DragOutline(*_drag, _dragContext);
    [view setDragOutline:std::vector<ui::charter::Point>(outline.begin(),
                                                         outline.end())
                safeZone:[self cmSafeZoneOf:_dragContext]];
}

- (void)charterFrame:(CMCharterFrameView*)view
           mouseUpAt:(ui::charter::Point)point
           modifiers:(NSEventModifierFlags)modifiers {
    if (!_drag || view.frameIndex != _dragFrame) return;
    [self cmUpdateDrag:point modifiers:modifiers];
    const std::vector<MotionKitEdit> edits =
        ui::charter::DragEdits(*_drag, _dragContext);
    const HitArea area = _drag->area;
    _drag.reset();
    [view setDragOutline:{} safeZone:{}];
    if (edits.empty()) return;  // no change, no version
    NSString* what =
        area.kind == AreaKind::Captions
            ? @"sous-titre déplacé"
            : [NSString
                  stringWithFormat:@"« %@ » déplacé en %@", Str(area.piece),
                                   Str(PreviewFrames()[static_cast<size_t>(
                                                           view.frameIndex)]
                                           .label)];
    [self cmWrite:edits label:what path:std::string() gesture:_dragGesture];
}

- (void)charterFrame:(CMCharterFrameView*)view
        mouseMovedTo:(ui::charter::Point)point {
    if (_drag) return;
    const RenderedFrame* frame = [self cmRenderedFrame:view.frameIndex];
    if (frame == nullptr || !_writable) {
        [view setHover:{} handles:{}];
        return;
    }
    const std::vector<HitArea> areas = [self cmAreasOf:*frame];
    const HitArea* over =
        ui::charter::HitTest(areas, point.first, point.second);
    for (const HitArea& area : areas)
        if ([self cmHandleAt:point area:area view:view] >= 0) over = &area;
    if (over == nullptr) {
        [view setHover:{} handles:{}];
        return;
    }
    std::vector<ui::charter::Point> handles;
    if (over->kind == AreaKind::Piece && over->piece == "logo")
        handles = MotionSelectionHandles(over->quad.data());
    [view setHover:std::vector<ui::charter::Point>(over->quad.begin(),
                                                   over->quad.end())
           handles:handles];
}

- (void)charterFrameMouseExited:(CMCharterFrameView*)view {
    if (!_drag) [view setHover:{} handles:{}];
}

// -- Writing.

- (void)cmWrite:(const std::vector<MotionKitEdit>&)edits
          label:(NSString*)label
           path:(const std::string&)path
        gesture:(const GestureBase&)gesture {
    if (_selected.empty() || !_hasState) {
        NSBeep();
        return;
    }
    if ([self cmRefusesGesture:gesture]) return;
    // The charter and the version the gesture began on: moved since by
    // anyone but the panel itself, the engine answers StaleKit.
    const std::string name = gesture.name;
    const int64_t expected = gesture.version;
    const MotionLibraryLocation location = _location;
    const std::shared_ptr<WriteChain> chain = _chain;
    const std::vector<MotionKitEdit> batch = edits;
    // By value: a block holds a C++ reference as a reference, and `path` is
    // the caller's, gone by the time the write comes back.
    const std::string fieldPath = path;
    ++_pendingIo;
    __weak CMCharterPanel* weakSelf = self;
    dispatch_async(_ioQueue, ^{
      const int64_t used = ExpectedFor(*chain, name, expected);
      MotionKitSaved saved;
      MotionKitError error;
      const bool ok = EditMotionKitVersion(location, name, batch, used,
                                           kPanelSource, saved, error);
      if (ok && saved.created) RecordWrite(*chain, name, used, saved.version);
      dispatch_async(dispatch_get_main_queue(), ^{
        CMCharterPanel* strongSelf = weakSelf;
        if (strongSelf == nil) return;
        [strongSelf cmFinishWrite:ok
                            saved:saved
                            error:error
                             used:used
                             name:name
                            label:label
                             path:fieldPath
                          restore:CMCharterRestorePlain
                        restoring:0];
      });
    });
}

- (void)cmRestore:(int64_t)version kind:(CMCharterRestore)kind {
    if (_selected.empty() || !_hasState) {
        NSBeep();
        return;
    }
    // A restore is a gesture of one moment: the button pressed, the key.
    const GestureBase gesture = [self cmGestureBase];
    if ([self cmRefusesGesture:gesture]) return;
    const std::string name = gesture.name;
    const int64_t expected = gesture.version;
    const MotionLibraryLocation location = _location;
    const std::shared_ptr<WriteChain> chain = _chain;
    ++_pendingIo;
    __weak CMCharterPanel* weakSelf = self;
    dispatch_async(_ioQueue, ^{
      const int64_t used = ExpectedFor(*chain, name, expected);
      MotionKitSaved saved;
      MotionKitError error;
      const bool ok =
          RestoreMotionKitVersion(location, name, version, used, saved, error);
      if (ok && saved.created) RecordWrite(*chain, name, used, saved.version);
      dispatch_async(dispatch_get_main_queue(), ^{
        CMCharterPanel* strongSelf = weakSelf;
        if (strongSelf == nil) return;
        [strongSelf cmFinishWrite:ok
                            saved:saved
                            error:error
                             used:used
                             name:name
                            label:nil
                             path:std::string()
                          restore:kind
                        restoring:version];
      });
    });
}

- (void)cmFinishWrite:(bool)ok
                saved:(const MotionKitSaved&)saved
                error:(const MotionKitError&)error
                 used:(int64_t)used
                 name:(const std::string&)name
                label:(NSString*)label
                 path:(const std::string&)path
              restore:(CMCharterRestore)kind
            restoring:(int64_t)restoring {
    --_pendingIo;
    // The history is the charter shown's, and only while it is the one
    // asked for.
    const bool current = name == _shown && _selected == _shown;
    if (!path.empty()) [self cmClearFieldError:path];
    _writeRefusal = ok ? std::string() : error.code;
    if (ok) {
        if (current) {
            if (kind == CMCharterRestoreUndo)
                ui::charter::RecordUndo(_history, saved.version);
            else if (kind == CMCharterRestoreRedo)
                ui::charter::RecordRedo(_history, saved.version);
            else if (saved.created)
                ui::charter::RecordSave(_history, used, saved.version);
        }
        NSString* what =
            kind == CMCharterRestoreUndo
                ? [NSString
                      stringWithFormat:@"annulation (contenu de la v%lld)",
                                       static_cast<long long>(restoring)]
            : kind == CMCharterRestoreRedo
                ? [NSString
                      stringWithFormat:@"rétablissement (contenu de la v%lld)",
                                       static_cast<long long>(restoring)]
            : kind == CMCharterRestorePlain && restoring > 0
                ? [NSString stringWithFormat:@"v%lld restaurée",
                                             static_cast<long long>(restoring)]
                : label;
        _writeNotice =
            saved.created
                ? [NSString
                      stringWithFormat:@"Version %lld écrite : %@.",
                                       static_cast<long long>(saved.version),
                                       what ?: @""]
                : @"Rien n’a changé : aucune version écrite.";
    } else {
        _writeNotice = Str(ui::charter::KitWriteRefusalMessage(name, error));
        if (!path.empty() && error.code != kMotionKitStaleKit &&
            error.code != kMotionKitFileDiverged)
            _fieldErrors.emplace_back(path, Utf8(_writeNotice));
        NSBeep();
    }
    [self cmReload];
}

- (void)cmClearFieldError:(const std::string&)path {
    _fieldErrors.erase(
        std::remove_if(
            _fieldErrors.begin(), _fieldErrors.end(),
            [&path](const std::pair<std::string, std::string>& entry) {
                return entry.first == path;
            }),
        _fieldErrors.end());
}

- (void)fieldRow:(CMCharterFieldRow*)row
    committedText:(NSString*)text
          gesture:(const GestureBase&)gesture {
    // A row the fields were rebuilt without (its editing ended as it left)
    // has nothing left to write.
    if (![_fieldItems containsObject:row]) return;
    const Field field = row.field;
    const std::string typed = Utf8(text);
    if (typed == field.value) {
        [row showError:nil];
        return;
    }
    MotionKitEdit edit;
    std::string message;
    if (!ui::charter::FieldEdit(field, typed, edit, message)) {
        [self cmClearFieldError:field.path];
        _fieldErrors.emplace_back(field.path, message);
        [row showError:Str(message)];
        [self cmLayoutFields];
        return;
    }
    [row showError:nil];
    [self cmWrite:ui::charter::WithBrandFormat(_kitFile, {edit})
            label:Str(field.label)
             path:field.path
          gesture:gesture];
}

// -- Undo, as versions (DEC61).

- (NSTextView*)typingTextView {
    // A sheet of the panel (Nouvelle charte) types in its own fields.
    NSWindow* keyed =
        NSApp.keyWindow.sheetParent == _panel ? NSApp.keyWindow : _panel;
    NSResponder* responder = keyed.firstResponder;
    if (![responder isKindOfClass:NSTextView.class]) return nil;
    NSTextView* text = (NSTextView*)responder;
    // The file's JSON reads, it is not typed in; nor is a field the panel
    // turned off while it was being edited.
    if (!text.editable) return nil;
    if (text.fieldEditor && [text.delegate isKindOfClass:NSTextField.class]) {
        NSTextField* field = (NSTextField*)text.delegate;
        if (!field.enabled || !field.editable) return nil;
    }
    return text;
}

- (NSTextView*)cmTyping {
    return [self typingTextView];
}

- (BOOL)holdsKeyboard {
    return _panel.isKeyWindow || NSApp.keyWindow.sheetParent == _panel;
}

- (BOOL)ownsUndo {
    return [self holdsKeyboard];
}

- (BOOL)canUndo {
    if (NSTextView* typing = [self cmTyping]) return typing.undoManager.canUndo;
    return _panel.attachedSheet == nil && !_history.undo.empty();
}

- (BOOL)canRedo {
    if (NSTextView* typing = [self cmTyping]) return typing.undoManager.canRedo;
    return _panel.attachedSheet == nil && !_history.redo.empty();
}

- (void)undo {
    if (NSTextView* typing = [self cmTyping]) {
        [typing.undoManager undo];
        return;
    }
    if (_pendingIo > 0 || _panel.attachedSheet != nil) {
        NSBeep();
        return;
    }
    const std::optional<int64_t> target =
        ui::charter::UndoTarget(_history, _state.latest.version);
    if (!target) {
        NSBeep();
        if (!_history.undo.empty()) {
            _writeNotice = @"La charte a changé ailleurs depuis ton dernier "
                           @"geste : relue, rien n’est annulé.";
            [self cmReload];
        }
        return;
    }
    [self cmRestore:*target kind:CMCharterRestoreUndo];
}

- (void)redo {
    if (NSTextView* typing = [self cmTyping]) {
        [typing.undoManager redo];
        return;
    }
    if (_pendingIo > 0 || _panel.attachedSheet != nil) {
        NSBeep();
        return;
    }
    const std::optional<int64_t> target =
        ui::charter::RedoTarget(_history, _state.latest.version);
    if (!target) {
        NSBeep();
        if (!_history.redo.empty()) {
            _writeNotice = @"La charte a changé ailleurs : relue, rien n’est "
                           @"rétabli.";
            [self cmReload];
        }
        return;
    }
    [self cmRestore:*target kind:CMCharterRestoreRedo];
}

// -- The controls.

- (void)cmFrameChosen:(NSSegmentedControl*)sender {
    const NSInteger segment = sender.selectedSegment;
    _allFrames = segment == 3;
    if (!_allFrames) _selectedFrame = std::clamp<NSInteger>(segment, 0, 2);
    [self cmLayout];
    [self cmRefreshFields];
    [self cmRefreshFrameChrome];
    [self cmRefreshNotice];
}

- (void)selectFrameIndex:(NSInteger)index {
    _frameSelector.selectedSegment = std::clamp<NSInteger>(index, 0, 3);
    [self cmFrameChosen:_frameSelector];
}

- (void)cmBackgroundChosen:(id)sender {
    (void)sender;
    [self cmRefreshFrameChrome];
}

- (void)cmTogglesChanged:(id)sender {
    (void)sender;
    [self cmRefreshFields];
    [self cmRefreshFrameChrome];
    [self cmScheduleRender];
}

- (void)cmRestore:(id)sender {
    (void)sender;
    const NSInteger row = _versionsTable.selectedRow;
    if (row < 0 || static_cast<size_t>(row) >= _versions.size()) return;
    [self cmRestore:_versions[static_cast<size_t>(row)].version
               kind:CMCharterRestorePlain];
}

- (void)cmReveal:(id)sender {
    (void)sender;
    std::error_code error;
    std::string target = _state.path;
    if (target.empty() || !fs::exists(target, error)) {
        target = _location.inbox.empty() ? std::string()
                                         : MotionKitDirectory(_location.inbox);
        if (!target.empty() && !fs::exists(target, error))
            target = _location.inbox;
    }
    if (target.empty()) {
        NSBeep();
        return;
    }
    [NSWorkspace.sharedWorkspace
        activateFileViewerSelectingURLs:@[ [NSURL
                                            fileURLWithPath:Str(target)] ]];
}

- (void)cmPerFormat:(id)sender {
    (void)sender;
    const std::vector<MotionKitEdit> edits =
        ui::charter::CaptionFormatEdits(_kitFile, PreviewFrames());
    if (edits.empty() || !_writable) {
        NSBeep();
        return;
    }
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Régler les sous-titres par format ?";
    alert.informativeText =
        @"La charte reçoit un format de sous-titre pour chacune des trois "
        @"tailles de l’aperçu (9:16, 16:9, 1:1), qui reprend d’abord ses "
        @"réglages de base. Une fois qu’une charte a des formats, une taille "
        @"qui n’a pas le sien n’a plus de style de sous-titre du tout : un "
        @"film en 4:5 n’en recevrait aucun tant que la charte ne lui en "
        @"donne pas.";
    [alert addButtonWithTitle:@"Régler par format"];
    [alert addButtonWithTitle:@"Annuler"];
    // The gesture began with the button: its edits and its version are
    // those of the charter shown then.
    const GestureBase gesture = [self cmGestureBase];
    const std::vector<MotionKitEdit> batch =
        ui::charter::WithBrandFormat(_kitFile, edits);
    __weak CMCharterPanel* weakSelf = self;
    [alert
        beginSheetModalForWindow:_panel
               completionHandler:^(NSModalResponse response) {
                 CMCharterPanel* strongSelf = weakSelf;
                 if (strongSelf == nil || response != NSAlertFirstButtonReturn)
                     return;
                 [strongSelf cmWrite:batch
                               label:@"sous-titres réglés par format"
                                path:std::string()
                             gesture:gesture];
               }];
}

- (void)cmNewCharter:(id)sender {
    (void)sender;
    if ([CMFormPanel formShownInWindow:_panel] || _location.base.empty()) {
        NSBeep();
        return;
    }
    const CGFloat width = 380.0;
    NSView* form = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, 60)];
    NSTextField* nameLabel = [NSTextField labelWithString:@"Nom"];
    nameLabel.frame = NSMakeRect(0, 34, 90, 20);
    [form addSubview:nameLabel];
    NSTextField* nameField = [NSTextField textFieldWithString:@""];
    nameField.placeholderString = @"marque";
    nameField.frame = NSMakeRect(96, 34, width - 96, 22);
    nameField.autoresizingMask = NSViewWidthSizable;
    [form addSubview:nameField];
    NSTextField* descriptionLabel =
        [NSTextField labelWithString:@"Description"];
    descriptionLabel.frame = NSMakeRect(0, 2, 90, 20);
    [form addSubview:descriptionLabel];
    NSTextField* descriptionField = [NSTextField textFieldWithString:@""];
    descriptionField.placeholderString = @"Ce qu’est la marque, en une phrase";
    descriptionField.frame = NSMakeRect(96, 2, width - 96, 22);
    descriptionField.autoresizingMask = NSViewWidthSizable;
    [form addSubview:descriptionField];
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Nouvelle charte"
              message:@"Le nom est celui du fichier kits/<nom>.json : des "
                      @"lettres, des chiffres, - et _. La charte commence "
                      @"par la police des pièces livrées ; le reste se règle "
                      @"dans le panneau."
                 form:form];
    [panel addButtonWithTitle:@"Créer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    // What the form asks for, checked before it closes; the engine writes
    // it after, on the panel's queue (its lock may be held by the agent, and
    // a write syncs the disk), and a refusal of its own is said on the
    // panel's notice line.
    __block std::string created;
    __block std::string createdDescription;
    __weak CMCharterPanel* weakSelf = self;
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      CMCharterPanel* strongSelf = weakSelf;
      if (strongSelf == nil) return nil;
      NSCharacterSet* blank = NSCharacterSet.whitespaceAndNewlineCharacterSet;
      const std::string name =
          Utf8([nameField.stringValue stringByTrimmingCharactersInSet:blank]);
      const std::string description = Utf8(
          [descriptionField.stringValue stringByTrimmingCharactersInSet:blank]);
      if (!IsMotionPieceName(name))
          return @"Le nom s’écrit en lettres, chiffres, - et _, sans espace "
                 @"ni accent, 64 signes au plus.";
      const auto folded = [](std::string text) {
          for (char& c : text)
              if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
          return text;
      };
      for (const std::string& other : strongSelf->_names) {
          if (other == name) return @"Une charte porte déjà ce nom.";
          // The engine refuses it too (MotionLibraryBase.h): on a disk
          // that ignores case, the two would be one file.
          if (folded(other) == folded(name))
              return [NSString
                  stringWithFormat:@"« %@ » ne diffère de « %@ » que par les "
                                   @"majuscules : sur le disque d’un Mac, ce "
                                   @"serait le même fichier.",
                                   Str(name), Str(other)];
      }
      if (description.empty())
          return @"Une phrase de description est attendue : c’est par elle "
                 @"qu’on choisit une charte.";
      created = name;
      createdDescription = description;
      return nil;
    };
    [panel presentInWindow:_panel
                completion:^(NSModalResponse response) {
                  CMCharterPanel* strongSelf = weakSelf;
                  if (strongSelf == nil ||
                      response != NSAlertFirstButtonReturn || created.empty())
                      return;
                  [strongSelf cmCreateCharter:created
                                  description:createdDescription];
                }];
}

// The charter of the « Nouvelle charte » form, written on the panel's
// queue: shown once written, or the engine's refusal on the notice line.
- (void)cmCreateCharter:(const std::string&)kitName
            description:(const std::string&)description {
    const MotionLibraryLocation location = _location;
    // By value, as everything a block keeps (cmWrite).
    const std::string name = kitName;
    const mcp_json::Value file = ui::charter::NewKitFile(description);
    ++_pendingIo;
    __weak CMCharterPanel* weakSelf = self;
    dispatch_async(_ioQueue, ^{
      MotionKitSaved saved;
      MotionKitError error;
      const bool ok = SaveMotionKitVersion(location, name, file, 0,
                                           kPanelSource, saved, error);
      dispatch_async(dispatch_get_main_queue(), ^{
        CMCharterPanel* strongSelf = weakSelf;
        if (strongSelf == nil) return;
        --strongSelf->_pendingIo;
        if (!ok) {
            NSBeep();
            strongSelf->_writeNotice =
                Str(ui::charter::KitWriteRefusalMessage(name, error));
            strongSelf->_writeRefusal = error.code;
            [strongSelf cmRefreshNotice];
            return;
        }
        [strongSelf cmSelectCharter:name];
        strongSelf->_writeNotice =
            [NSString stringWithFormat:@"Charte « %@ » créée.", Str(name)];
      });
    });
}

// -- The lists.

- (NSInteger)numberOfRowsInTableView:(NSTableView*)table {
    if (table == _namesTable) return static_cast<NSInteger>(_names.size());
    return static_cast<NSInteger>(_versions.size());
}

- (NSView*)tableView:(NSTableView*)table
    viewForTableColumn:(NSTableColumn*)column
                   row:(NSInteger)row {
    (void)column;
    NSTextField* cell = [table makeViewWithIdentifier:@"charter.cell"
                                                owner:self];
    if (cell == nil) {
        cell = [NSTextField labelWithString:@""];
        cell.identifier = @"charter.cell";
        cell.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
        cell.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    const auto index = static_cast<size_t>(row);
    if (table == _namesTable && index < _names.size()) {
        const std::string& name = _names[index];
        const bool broken = _broken.count(name) > 0;
        cell.stringValue =
            broken ? [NSString stringWithFormat:@"%@ — illisible", Str(name)]
                   : Str(name);
        cell.textColor =
            broken ? CMThemeColor(ui::theme::kError) : CMTextPrimaryColor();
        cell.toolTip = broken ? @"Sa dernière version ou son fichier ne se "
                                @"lit plus"
                              : nil;
    } else if (index < _versions.size()) {
        const MotionKitVersion& version = _versions[index];
        NSString* text = [NSString
            stringWithFormat:@"v%lld · %@ · %@%@",
                             static_cast<long long>(version.version),
                             VersionDate(version.created_at),
                             VersionSource(version.source),
                             version.issue.empty() ? @"" : @" · illisible"];
        cell.stringValue = text;
        cell.textColor = version.version == _state.latest.version
                             ? CMTextPrimaryColor()
                             : CMTextSecondaryColor();
        cell.toolTip = Str(version.source);
    }
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    if (notification.object == _versionsTable) {
        [self cmRefreshRestore];
        return;
    }
    const NSInteger row = _namesTable.selectedRow;
    if (row < 0 || static_cast<size_t>(row) >= _names.size()) return;
    const std::string name = _names[static_cast<size_t>(row)];
    if (name == _selected) return;
    [self cmSelectCharter:name];
}

// -- The watch on kits/: a hand edit, the agent's write, read again.

- (void)cmStopWatching {
    if (_folderWatch) dispatch_source_cancel(_folderWatch);
    if (_fileWatch) dispatch_source_cancel(_fileWatch);
    _folderWatch = nil;
    _fileWatch = nil;
}

- (dispatch_source_t)cmWatchPath:(const std::string&)path
                           flags:(unsigned long)flags {
    const int descriptor = open(path.c_str(), O_EVTONLY);
    if (descriptor < 0) return nil;
    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_VNODE, static_cast<uintptr_t>(descriptor), flags,
        dispatch_get_main_queue());
    if (source == nil) {
        close(descriptor);
        return nil;
    }
    __weak CMCharterPanel* weakSelf = self;
    dispatch_source_set_event_handler(source, ^{
      [weakSelf cmWatchFired];
    });
    dispatch_source_set_cancel_handler(source, ^{
      close(descriptor);
    });
    dispatch_resume(source);
    return source;
}

// The folder (editors save by renaming a new file over the old one, which
// changes the folder) and the file shown (a save in place changes only
// the file).
- (void)cmWatch {
    [self cmStopWatching];
    if (!self.shown || _location.inbox.empty()) return;
    std::error_code error;
    const std::string kits = MotionKitDirectory(_location.inbox);
    const std::string folder = fs::is_directory(kits, error) ? kits
                               : fs::is_directory(_location.inbox, error)
                                   ? _location.inbox
                                   : std::string();
    if (!folder.empty())
        _folderWatch =
            [self cmWatchPath:folder
                        flags:DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE |
                              DISPATCH_VNODE_RENAME | DISPATCH_VNODE_LINK];
    if (!_state.path.empty() && fs::is_regular_file(_state.path, error))
        _fileWatch =
            [self cmWatchPath:_state.path
                        flags:DISPATCH_VNODE_WRITE | DISPATCH_VNODE_EXTEND |
                              DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME |
                              DISPATCH_VNODE_ATTRIB];
}

// A burst of events (a temporary file, a rename, an attribute) settles
// before anything is read.
- (void)cmWatchFired {
    const uint64_t generation = ++_watchGeneration;
    __weak CMCharterPanel* weakSelf = self;
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC),
        dispatch_get_main_queue(), ^{
          CMCharterPanel* strongSelf = weakSelf;
          if (strongSelf == nil || generation != strongSelf->_watchGeneration)
              return;
          [strongSelf cmWatchSettled];
        });
}

// Nothing new -- the panel's own write read back, an attribute touched --
// and only the watch is set again, on the file a rename put in place.
// Whether anything is new is the engine's to say, on the panel's queue
// (CH6 review): ReadMotionKitState reads kits/<name>.json as rule R does,
// a regular file only, never a link followed nor a pipe waited on, at
// most the size of a charter -- and a hand edit it finds, it imports, as
// the reload that follows would.
- (void)cmWatchSettled {
    if (!self.shown) return;
    if (!_hasState || _state.path.empty() ||
        _state.file != MotionKitFileState::Matches ||
        [self cmSelectionPending]) {
        [self cmReload];
        return;
    }
    const MotionLibraryLocation location = _location;
    const std::string name = _shown;
    const int64_t version = _state.latest.version;
    const std::set<std::string> kitFiles = _kitFiles;
    const uint64_t generation = _readGeneration;
    ++_pendingIo;
    __weak CMCharterPanel* weakSelf = self;
    dispatch_async(_ioQueue, ^{
      MotionKitState state;
      MotionKitError error;
      const bool unchanged =
          ReadMotionKitState(location, name, state, error) &&
          state.file == MotionKitFileState::Matches && state.imported.empty() &&
          state.latest.version == version && KitFileNames(location) == kitFiles;
      dispatch_async(dispatch_get_main_queue(), ^{
        CMCharterPanel* strongSelf = weakSelf;
        if (strongSelf == nil) return;
        --strongSelf->_pendingIo;
        // A read came back meanwhile: it set the watch again itself.
        if (!strongSelf.shown || generation != strongSelf->_readGeneration)
            return;
        if (unchanged)
            [strongSelf cmWatch];
        else
            [strongSelf cmReload];
      });
    });
}

// -- For the window test.

- (BOOL)waitUntilIdle:(NSTimeInterval)timeout {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
    while (_pendingIo > 0 || _pendingRender > 0 || _renderScheduled) {
        if ([deadline timeIntervalSinceNow] <= 0) return NO;
        [NSRunLoop.currentRunLoop
               runMode:NSDefaultRunLoopMode
            beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    return YES;
}

- (BOOL)selectCharterNamed:(NSString*)name {
    const std::string wanted = Utf8(name);
    if (std::find(_names.begin(), _names.end(), wanted) == _names.end())
        return NO;
    if (wanted != _selected) [self cmSelectCharter:wanted];
    return YES;
}

- (NSControl*)controlForPath:(NSString*)path {
    const std::string wanted = Utf8(path);
    for (NSView* item in _fieldItems)
        if ([item isKindOfClass:CMCharterFieldRow.class] &&
            ((CMCharterFieldRow*)item).field.path == wanted)
            return [(CMCharterFieldRow*)item control];
    return nil;
}

- (NSView*)frameViewAtIndex:(NSInteger)index {
    return index >= 0 && index < static_cast<NSInteger>(_frameViews.count)
               ? _frameViews[static_cast<NSUInteger>(index)]
               : nil;
}

- (BOOL)anchorOfPiece:(NSString*)piece
              inFrame:(NSInteger)index
                 quad:(Quad&)quad {
    const RenderedFrame* frame = [self cmRenderedFrame:index];
    if (frame == nullptr) return NO;
    const RenderedPiece* found = [self cmPiece:Utf8(piece) of:*frame];
    if (found == nullptr || !found->anchored) return NO;
    quad = found->quad;
    return YES;
}

@end
