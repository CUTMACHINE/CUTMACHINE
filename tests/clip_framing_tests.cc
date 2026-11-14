// R1 -- docs/roadmap/REEL-2026-09.md. The one rectangle: where a clip's
// picture lands in the sequence frame and which part of its source fills
// it.
//
// Portable on purpose (guard rail 1 of the lot): the monitor, the export,
// the sheets and verify_export all reach this arithmetic, and a second
// answer to the same question would diverge without any of them failing --
// the trap the drawn and the clickable timeline already set. Everything
// here runs without AppKit, Metal or FFmpeg.

#include "ClipFraming.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

std::string Describe(const ClipFramingGeometry& geometry) {
    return "source " + std::to_string(geometry.source_x) + ',' +
           std::to_string(geometry.source_y) + ' ' +
           std::to_string(geometry.source_width) + 'x' +
           std::to_string(geometry.source_height) + " -> frame " +
           std::to_string(geometry.x) + ',' + std::to_string(geometry.y) + ' ' +
           std::to_string(geometry.width) + 'x' +
           std::to_string(geometry.height) + " uncovered " +
           std::to_string(geometry.uncovered_left) + '/' +
           std::to_string(geometry.uncovered_top) + '/' +
           std::to_string(geometry.uncovered_right) + '/' +
           std::to_string(geometry.uncovered_bottom);
}

ClipFramingGeometry Resolve(const ClipFraming& framing, int64_t sourceWidth,
                            int64_t sourceHeight, int64_t frameWidth,
                            int64_t frameHeight) {
    ClipFramingGeometry geometry;
    std::string error;
    if (!ResolveClipFraming(framing, sourceWidth, sourceHeight, frameWidth,
                            frameHeight, geometry, error)) {
        throw std::runtime_error("refused: " + error);
    }
    return geometry;
}

EffectParamValue Fraction(int32_t num, int32_t den) { return {num, den}; }

// A one-clip document, so that the rotation step and the media lookup are
// exercised through the call every consumer actually makes.
Document DocumentWith(int32_t storedWidth, int32_t storedHeight,
                      int32_t rotation, const ClipFraming& framing,
                      int32_t sequenceWidth, int32_t sequenceHeight) {
    Document document;
    document.sequence.width = sequenceWidth;
    document.sequence.height = sequenceHeight;
    document.sequence.frame_rate = {25, 1};
    LibraryMedia media;
    media.id = "01K60000000000000000000001";
    media.path = "rush.mov";
    media.filename = "rush.mov";
    media.has_video = true;
    media.width = storedWidth;
    media.height = storedHeight;
    media.rotation_degrees = rotation;
    media.rate = {25, 1};
    media.duration = {1000, 25};
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sources.push_back(
        {media.id, media.path, media.rate, media.duration});
    DocumentClip clip;
    clip.id = "01K60000000000000000000011";
    clip.source_id = media.id;
    clip.source_in = {0, 25};
    clip.duration = {50, 25};
    clip.timeline_in = {0, 25};
    clip.include_audio = false;
    clip.framing = framing;
    document.sequence.tracks.push_back(
        {"01K60000000000000000000021", "video", 0, {clip}});
    return document;
}

}  // namespace

int main() {
    Test("the default framing is the fit-and-centre of before this ticket", [] {
        // A rush that matches the sequence is drawn over the whole
        // frame, whole: the one case where the rule and the historical
        // `force_original_aspect_ratio=decrease` plus centred overlay
        // provably name the same pixels, and the reason an unframed
        // clip may keep that chain.
        const ClipFramingGeometry same =
            Resolve(ClipFraming{}, 1920, 1080, 1920, 1080);
        Check(same.is_identity && same.covers_frame,
              "a matching rush is the identity: " + Describe(same));
        Check(same.x == 0 && same.y == 0 && same.width == 1920 &&
                  same.height == 1080 && same.source_x == 0 &&
                  same.source_width == 1920,
              "and fills the frame: " + Describe(same));

        // A 16:9 rush in a 9:16 sequence: the black bars R1 exists to
        // remove, named in pixels rather than found in the delivery.
        const ClipFramingGeometry bars =
            Resolve(ClipFraming{}, 1920, 1080, 1080, 1920);
        Check(bars.width == 1080 && bars.height == 608 && bars.x == 0 &&
                  bars.y == 656,
              "a 16:9 rush fitted in 1080x1920: " + Describe(bars));
        Check(bars.uncovered_top == 656 && bars.uncovered_bottom == 656 &&
                  bars.uncovered_left == 0 && bars.uncovered_right == 0 &&
                  !bars.covers_frame,
              "with the two bars measured: " + Describe(bars));
        // The frame is accounted for exactly: no pixel is invented and
        // none is lost between the bars and the picture.
        Check(
            bars.uncovered_top + bars.height + bars.uncovered_bottom == 1920,
            "the bars and the picture add up to the frame: " + Describe(bars));
    });

    Test("fill covers the frame at three horizontal positions", [] {
        // Criterion 1 of the ticket, as geometry: a 1920x1080 rush in a
        // 1080x1920 sequence, filled, leaves no edge uncovered, and the
        // source band it reads moves with the position.
        const EffectParamValue positions[3] = {Fraction(-1, 4), Fraction(0, 1),
                                               Fraction(1, 4)};
        int64_t previousSourceX = 1 << 30;
        for (int index = 0; index < 3; ++index) {
            ClipFraming framing;
            framing.mode = FramingMode::Fill;
            framing.position_x = positions[index];
            const ClipFramingGeometry geometry =
                Resolve(framing, 1920, 1080, 1080, 1920);
            Check(geometry.covers_frame, "fill leaves no bar at position " +
                                             std::to_string(index) + ": " +
                                             Describe(geometry));
            Check(geometry.x == 0 && geometry.y == 0 &&
                      geometry.width == 1080 && geometry.height == 1920,
                  "and draws over the whole frame: " + Describe(geometry));
            Check(geometry.source_height == 1080,
                  "the full height of the rush is used: " + Describe(geometry));
            // Moving the picture right slides the frame's window leftwards
            // over it: the band read moves the other way, which is the
            // whole reason this is measured rather than assumed.
            Check(geometry.source_x < previousSourceX,
                  "a position further right reads a band further left: " +
                      Describe(geometry));
            previousSourceX = geometry.source_x;
        }
        // The centred band, checked by hand: 1080 frame pixels at 16/9 of
        // magnification is 1080 * 9 / 16 = 607.5 source pixels, centred on
        // 960, so 656.25 to 1263.75 -- on the even grid, 656 to 1264.
        ClipFraming centred;
        centred.mode = FramingMode::Fill;
        const ClipFramingGeometry middle =
            Resolve(centred, 1920, 1080, 1080, 1920);
        Check(middle.source_x == 656 && middle.source_width == 608,
              "the centred band of a filled 16:9 rush: " + Describe(middle));
        // A quarter of the frame to the right moves the picture right, so
        // the band read moves left by a quarter of the frame expressed in
        // source pixels: 1080/4 = 270 frame pixels, 270 * 9/16 = 151.875.
        ClipFraming right;
        right.mode = FramingMode::Fill;
        right.position_x = Fraction(1, 4);
        const ClipFramingGeometry moved =
            Resolve(right, 1920, 1080, 1080, 1920);
        Check(
            moved.source_x == 504 && moved.source_width == 608,
            "a quarter right reads a band a quarter left: " + Describe(moved));
    });

    Test("a scale below one leaves the frame uncovered on all four edges", [] {
        // Half of a fit, on a rush whose aspect matches the frame: the
        // picture shrinks inside it and all four bars are reported.
        ClipFraming framing;
        framing.scale = Fraction(1, 2);
        const ClipFramingGeometry geometry =
            Resolve(framing, 1920, 1080, 1920, 1080);
        Check(!geometry.covers_frame,
              "half a fit no longer covers: " + Describe(geometry));
        Check(geometry.width == 960 && geometry.height == 540,
              "the picture is half the frame: " + Describe(geometry));
        Check(geometry.uncovered_left == 480 &&
                  geometry.uncovered_right == 480 &&
                  geometry.uncovered_top == 270 &&
                  geometry.uncovered_bottom == 270,
              "and the four bars are measured: " + Describe(geometry));
        Check(geometry.source_x == 0 && geometry.source_width == 1920 &&
                  geometry.source_y == 0 && geometry.source_height == 1080,
              "the whole rush is read, since nothing is cut: " +
                  Describe(geometry));
        // The engine measures; nothing here was derived by the caller.
        Check(geometry.full_x == geometry.x &&
                  geometry.full_width == geometry.width,
              "nothing overruns the frame: " + Describe(geometry));

        // Half of a fill still overruns sideways, because a fill of a
        // 16:9 rush into 9:16 magnifies by 16/9: the bars are on the
        // other axis, and the engine says which.
        ClipFraming halfFill;
        halfFill.mode = FramingMode::Fill;
        halfFill.scale = Fraction(1, 2);
        const ClipFramingGeometry sideways =
            Resolve(halfFill, 1920, 1080, 1080, 1920);
        Check(sideways.uncovered_top == 480 &&
                  sideways.uncovered_bottom == 480 &&
                  sideways.uncovered_left == 0 && sideways.uncovered_right == 0,
              "half a fill bars top and bottom only: " + Describe(sideways));
    });

    Test("the crop comes before the mode, not after", [] {
        // The order is what this pins. Cropping the right half of a
        // 1920x1080 rush leaves a 960x1080 picture, narrower than the 16:9
        // it was: filled into a 1080x1920 frame it is now limited by its
        // height, so its whole height is read and only part of its width.
        // Filling first and cropping after would keep the 16:9 band.
        ClipFraming framing;
        framing.crop_right = Fraction(1, 2);
        const ClipFramingGeometry geometry =
            Resolve(framing, 1920, 1080, 1080, 1920);
        // Cropped first, the picture is 960x1080 and fits the 1080-wide
        // frame on its width: 1080 across, 1215 down, bars above and below.
        Check(geometry.width == 1080 && geometry.height == 1215 &&
                  geometry.x == 0 && geometry.y == 353,
              "crop then fit: " + Describe(geometry));
        Check(geometry.source_x == 0 && geometry.source_width == 960 &&
                  geometry.source_y == 0 && geometry.source_height == 1080,
              "and the whole cropped half is read: " + Describe(geometry));
        // Fitting first would have given a 1080x608 picture, and cropping
        // half of that a 540x608 one: a quarter of the area this produces.
        // The two orders are not a matter of taste.
        const ClipFramingGeometry fittedWhole =
            Resolve(ClipFraming{}, 1920, 1080, 1080, 1920);
        Check(fittedWhole.height == 608 && geometry.height == 1215,
              "fitting first would have given another picture entirely: " +
                  Describe(fittedWhole) + " vs " + Describe(geometry));
    });

    Test("a crop that leaves no picture is refused, not clamped", [] {
        ClipFramingGeometry geometry;
        std::string error;
        ClipFraming whole;
        whole.crop_left = Fraction(1, 2);
        whole.crop_right = Fraction(1, 2);
        Check(!ResolveClipFraming(whole, 1920, 1080, 1080, 1920, geometry,
                                  error) &&
                  error.find("leave a picture") != std::string::npos,
              "half and half leaves nothing: " + error);
        ClipFraming past;
        past.crop_top = Fraction(3, 2);
        Check(!ResolveClipFraming(past, 1920, 1080, 1080, 1920, geometry,
                                  error) &&
                  error.find("below 1") != std::string::npos,
              "a crop past the whole edge is refused: " + error);
        ClipFraming negative;
        negative.crop_left = Fraction(-1, 4);
        Check(!ResolveClipFraming(negative, 1920, 1080, 1080, 1920, geometry,
                                  error),
              "a negative crop is refused: " + error);
        ClipFraming zeroScale;
        zeroScale.scale = Fraction(0, 1);
        Check(!ResolveClipFraming(zeroScale, 1920, 1080, 1080, 1920, geometry,
                                  error) &&
                  error.find("positive fraction") != std::string::npos,
              "a zero scale is refused: " + error);
        ClipFraming mirror;
        mirror.scale = Fraction(-1, 1);
        Check(!ResolveClipFraming(mirror, 1920, 1080, 1080, 1920, geometry,
                                  error),
              "a mirror is not a scale: " + error);
        ClipFraming offscreen;
        offscreen.position_x = Fraction(4, 1);
        Check(!ResolveClipFraming(offscreen, 1920, 1080, 1080, 1920, geometry,
                                  error) &&
                  error.find("outside the sequence frame") != std::string::npos,
              "a picture pushed entirely off screen is refused: " + error);
    });

    Test("a source rotated by 90 degrees is framed as it is displayed", [] {
        // Step 1 of the order. The file stores 1920x1080 with a quarter
        // turn in its display matrix; what the sequence sees is 1080x1920,
        // and choosing the stored size would frame a sideways picture.
        ClipFraming framing;
        framing.mode = FramingMode::Fill;
        const Document rotated =
            DocumentWith(1920, 1080, 90, framing, 1080, 1920);
        const DocumentClip& clip = rotated.sequence.tracks[0].clips[0];
        int64_t width = 0;
        int64_t height = 0;
        std::string error;
        Check(ClipDisplayedSize(rotated, clip, width, height, error) &&
                  width == 1080 && height == 1920,
              "the displayed size swaps: " + std::to_string(width) + 'x' +
                  std::to_string(height) + ' ' + error);
        ClipFramingGeometry geometry;
        Check(ResolveClipFramingForClip(rotated, clip, 1080, 1920, geometry,
                                        error),
              "and resolves: " + error);
        Check(geometry.is_identity,
              "a portrait rush fills a portrait sequence whole: " +
                  Describe(geometry));

        // The same file without the rotation flag is a landscape rush and
        // gets the band of the fill test, which is what says the swap is
        // not decorative.
        const Document upright =
            DocumentWith(1920, 1080, 0, framing, 1080, 1920);
        Check(ResolveClipFramingForClip(upright,
                                        upright.sequence.tracks[0].clips[0],
                                        1080, 1920, geometry, error),
              "the upright rush resolves: " + error);
        Check(!geometry.is_identity && geometry.source_width == 608,
              "and is cropped to a band: " + Describe(geometry));

        // A rotation that is not a right angle has no exact displayed size.
        const Document tilted =
            DocumentWith(1920, 1080, 45, framing, 1080, 1920);
        Check(!ClipDisplayedSize(tilted, tilted.sequence.tracks[0].clips[0],
                                 width, height, error) &&
                  error.find("right angle") != std::string::npos,
              "a 45 degree display matrix is refused: " + error);

        // A media that was never probed has no size to frame.
        Document unprobed = DocumentWith(1920, 1080, 0, framing, 1080, 1920);
        unprobed.library[0].metadata_complete = false;
        Check(!ClipDisplayedSize(unprobed, unprobed.sequence.tracks[0].clips[0],
                                 width, height, error) &&
                  error.find("never probed") != std::string::npos,
              "an unprobed media is refused: " + error);
    });

    Test("the frame is a frame at any size: a thumbnail agrees", [] {
        // verify_export resolves the same framing against the thumbnail
        // grid rather than the sequence. What must hold is that the
        // proportions survive: the band read is the same band.
        ClipFraming framing;
        framing.mode = FramingMode::Fill;
        framing.position_x = Fraction(1, 8);
        const ClipFramingGeometry full =
            Resolve(framing, 1920, 1080, 1080, 1920);
        const ClipFramingGeometry small = Resolve(framing, 1920, 1080, 36, 64);
        Check(small.covers_frame && small.width == 36 && small.height == 64,
              "the thumbnail is covered too: " + Describe(small));
        const double fullCentre = full.source_x + (full.source_width / 2.0);
        const double smallCentre = small.source_x + (small.source_width / 2.0);
        Check(std::abs(fullCentre - smallCentre) < 8.0,
              "and reads the same band, within the thumbnail's own pixel: " +
                  Describe(full) + " vs " + Describe(small));
    });

    Test("a reduced frame reads exactly the sequence's source region", [] {
        // Agentic QA of 23 September 2026: a quarter cropped off a 320x180
        // rush filled into 1080x1920. Resolved at 288x512, read_frame's
        // size, the edges round there and it read 80 source pixels where
        // the export reads 82 -- visible at a 13x enlargement.
        ClipFraming framing;
        framing.mode = FramingMode::Fill;
        framing.crop_left = Fraction(1, 4);
        framing.crop_top = Fraction(1, 5);
        const ClipFramingGeometry full = Resolve(framing, 320, 180, 1080, 1920);
        ClipFramingGeometry reduced;
        std::string error;
        Check(ResolveClipFramingReduced(framing, 320, 180, 1080, 1920, 288, 512,
                                        reduced, error),
              "the reduced framing resolves: " + error);
        Check(reduced.source_x == full.source_x &&
                  reduced.source_y == full.source_y &&
                  reduced.source_width == full.source_width &&
                  reduced.source_height == full.source_height,
              "the same source region: " + Describe(full) + " vs " +
                  Describe(reduced));
        Check(reduced.x == 0 && reduced.y == 0 && reduced.width == 288 &&
                  reduced.height == 512 && reduced.covers_frame,
              "drawn over the whole reduced frame: " + Describe(reduced));

        // A 16:9 picture fitted into 9:16 leaves 656 of 1920 rows above
        // and below; at 512 rows that is 175 above, 175 below.
        ClipFraming fit;
        fit.mode = FramingMode::Fit;
        ClipFramingGeometry bars;
        Check(ResolveClipFramingReduced(fit, 320, 180, 1080, 1920, 288, 512,
                                        bars, error) &&
                  bars.x == 0 && bars.width == 288 && bars.y == 175 &&
                  bars.height == 162 && bars.uncovered_top == 175 &&
                  bars.uncovered_bottom == 175 && !bars.covers_frame,
              "the bands scale with the frame: " + Describe(bars));
    });

    Test("a framing is reduced before it is stored", [] {
        ClipFraming framing;
        framing.scale = Fraction(22, 20);
        framing.position_x = Fraction(0, 4);
        framing.crop_left = Fraction(50, 100);
        const ClipFraming reduced = ReduceClipFraming(framing);
        Check(reduced.scale.num == 11 && reduced.scale.den == 10,
              "22/20 becomes 11/10");
        Check(reduced.position_x.num == 0 && reduced.position_x.den == 1,
              "0/4 becomes the 0/1 the file leaves out");
        Check(reduced.crop_left.num == 1 && reduced.crop_left.den == 2,
              "50/100 becomes 1/2");
        // A framing spelled entirely in its own defaults becomes the
        // default, so it is not written and does not raise the version.
        ClipFraming spelled;
        spelled.scale = Fraction(2, 2);
        spelled.crop_top = Fraction(0, 7);
        DocumentClip clip;
        clip.framing = spelled;
        Check(ClipHasFraming(clip),
              "spelled 2/2, it is a framing the file must write");
        clip.framing = ReduceClipFraming(spelled);
        Check(!ClipHasFraming(clip),
              "reduced, it is the default the file leaves out");
    });

    Test("the crop filter is empty when the whole source is read", [] {
        const ClipFramingGeometry whole =
            Resolve(ClipFraming{}, 1920, 1080, 1920, 1080);
        Check(ClipFramingCropFilter(whole, 1920, 1080).empty(),
              "an untouched source needs no crop");
        ClipFraming framing;
        framing.mode = FramingMode::Fill;
        const ClipFramingGeometry band =
            Resolve(framing, 1920, 1080, 1080, 1920);
        Check(ClipFramingCropFilter(band, 1920, 1080) == "crop=608:1080:656:0",
              "and a band is cropped where the rule put it: " +
                  ClipFramingCropFilter(band, 1920, 1080));
        // Every crop offset and size is even, so that a subsampled plane
        // can actually be cut there and FFmpeg never rounds behind us.
        for (int32_t numerator = 1; numerator <= 40; ++numerator) {
            ClipFraming moving;
            moving.mode = FramingMode::Fill;
            moving.position_x = Fraction(numerator - 20, 40);
            moving.scale = Fraction(100 + numerator, 100);
            const ClipFramingGeometry geometry =
                Resolve(moving, 1920, 1080, 1080, 1920);
            Check(geometry.source_x % 2 == 0 && geometry.source_y % 2 == 0 &&
                      geometry.source_width % 2 == 0 &&
                      geometry.source_height % 2 == 0,
                  "the crop stays on the even grid: " + Describe(geometry));
            Check(geometry.source_x >= 0 && geometry.source_y >= 0 &&
                      geometry.source_x + geometry.source_width <= 1920 &&
                      geometry.source_y + geometry.source_height <= 1080,
                  "and inside the source: " + Describe(geometry));
            Check(geometry.x >= 0 && geometry.y >= 0 &&
                      geometry.x + geometry.width <= 1080 &&
                      geometry.y + geometry.height <= 1920,
                  "and the picture inside the frame: " + Describe(geometry));
        }
    });

    Test("the zooms and offsets of the film measured resolve", [] {
        // The first film reframed 15 of 31 shots, zoom 1.1 to 1.45, offset
        // up to 100 px, always an enlargement -- never an uncovered edge.
        // Its rushes were 2160x3840 in a 1080x1920 sequence.
        // A rush of exactly twice the sequence is already filling it, so
        // the slack a zoom buys is (zoom - 1) * 1080 / 2 sideways. At 1.45
        // that is 243 px and every offset the film used fits.
        for (int32_t offset = -100; offset <= 100; offset += 50) {
            ClipFraming framing;
            framing.mode = FramingMode::Fill;
            framing.scale = Fraction(145, 100);
            framing.position_x = Fraction(offset, 1080);
            const ClipFramingGeometry geometry =
                Resolve(framing, 2160, 3840, 1080, 1920);
            Check(geometry.covers_frame,
                  "zoom 1.45 offset " + std::to_string(offset) +
                      " covers the frame: " + Describe(geometry));
        }
        // At 1.1 the slack is 54 px, and the same 100 px offset uncovers an
        // edge. The engine says so, in pixels, instead of delivering it --
        // which is the whole of guard rail 4 and the thing no criterion of
        // this ticket could see by looking at the picture.
        ClipFraming tooFar;
        tooFar.mode = FramingMode::Fill;
        tooFar.scale = Fraction(110, 100);
        tooFar.position_x = Fraction(100, 1080);
        const ClipFramingGeometry short_ =
            Resolve(tooFar, 2160, 3840, 1080, 1920);
        Check(!short_.covers_frame && short_.uncovered_left == 46 &&
                  short_.uncovered_right == 0,
              "zoom 1.1 with a 100 px offset uncovers 46 px on the left: " +
                  Describe(short_));
    });

    Test("the geometry is reported whole, so no caller derives one", [] {
        ClipFraming framing;
        framing.mode = FramingMode::Fill;
        framing.scale = Fraction(11, 10);
        const ClipFramingGeometry geometry =
            Resolve(framing, 1920, 1080, 1080, 1920);
        const std::string json = SerializeClipFramingGeometry(geometry);
        Check(json.find("\"sequence_rect\"") != std::string::npos &&
                  json.find("\"source_rect\"") != std::string::npos &&
                  json.find("\"uncovered\"") != std::string::npos &&
                  json.find("\"covers_frame\":true") != std::string::npos,
              "the reading tool reports the whole rectangle: " + json);
    });

    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All clip framing tests passed\n";
    return 0;
}
