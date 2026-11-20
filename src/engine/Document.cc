#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "ClipSpeed.h"
#include "LookLut.h"
#include "MotionClip.h"
#include "MotionDocument.h"
#include "Project.h"
#include "ShotDescriptions.h"
#include "TransitionRender.h"

#include "ColorEffects.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

bool OneOf(const std::string& value,
           std::initializer_list<const char*> allowed) {
    return std::any_of(allowed.begin(), allowed.end(),
                       [&](const char* item) { return value == item; });
}

bool RegisterId(const Ulid& id, const std::string& context, std::set<Ulid>& ids,
                std::string& error) {
    if (!IsValidUlid(id)) {
        error = context + " has invalid ULID '" + id + "'";
        return false;
    }
    if (!ids.insert(id).second) {
        error = "duplicate ID '" + id + "' at " + context;
        return false;
    }
    return true;
}

Ulid MigratedProjectId(const Ulid& sequenceId) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : sequenceId) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    static constexpr char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::string suffix(13, '0');
    for (size_t index = suffix.size(); index-- > 0;) {
        suffix[index] = alphabet[hash & 31U];
        hash >>= 5;
    }
    return "0000000000001" + suffix;
}

}  // namespace

void KeepMediaReading(const LibraryMedia& kept, LibraryMedia& probed) {
    probed.white_balance_kelvin = kept.white_balance_kelvin;
    probed.color_input = kept.color_input;
    // PL1 -- docs/roadmap/PLANS-2026-10.md. What was written about the
    // picture stays with the media, whichever file now carries it.
    probed.shots = kept.shots;
    // GN3 -- docs/roadmap/GENERATIF-2026-10.md. Where the picture came
    // from is no fact of the file: a relink keeps it.
    probed.generation = kept.generation;
}

Project Project::FromDocument(Document document, std::string projectName) {
    Project project(std::move(projectName));
    project.id = MigratedProjectId(document.sequence.id);
    project.settings.color_management = document.color_management;
    project.rushes = std::move(document.library);
    project.bins = std::move(document.bins);
    project.sources = std::move(document.sources);
    project.compositions = std::move(document.compositions);
    project.timelines = {std::move(document.sequence)};
    project.active_timeline_id = project.timelines.front().id;
    return project;
}

void RenewClipIdentity(DocumentClip& clip) {
    clip.id = GenerateUlid();
    for (ClipEffect& effect : clip.effects) effect.id = GenerateUlid();
}

bool Project::Load(const std::string& path, Project& output,
                   std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open project '" + path + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read project '" + path + "'";
        return false;
    }
    return LoadFromString(contents.str(), output, error);
}

bool Project::Save(const std::string& path, std::string& error) const {
    if (!Validate(error)) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to create project '" + path + "'";
        return false;
    }
    output << SaveToString();
    if (!output) {
        error = "unable to write project '" + path + "'";
        return false;
    }
    error.clear();
    return true;
}

bool Document::Load(const std::string& path, Document& output,
                    std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open document '" + path + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read document '" + path + "'";
        return false;
    }
    return LoadFromString(contents.str(), output, error);
}

bool Document::Save(const std::string& path, std::string& error) const {
    if (!Validate(error)) {
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to create document '" + path + "'";
        return false;
    }
    output << SaveToString();
    if (!output) {
        error = "unable to write document '" + path + "'";
        return false;
    }
    error.clear();
    return true;
}

bool ValidateCaptionStyle(const CaptionStyle& style, std::string& error) {
    const auto validText = [](const std::string& value, size_t maximum) {
        if (value.empty() || value.size() > maximum) return false;
        return std::none_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte < 0x20 || byte == 0x7f;
        });
    };
    if (!validText(style.font_family, 128) || style.font_size <= 0 ||
        !validText(style.color, 64) || !validText(style.position, 64)) {
        error = "has an invalid font_family, font_size, color or position";
        return false;
    }
    if (style.font_weight < kCaptionStyleMinFontWeight ||
        style.font_weight > kCaptionStyleMaxFontWeight) {
        error = "has a font_weight outside 1 to 1000";
        return false;
    }
    const EffectParamValue& vertical = style.vertical_position;
    if (style.outline_width < 0 ||
        style.outline_width > kCaptionStyleMaxOutlineWidth ||
        style.shadow_offset < 0 ||
        style.shadow_offset > kCaptionStyleMaxShadowOffset ||
        style.background_padding < 0 ||
        style.background_padding > kCaptionStyleMaxBackgroundPadding ||
        !validText(style.outline_color, 64) ||
        !validText(style.shadow_color, 64) ||
        (!style.background_color.empty() &&
         !validText(style.background_color, 64)) ||
        vertical.den <= 0 || vertical.num < 0 || vertical.num >= vertical.den ||
        (vertical.num == 0 && vertical.den != 1)) {
        error =
            "has an invalid outline, shadow, background or vertical_position";
        return false;
    }
    // PA4 -- the highlight colour is checked as the other colours are, and
    // whatever the animation: a style keeps a legal one to switch to.
    if ((!style.word_animation.empty() &&
         style.word_animation != kCaptionWordAnimationHighlight &&
         style.word_animation != kCaptionWordAnimationReveal) ||
        !validText(style.highlight_color, 64)) {
        error =
            "has a word_animation other than 'highlight' or 'reveal', or "
            "an invalid highlight_color";
        return false;
    }
    return true;
}

bool ClipMaskIsValid(const ClipMask& mask, std::string& error) {
    if (mask.shape == kClipMaskPerson) {
        // PA15 -- the matte says where the picture is kept: the geometry
        // of a shape stays at its defaults, so one person mask has one
        // spelling.
        const ClipMask defaults;
        const auto same = [](const EffectParamValue& left,
                             const EffectParamValue& right) {
            return left.den > 0 && right.den > 0 &&
                   static_cast<int64_t>(left.num) * right.den ==
                       static_cast<int64_t>(right.num) * left.den;
        };
        if (!same(mask.center_x, defaults.center_x) ||
            !same(mask.center_y, defaults.center_y) ||
            !same(mask.width, defaults.width) ||
            !same(mask.height, defaults.height) ||
            !same(mask.feather, defaults.feather)) {
            error =
                "has a 'person' mask with a centre, a size or a feather: "
                "the matte of the people is the shape";
            return false;
        }
        return true;
    }
    if (mask.shape != kClipMaskRectangle && mask.shape != kClipMaskEllipse) {
        error =
            "has a mask whose shape is neither 'rectangle', 'ellipse' nor "
            "'person'";
        return false;
    }
    const auto within = [](const EffectParamValue& value, int64_t lowNum,
                           int64_t highNum, bool lowOpen) {
        if (value.den <= 0) return false;
        const int64_t num = value.num;
        const int64_t den = value.den;
        return (lowOpen ? num > lowNum * den : num >= lowNum * den) &&
               num <= highNum * den;
    };
    if (!within(mask.center_x, -1, 2, false) ||
        !within(mask.center_y, -1, 2, false)) {
        error = "has a mask whose centre lies outside [-1, 2] of its picture";
        return false;
    }
    if (!within(mask.width, 0, 4, true) || !within(mask.height, 0, 4, true)) {
        error =
            "has a mask whose width or height lies outside ]0, 4] of its "
            "picture";
        return false;
    }
    if (!within(mask.feather, 0, 1, false)) {
        error = "has a mask whose feather lies outside [0, 1]";
        return false;
    }
    return true;
}

ClipMask ReduceClipMask(const ClipMask& mask) {
    if (mask.shape.empty()) return ClipMask{};
    const auto reduced = [](EffectParamValue value) {
        if (value.den <= 0) return value;
        if (value.num == 0) return EffectParamValue{0, 1};
        const int32_t divisor =
            std::gcd(value.num < 0 ? -value.num : value.num, value.den);
        return EffectParamValue{value.num / divisor, value.den / divisor};
    };
    ClipMask out = mask;
    out.center_x = reduced(mask.center_x);
    out.center_y = reduced(mask.center_y);
    out.width = reduced(mask.width);
    out.height = reduced(mask.height);
    out.feather = reduced(mask.feather);
    return out;
}

size_t CaptionWordCount(const std::string& text) {
    size_t count = 0;
    bool inside = false;
    for (const char byte : text) {
        const bool space =
            byte == ' ' || byte == '\n' || byte == '\t' || byte == '\r';
        if (!space && !inside) ++count;
        inside = !space;
    }
    return count;
}

RationalTime ReducedCaptionWordTime(const RationalTime& time) {
    if (time.rate <= 0) return time;
    if (time.value == 0) return {0, 1};
    const int64_t divisor = std::gcd(time.value < 0 ? -time.value : time.value,
                                     static_cast<int64_t>(time.rate));
    return {time.value / divisor, static_cast<int32_t>(time.rate / divisor)};
}

int32_t CaptionWordsSpoken(const DocumentClip& clip,
                           const RationalTime& position) {
    if (clip.caption_word_times.empty()) return 0;
    const RationalTime offset =
        position.sub(clip.timeline_in).add(clip.source_in);
    int32_t spoken = 0;
    for (const RationalTime& time : clip.caption_word_times) {
        if (time.compare(offset) <= 0) ++spoken;
    }
    return spoken;
}

bool Document::Validate(std::string& error) const {
    return Validate(error, CompositionCheck::Check);
}

bool Document::Validate(std::string& error,
                        CompositionCheck compositionCheck) const {
    if (version != 7) {
        error = "unsupported document version " + std::to_string(version);
        return false;
    }
    if (!IsValidUlid(sequence.id) || sequence.name.empty() ||
        sequence.width <= 0 || sequence.height <= 0 || sequence.width > 16384 ||
        sequence.height > 16384 || sequence.frame_rate.num <= 0 ||
        sequence.frame_rate.den <= 0) {
        error = "sequence has an invalid ID, name, dimensions or frame rate";
        return false;
    }

    std::string inputError;
    if (!ValidateColorInputNames(color_management.input_gamut,
                                 color_management.input_transfer,
                                 color_management.input_ycbcr_matrix,
                                 color_management.input_range, inputError) ||
        !OneOf(color_management.working_gamut,
               {"acescct", "rec2020", "rec709"}) ||
        !OneOf(color_management.output_gamut, {"rec709", "rec2020"}) ||
        !OneOf(color_management.output_transfer, {"rec709", "hlg"})) {
        error =
            "color_management contains an unsupported color space or transfer";
        return false;
    }
    if (color_management.output_transfer == "hlg" &&
        color_management.output_gamut != "rec2020") {
        error = "HLG output requires the rec2020 output gamut";
        return false;
    }
    if (color_management.enabled) {
        if (!IsSupportedColorInput(color_management.input_gamut,
                                   color_management.input_transfer)) {
            error =
                "color_management input gamut/transfer combination is "
                "not available in the embedded OpenColorIO config";
            return false;
        }
    }

    // R6 -- docs/roadmap/REEL-2026-09.md. The look LUT is a reference, and
    // this is where its shape and its declared space are decided, once, for
    // every surface: the same answer the monitor and the export then read
    // (LookLut.h). The file itself is checked where it is read, because a
    // Document knows nothing of the package it came from.
    {
        const SequenceLut& look = sequence.look_lut;
        if (look.digest.empty() &&
            (!look.name.empty() || !look.space.empty())) {
            error =
                "sequence look_lut names a LUT without a digest: it names no "
                "file at all";
            return false;
        }
        if (!look.digest.empty()) {
            if (!IsLookLutDigest(look.digest)) {
                error =
                    "sequence look_lut digest is not a 64-character "
                    "lowercase SHA-256";
                return false;
            }
            if (look.name.size() > 255 ||
                look.name.find('/') != std::string::npos ||
                look.name.find('\\') != std::string::npos ||
                std::any_of(look.name.begin(), look.name.end(),
                            [](unsigned char letter) {
                                return letter < 0x20 || letter == 0x7f;
                            })) {
                error =
                    "sequence look_lut name must be a plain file name, not a "
                    "path";
                return false;
            }
            LookLutStage stage = LookLutStage::None;
            if (!ResolveLookLutStage(look, color_management, stage, error))
                return false;
        }
    }

    std::set<Ulid> ids;
    if (!RegisterId(sequence.id, "sequence", ids, error)) return false;
    std::set<Ulid> binIds;
    for (size_t index = 0; index < bins.size(); ++index) {
        const DocumentBin& bin = bins[index];
        const std::string context = "bin " + std::to_string(index);
        if (!RegisterId(bin.id, context, ids, error)) return false;
        if (bin.name.empty()) {
            error = context + " has an empty name";
            return false;
        }
        binIds.insert(bin.id);
    }
    for (size_t index = 0; index < bins.size(); ++index) {
        const DocumentBin& bin = bins[index];
        if (!bin.parent_id.empty() &&
            binIds.find(bin.parent_id) == binIds.end()) {
            error = "bin " + std::to_string(index) +
                    " references unknown parent_id '" + bin.parent_id + "'";
            return false;
        }
        std::set<Ulid> ancestors;
        const DocumentBin* cursor = &bin;
        while (!cursor->parent_id.empty()) {
            if (!ancestors.insert(cursor->id).second) {
                error = "bin hierarchy contains a cycle at '" + bin.id + "'";
                return false;
            }
            cursor = FindBin(cursor->parent_id);
            if (!cursor) break;
        }
    }
    const auto validMarkerText = [](const std::string& value, size_t maximum) {
        if (value.empty() || value.size() > maximum) return false;
        return std::none_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte < 0x20 || byte == 0x7f;
        });
    };
    for (size_t index = 0; index < sequence.markers.size(); ++index) {
        const DocumentMarker& marker = sequence.markers[index];
        const std::string context = "marker " + std::to_string(index);
        if (!RegisterId(marker.id, context, ids, error)) return false;
        if (!validMarkerText(marker.name, 4096) ||
            !validMarkerText(marker.color, 64) ||
            !validMarkerText(marker.category, 128)) {
            error = context + " has an invalid name, color or category";
            return false;
        }
        if (marker.duration.rate <= 0 || marker.duration.value < 0) {
            error = context + " has an invalid duration";
            return false;
        }
        if (marker.time.rate <= 0 || marker.time.value < 0) {
            error = context + " has an invalid time";
            return false;
        }
    }
    std::set<Ulid> libraryIds;
    for (size_t index = 0; index < library.size(); ++index) {
        const LibraryMedia& media = library[index];
        const std::string context = "library media " + std::to_string(index);
        if (!IsValidUlid(media.id)) {
            error = context + " has invalid ULID '" + media.id + "'";
            return false;
        }
        if (!libraryIds.insert(media.id).second) {
            error = "duplicate library ID '" + media.id + "'";
            return false;
        }
        if (media.path.empty() || media.filename.empty()) {
            error = context + " has an empty path or filename";
            return false;
        }
        if (!media.bin_id.empty() &&
            binIds.find(media.bin_id) == binIds.end()) {
            error =
                context + " references unknown bin_id '" + media.bin_id + "'";
            return false;
        }
        if (media.rate.num <= 0 || media.rate.den <= 0 ||
            media.duration.rate <= 0 || media.duration.value <= 0) {
            error = context + " has an invalid rate or duration";
            return false;
        }
        if (media.white_balance_kelvin != 0 &&
            (media.white_balance_kelvin < kMinimumMediaWhiteBalance ||
             media.white_balance_kelvin > kMaximumMediaWhiteBalance)) {
            error = context + " has an out-of-range white balance";
            return false;
        }
        // G3 -- a declared input is one the pipeline can read, whether or
        // not the project manages colour today: turning management on must
        // not find a media it cannot decode.
        if (media.color_input.graphics) {
            // DEC31 -- a graphic declares no curve: its matrix and range
            // are the only reading it takes, and a sound has no picture.
            std::string inputError;
            if (!media.color_input.gamut.empty() ||
                !media.color_input.transfer.empty()) {
                error = context +
                        " is declared a graphic and a camera input at once";
                return false;
            }
            if (!ValidateColorInputNames("rec709", "rec709",
                                         media.color_input.ycbcr_matrix,
                                         media.color_input.range, inputError)) {
                error = context +
                        " declares a graphic it cannot read: " + inputError;
                return false;
            }
            if (media.metadata_complete && !media.has_video) {
                error = context + " is audio-only and cannot be a graphic";
                return false;
            }
        } else if (DeclaresColorInput(media.color_input)) {
            std::string inputError;
            if (!ValidateColorInputNames(media.color_input.gamut,
                                         media.color_input.transfer,
                                         media.color_input.ycbcr_matrix,
                                         media.color_input.range, inputError) ||
                !IsSupportedColorInput(media.color_input.gamut,
                                       media.color_input.transfer)) {
                error = context + " declares an unsupported colour input";
                return false;
            }
        } else if (media.color_input != MediaColorInput{}) {
            error = context +
                    " spells a colour input without a transfer: a "
                    "declaration names both gamut and transfer";
            return false;
        }
        // PL1 -- docs/roadmap/PLANS-2026-10.md. Described shots inside the
        // media, in order, and in the document's one space of IDs.
        if (!ValidateMediaShots(media, media.shots, error)) {
            error = context + ": " + error;
            return false;
        }
        for (const ShotDescription& shot : media.shots)
            if (!RegisterId(shot.id, context + " shot", ids, error))
                return false;
        // GN3 -- docs/roadmap/GENERATIF-2026-10.md. A provenance names its
        // service, its model, what was asked and when, or it is none.
        if (media.generation != MediaGeneration{}) {
            const MediaGeneration& generation = media.generation;
            if (generation.service.empty() || generation.model.empty() ||
                generation.kind.empty() || generation.created_at.empty()) {
                error = context +
                        " has a generation without its service, model, kind "
                        "or date";
                return false;
            }
        }
        if (media.metadata_complete) {
            if (media.codec.empty() || (!media.has_video && !media.has_audio)) {
                error = context + " has no usable audio or video stream";
                return false;
            }
            if (media.has_video && (media.width <= 0 || media.height <= 0 ||
                                    media.rotation_degrees < -180 ||
                                    media.rotation_degrees > 180 ||
                                    (media.orientation != "landscape" &&
                                     media.orientation != "portrait" &&
                                     media.orientation != "square"))) {
                error = context + " has invalid video metadata";
                return false;
            }
            if (!media.has_video &&
                (media.width != 0 || media.height != 0 ||
                 media.rotation_degrees != 0 || media.orientation != "audio")) {
                error = context + " has picture metadata without video";
                return false;
            }
            if (media.has_audio &&
                (media.audio_rate <= 0 || media.audio_channels <= 0)) {
                error = context + " has invalid audio metadata";
                return false;
            }
            if (media.audio_level_measured &&
                (!media.has_audio || media.audio_level < 0 ||
                 media.audio_level > kAudioLevelScale)) {
                error = context + " has an out-of-range audio level";
                return false;
            }
        }
    }
    std::set<Ulid> sourceIds;
    for (size_t index = 0; index < sources.size(); ++index) {
        const DocumentSource& source = sources[index];
        const std::string context = "source " + std::to_string(index);
        if (!RegisterId(source.id, context, ids, error)) return false;
        sourceIds.insert(source.id);
        if (source.path.empty()) {
            error = context + " ('" + source.id + "') has an empty path";
            return false;
        }
        if (source.rate.num <= 0 || source.rate.den <= 0) {
            error = context + " ('" + source.id +
                    "') has a zero or negative media rate";
            return false;
        }
        if (source.duration.rate <= 0) {
            error = context + " ('" + source.id +
                    "') has a zero or negative duration rate";
            return false;
        }
        if (source.duration.value <= 0) {
            error = context + " ('" + source.id +
                    "') has a zero or negative duration";
            return false;
        }
    }
    // A mounted source deliberately shares its media ULID. Library-only IDs,
    // however, must remain globally distinct from tracks and clips.
    for (const Ulid& libraryId : libraryIds) {
        if (sourceIds.find(libraryId) == sourceIds.end() &&
            !ids.insert(libraryId).second) {
            error = "library ID '" + libraryId +
                    "' collides with another document object";
            return false;
        }
    }

    // MOTION-2026-09 M4 -- each composition a motion clip names, parsed
    // once: a timeline of forty cartons of one title card parses it once.
    std::map<Ulid, std::optional<MotionComposition>> parsedCompositions;
    const auto parsedComposition =
        [&](const Ulid& id) -> const MotionComposition* {
        auto found = parsedCompositions.find(id);
        if (found == parsedCompositions.end()) {
            std::optional<MotionComposition> parsed;
            if (const DocumentComposition* stored = FindComposition(id)) {
                MotionComposition composition;
                MotionError ignored;
                if (ParseDocumentComposition(*stored, composition, ignored)) {
                    parsed = std::move(composition);
                }
            }
            found = parsedCompositions.emplace(id, std::move(parsed)).first;
        }
        return found->second ? &*found->second : nullptr;
    };
    // Each clip looks its media and its source up by id. A linear Find per
    // clip made validation O(clips x rushes) -- 40 million string compares
    // for 40 000 clips over 1 000 rushes -- and every edit validates
    // (ValidateResult). Indexed once here; the first entry of an id wins, as
    // with Find, and duplicates were refused above.
    std::unordered_map<std::string_view, const LibraryMedia*> mediaById;
    mediaById.reserve(library.size());
    for (const LibraryMedia& media : library)
        mediaById.emplace(media.id, &media);
    std::unordered_map<std::string_view, const DocumentSource*> sourceById;
    sourceById.reserve(sources.size());
    for (const DocumentSource& source : sources)
        sourceById.emplace(source.id, &source);
    const auto mediaOf = [&](const Ulid& id) -> const LibraryMedia* {
        const auto found = mediaById.find(id);
        return found == mediaById.end() ? nullptr : found->second;
    };
    const auto sourceOf = [&](const Ulid& id) -> const DocumentSource* {
        const auto found = sourceById.find(id);
        return found == sourceById.end() ? nullptr : found->second;
    };
    std::set<int32_t> trackIndices;
    for (size_t trackIndex = 0; trackIndex < sequence.tracks.size();
         ++trackIndex) {
        const DocumentTrack& track = sequence.tracks[trackIndex];
        const std::string trackContext = "track " + std::to_string(trackIndex);
        if (!RegisterId(track.id, trackContext, ids, error)) return false;
        if (!trackIndices.insert(track.index).second) {
            error = "duplicate track index " + std::to_string(track.index);
            return false;
        }
        const DocumentClip* previous = nullptr;
        for (size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            const DocumentClip& clip = track.clips[clipIndex];
            const std::string context =
                trackContext + " clip " + std::to_string(clipIndex);
            if (!RegisterId(clip.id, context, ids, error)) return false;
            if (!clip.link_group_id.empty() &&
                !IsValidUlid(clip.link_group_id)) {
                error = context + " ('" + clip.id +
                        "') has invalid link_group_id '" + clip.link_group_id +
                        "'";
                return false;
            }
            if (!clip.sync_anchor_clip_id.empty() &&
                (!IsValidUlid(clip.sync_anchor_clip_id) ||
                 clip.sync_reference_delta.rate <= 0)) {
                error = context + " ('" + clip.id +
                        "') has invalid synchronization reference";
                return false;
            }
            if (clip.link_group_id.empty() &&
                !clip.sync_anchor_clip_id.empty()) {
                error = context + " ('" + clip.id +
                        "') has an incomplete link synchronization state";
                return false;
            }
            const bool captionClip = track.kind == "caption";
            if (captionClip && !clip.source_id.empty()) {
                error = context + " ('" + clip.id +
                        "') caption clip must not reference media";
                return false;
            }
            // MOTION-2026-09 M4 -- a clip whose source is a composition.
            const bool motionClip = IsMotionClip(clip);
            if (motionClip || !clip.motion_props.IsNull()) {
                std::string motionError;
                if (!ValidateMotionClip(
                        track, clip, FindComposition(clip.composition_id),
                        parsedComposition(clip.composition_id), motionError)) {
                    error = context;
                    error += ' ';
                    error += motionError;
                    return false;
                }
            }
            if (!captionClip && !motionClip &&
                sourceIds.find(clip.source_id) == sourceIds.end()) {
                error = context + " ('" + clip.id +
                        "') references unknown source_id '" + clip.source_id +
                        "'";
                return false;
            }
            if (clip.source_in.rate <= 0 || clip.duration.rate <= 0 ||
                clip.timeline_in.rate <= 0) {
                error = context + " ('" + clip.id +
                        "') has a zero or negative time rate";
                return false;
            }
            if (clip.duration.value <= 0) {
                error = context + " ('" + clip.id +
                        "') has a zero or negative duration";
                return false;
            }
            if (clip.source_in.value < 0) {
                error = context + " ('" + clip.id +
                        "') has source_in before source start";
                return false;
            }
            if (clip.timeline_in.value < 0) {
                error = context + " ('" + clip.id +
                        "') has timeline_in before zero";
                return false;
            }
            if (clip.opacity.den <= 0 || clip.opacity.num < 0 ||
                clip.opacity.num > clip.opacity.den) {
                error = context + " ('" + clip.id +
                        "') has opacity outside the exact [0, 1] range";
                return false;
            }
            // EF3 -- docs/roadmap/EFFETS-2026-10.md. A blur is a sigma in
            // sequence pixels, from none to kMaxClipBlur, and belongs to a
            // picture of a media on a video track. A motion clip's blur is
            // its composition's (a node's `blur`, drawn with it), and a
            // carton is drawn by the caption renderer, which has none.
            if (clip.blur.den <= 0 || clip.blur.num < 0 ||
                static_cast<int64_t>(clip.blur.num) >
                    static_cast<int64_t>(kMaxClipBlur) * clip.blur.den) {
                error = context + " ('" + clip.id +
                        "') has a blur outside the exact [0, " +
                        std::to_string(kMaxClipBlur) +
                        "] range of sequence pixels";
                return false;
            }
            if (ClipHasBlur(clip)) {
                if (track.kind != "video" || captionClip) {
                    error = context + " ('" + clip.id +
                            "') carries a blur on a track that is not video";
                    return false;
                }
                if (motionClip) {
                    error = context + " ('" + clip.id +
                            "') is a motion clip and cannot carry a blur: a "
                            "composition blurs its own nodes";
                    return false;
                }
                // The blur is drawn at the size the picture has on screen,
                // and every renderer sizes it once per clip: a picture whose
                // size or place changes every frame would need a sigma per
                // frame that FFmpeg's chain cannot be handed.
                for (const ClipParameterAnimation& entry : clip.animation) {
                    if (entry.parameter != ClipAnimatedParameter::Opacity) {
                        error = context + " ('" + clip.id +
                                "') carries a blur and animates its framing; "
                                "a blurred picture keeps one size and one "
                                "place";
                        return false;
                    }
                }
            }
            // PA8 -- docs/roadmap/PARITE-2026-11.md. A mask cuts out the
            // picture of a media on a video track. A motion clip draws its
            // own shapes, a carton is drawn by the caption renderer.
            if (ClipHasMask(clip)) {
                std::string maskError;
                if (!ClipMaskIsValid(clip.mask, maskError)) {
                    error = context + " ('" + clip.id + "') " + maskError;
                    return false;
                }
                if (track.kind != "video" || captionClip) {
                    error = context + " ('" + clip.id +
                            "') carries a mask on a track that is not video";
                    return false;
                }
                if (motionClip) {
                    error = context + " ('" + clip.id +
                            "') is a motion clip and cannot carry a mask: a "
                            "composition draws its own shapes";
                    return false;
                }
                // PA15 -- the people of a graphic are drawn, not filmed,
                // and its own alpha would be replaced by the matte.
                if (ClipHasPersonMask(clip)) {
                    const LibraryMedia* media = mediaOf(clip.source_id);
                    if (media != nullptr && media->color_input.graphics) {
                        error = context + " ('" + clip.id +
                                "') shows a graphic and cannot carry a "
                                "'person' mask";
                        return false;
                    }
                }
            } else {
                const ClipMask none;
                const auto same = [](const EffectParamValue& left,
                                     const EffectParamValue& right) {
                    return left.num == right.num && left.den == right.den;
                };
                if (!same(clip.mask.center_x, none.center_x) ||
                    !same(clip.mask.center_y, none.center_y) ||
                    !same(clip.mask.width, none.width) ||
                    !same(clip.mask.height, none.height) ||
                    !same(clip.mask.feather, none.feather) ||
                    clip.mask.inverted) {
                    error = context + " ('" + clip.id +
                            "') places a mask that has no shape";
                    return false;
                }
            }
            // R3 -- docs/roadmap/REEL-2026-09.md.
            std::string speedError;
            if (!ClipSpeedIsValid(clip.speed, speedError)) {
                error = context + " ('" + clip.id + "') " + speedError;
                return false;
            }
            // R3, criterion 4 -- a ramp: its keys, then where it may sit.
            // Picture only, like a freeze (ClipSpeed.h), and the only speed
            // the clip has.
            if (!ClipSpeedRampIsValid(clip.speed_ramp, speedError)) {
                error = context + " ('" + clip.id + "') " + speedError;
                return false;
            }
            if (ClipHasSpeedRamp(clip)) {
                if (clip.speed.num != 1 || clip.speed.den != 1) {
                    error = context + " ('" + clip.id +
                            "') carries both a constant speed and a speed "
                            "ramp; a ramped clip's speed is its keys";
                    return false;
                }
                if (track.kind != "video" || captionClip || motionClip) {
                    error = context + " ('" + clip.id +
                            "') carries a speed ramp off a video clip of a "
                            "media; a ramp is picture only";
                    return false;
                }
                if (!clip.link_group_id.empty()) {
                    error = context + " ('" + clip.id +
                            "') carries a speed ramp in a link group; its "
                            "sound cannot follow a speed that changes every "
                            "frame";
                    return false;
                }
            }
            // R1 -- docs/roadmap/REEL-2026-09.md.
            std::string framingError;
            if (!ClipFramingIsValid(clip.framing, framingError)) {
                error = context + " ('" + clip.id + "') " + framingError;
                return false;
            }
            if (ClipHasFraming(clip)) {
                if (captionClip || motionClip) {
                    error = context + " ('" + clip.id +
                            "') is a caption or motion clip and cannot carry "
                            "a framing";
                    return false;
                }
                if (track.kind != "video") {
                    error = context + " ('" + clip.id +
                            "') carries a framing on a track that is not "
                            "video";
                    return false;
                }
                // The rectangle it implies has to exist: a framing whose
                // media was never probed, or which leaves nothing on
                // screen, is refused here rather than delivered as a black
                // frame (guard rail 4 -- the engine measures).
                ClipFramingGeometry geometry;
                if (!ResolveClipFramingForClip(*this, clip, sequence.width,
                                               sequence.height, geometry,
                                               framingError)) {
                    error = context + " ('" + clip.id + "') " + framingError;
                    return false;
                }
            }
            // R2 -- docs/roadmap/REEL-2026-09.md. The shape of the keys,
            // then the rectangle every one of the clip's frames implies:
            // an animation that pushes the picture off the screen halfway
            // through is refused when it is set, not discovered in the
            // delivery (guard rail 4 -- the engine measures). It costs one
            // exact-rational rectangle per frame of an animated clip, and
            // only of an animated clip.
            std::string animationError;
            if (!ClipAnimationIsValid(clip.animation, animationError)) {
                error = context + " ('" + clip.id + "') " + animationError;
                return false;
            }
            if (ClipHasAnimation(clip)) {
                if (track.kind != "video") {
                    error = context + " ('" + clip.id +
                            "') animates a parameter on a track that is not "
                            "video";
                    return false;
                }
                const bool animatesFraming =
                    std::any_of(clip.animation.begin(), clip.animation.end(),
                                [](const ClipParameterAnimation& entry) {
                                    return entry.parameter !=
                                           ClipAnimatedParameter::Opacity;
                                });
                if (animatesFraming && (captionClip || motionClip)) {
                    error = context + " ('" + clip.id +
                            "') is a caption or motion clip and cannot "
                            "animate its framing";
                    return false;
                }
                int64_t animatedFrames = 0;
                if (!ClipFrameCount(clip, sequence.frame_rate, animatedFrames,
                                    animationError)) {
                    error = context + " ('" + clip.id + "') " + animationError;
                    return false;
                }
                for (int64_t frame = 0; frame < animatedFrames; ++frame) {
                    ClipSampledParameters sampled;
                    if (!SampleClipParameters(clip, frame, sequence.frame_rate,
                                              sampled, animationError)) {
                        error = context + " ('" + clip.id + "') at frame " +
                                std::to_string(frame) + ": " + animationError;
                        return false;
                    }
                    if (!animatesFraming || captionClip || motionClip) continue;
                    int64_t displayedWidth = 0;
                    int64_t displayedHeight = 0;
                    ClipFramingGeometry geometry;
                    if (!ClipDisplayedSize(*this, clip, displayedWidth,
                                           displayedHeight, animationError) ||
                        !ResolveClipFraming(sampled.framing, displayedWidth,
                                            displayedHeight, sequence.width,
                                            sequence.height, geometry,
                                            animationError)) {
                        error = context + " ('" + clip.id + "') at frame " +
                                std::to_string(frame) + ": " + animationError;
                        return false;
                    }
                }
            }
            if (clip.audio_gain_db.den <= 0 ||
                static_cast<int64_t>(clip.audio_gain_db.num) <
                    -120LL * clip.audio_gain_db.den ||
                static_cast<int64_t>(clip.audio_gain_db.num) >
                    48LL * clip.audio_gain_db.den) {
                error = context + " ('" + clip.id +
                        "') has gain outside the exact [-120, 48] dB range";
                return false;
            }
            if (clip.audio_fade_in.rate <= 0 || clip.audio_fade_out.rate <= 0 ||
                clip.audio_fade_in.value < 0 || clip.audio_fade_out.value < 0 ||
                clip.audio_fade_in > clip.duration ||
                clip.audio_fade_out > clip.duration) {
                error = context + " ('" + clip.id +
                        "') has an invalid audio fade duration";
                return false;
            }
            const bool hasAudioProcessing =
                clip.audio_gain_db.num != 0 || clip.audio_gain_db.den != 1 ||
                clip.audio_fade_in.value != 0 || clip.audio_fade_out.value != 0;
            if (hasAudioProcessing && track.kind != "audio") {
                error = context + " ('" + clip.id +
                        "') has audio processing on a non-audio track";
                return false;
            }
            const DocumentSource* source =
                captionClip || motionClip ? nullptr : sourceOf(clip.source_id);
            const LibraryMedia* media =
                captionClip || motionClip ? nullptr : mediaOf(clip.source_id);
            if (media && media->metadata_complete && track.kind == "video" &&
                !media->has_video) {
                error = context + " ('" + clip.id +
                        "') uses audio-only media on a video track";
                return false;
            }
            try {
                // R3 -- what a clip reads of its source is its duration
                // times its speed (ClipSpeed.h), not its duration.
                if (source != nullptr &&
                    ClipSourceEnd(clip) > source->duration) {
                    error = context + " ('" + clip.id +
                            "') has source_in + duration outside source bounds";
                    return false;
                }
                // R3 -- the invariant that makes the monitor and the export
                // name the same source frame: at any speed other than 1/1,
                // source_in must fall on a source frame, because the export
                // reopens the stream at an effective cadence and counts from
                // the frame the seek landed on (ClipSpeed.h). An edit that
                // would move it inside a frame is refused here rather than
                // delivered one frame away from what was monitored.
                // Criterion 4 -- a ramp is exported as an explicit list from
                // the frame its first image starts on, so it asks nothing of
                // source_in (ClipSpeed.h).
                if (source != nullptr && !IsUnitSpeed(clip.speed) &&
                    !IsOnSourceFrameBoundary(clip.source_in, source->rate)) {
                    error = context + " ('" + clip.id +
                            "') has a speed other than 1/1 and a source_in "
                            "that does not fall on a source frame";
                    return false;
                }
                // R4 -- a freeze holds one picture: on a video track, and on
                // a frame the source has (its end would hold nothing).
                if (ClipIsFreeze(clip) && track.kind != "video") {
                    error = context + " ('" + clip.id +
                            "') holds a frame (speed 0/1) on a track that is "
                            "not a video track";
                    return false;
                }
                if (source != nullptr && ClipIsFreeze(clip) &&
                    !(clip.source_in < source->duration)) {
                    error = context + " ('" + clip.id +
                            "') holds a frame past its source's end";
                    return false;
                }
                // Criterion 4 -- every frame of a ramp has a source time
                // the engine can write (the operations store it, the sheets
                // seek to it), and the last one shows a frame the source
                // has: a ramp that ends on a stop reaches its end exactly,
                // where a constant speed stops one frame short of it.
                if (source != nullptr && ClipHasSpeedRamp(clip)) {
                    int64_t frames = 0;
                    std::string framesError;
                    if (!ClipFrameCount(clip, sequence.frame_rate, frames,
                                        framesError)) {
                        error = context + " ('" + clip.id + "') " + framesError;
                        return false;
                    }
                    for (int64_t frame = 0; frame < frames; ++frame) {
                        ClipSourceTimeAt(
                            clip, RationalTime{frame * sequence.frame_rate.den,
                                               sequence.frame_rate.num});
                    }
                    const int64_t last = ClipSourceFrameAt(
                        clip,
                        RationalTime{(frames - 1) * sequence.frame_rate.den,
                                     sequence.frame_rate.num},
                        source->rate);
                    if (frames > 0 &&
                        !(RationalTime{last * source->rate.den,
                                       source->rate.num} < source->duration)) {
                        error = context + " ('" + clip.id +
                                "') ramps past its source's last frame";
                        return false;
                    }
                }
                if (ClipHasSpeed(clip) && (captionClip || motionClip)) {
                    error = context + " ('" + clip.id +
                            "') is a caption or motion clip and cannot carry "
                            "a speed";
                    return false;
                }
                if (previous) {
                    if (clip.timeline_in < previous->timeline_in) {
                        error = trackContext +
                                " clips are not sorted by timeline_in at '" +
                                clip.id + "'";
                        return false;
                    }
                    if (clip.timeline_in <
                        previous->timeline_in.add(previous->duration)) {
                        error = trackContext + " clips overlap at '" + clip.id +
                                "'";
                        return false;
                    }
                }
            } catch (const std::exception& exception) {
                error = context + " has invalid rational time arithmetic: " +
                        exception.what();
                return false;
            }
            if (captionClip && clip.caption_text.empty()) {
                error = context + " ('" + clip.id +
                        "') caption text must not be empty";
                return false;
            }
            // PA4 -- one time per word, or none. A time is not held inside
            // the clip: a head trim leaves the words said before it behind.
            if (!clip.caption_word_times.empty()) {
                if (!captionClip || clip.caption_word_times.size() !=
                                        CaptionWordCount(clip.caption_text)) {
                    error = context + " ('" + clip.id +
                            "') caption_word_times must hold one time per "
                            "word of a carton's text";
                    return false;
                }
                for (const RationalTime& time : clip.caption_word_times) {
                    const RationalTime reduced = ReducedCaptionWordTime(time);
                    if (time.rate <= 0 || reduced.value != time.value ||
                        reduced.rate != time.rate) {
                        error = context + " ('" + clip.id +
                                "') has a caption word time that is not a "
                                "fraction in lowest terms";
                        return false;
                    }
                }
            }
            for (size_t effectIndex = 0; effectIndex < clip.effects.size();
                 ++effectIndex) {
                const ClipEffect& effect = clip.effects[effectIndex];
                const std::string effectContext =
                    context + " effect " + std::to_string(effectIndex);
                if (!RegisterId(effect.id, effectContext, ids, error))
                    return false;
                if (track.kind != "video") {
                    error = effectContext + " ('" + effect.id +
                            "') is on a non-video track";
                    return false;
                }
                if (effect.type.rfind("color.", 0) != 0 ||
                    effect.type.size() <= 6 || effect.type.size() > 128) {
                    error = effectContext + " ('" + effect.id +
                            "') has an unsupported effect type '" +
                            effect.type + "'";
                    return false;
                }
                for (const auto& param : effect.params) {
                    if (param.first.empty() || param.first.size() > 64) {
                        error = effectContext + " ('" + effect.id +
                                "') has an invalid parameter name";
                        return false;
                    }
                    if (param.second.den <= 0) {
                        error = effectContext + " ('" + effect.id +
                                "') parameter '" + param.first +
                                "' has a non-positive denominator";
                        return false;
                    }
                }
                // F1.3 -- wheels and curves have named, bounded knobs.
                std::string effectError;
                if (!ValidateColorEffect(effect, effectError)) {
                    error.assign(effectContext)
                        .append(" ('")
                        .append(effect.id)
                        .append("'): ")
                        .append(effectError);
                    return false;
                }
            }
            if (!clip.caption_group_id.empty()) {
                if (!IsValidUlid(clip.caption_group_id)) {
                    error = context + " ('" + clip.id +
                            "') has invalid caption_group_id '" +
                            clip.caption_group_id + "'";
                    return false;
                }
                if (!FindCaptionStyle(clip.caption_group_id)) {
                    error = context + " ('" + clip.id +
                            "') references unknown caption_group_id '" +
                            clip.caption_group_id + "'";
                    return false;
                }
            }
            previous = &clip;
        }
    }
    for (size_t index = 0; index < sequence.transitions.size(); ++index) {
        const DocumentTransition& transition = sequence.transitions[index];
        const std::string context = "transition " + std::to_string(index);
        if (!RegisterId(transition.id, context, ids, error)) return false;
        const DocumentTrack* track = FindTrack(transition.track_id);
        // EF2 -- docs/roadmap/EFFETS-2026-10.md. A cross fade is the
        // sound's, the dissolve and the dip the picture's.
        const char* trackKind = TransitionTrackKind(transition.type);
        if (!track || track->kind != trackKind) {
            error =
                context + " must reference " +
                (TransitionIsCrossFade(transition) ? "an audio" : "a video") +
                " track";
            return false;
        }
        // R5 -- docs/roadmap/REEL-2026-09.md. Two types, and only two: a
        // dissolve between two shots, and a flat colour over a junction.
        // The flash is deliberately not one of them -- it is a clip, and
        // tests/motion_tests.cc shows the four frames it takes.
        const bool dip = TransitionIsColorDip(transition);
        if ((transition.type != kTransitionCrossDissolve && !dip &&
             !TransitionIsCrossFade(transition)) ||
            transition.duration.rate <= 0 || transition.duration.value <= 0) {
            error = context + " has an unsupported type or invalid duration";
            return false;
        }
        TransitionColor dipColor;
        if (!dip) {
            if (transition.color != DocumentTransition().color) {
                error =
                    context + " carries a colour but is not a 'dip_to_color'";
                return false;
            }
        } else if (!ParseTransitionColor(transition.color, dipColor, error)) {
            error = context + " " + error;
            return false;
        }
        // R5 -- a dissolve needs two pictures; a colour dip needs only the
        // one it covers, which is how a track opens on black.
        if (!dip && TransitionIsAtTrackEdge(transition)) {
            error = context + " is a " +
                    (TransitionIsCrossFade(transition) ? "cross fade"
                                                       : "cross dissolve") +
                    " with nothing on one side; only a 'dip_to_color' sits "
                    "at the edge of a track";
            return false;
        }
        if (dip && transition.left_clip_id.empty() &&
            transition.alignment != TransitionAlignment::StartAtCut) {
            error = context +
                    " opens a track and must be aligned 'start_at_cut', so "
                    "that its frames fall on the clip it covers";
            return false;
        }
        if (dip && transition.right_clip_id.empty() &&
            transition.alignment != TransitionAlignment::EndAtCut) {
            error = context +
                    " closes a track and must be aligned 'end_at_cut', so "
                    "that its frames fall on the clip it covers";
            return false;
        }
        // R5 -- where the transition falls, by the one function the
        // monitor, the export and verify_export also ask (guard rail 1).
        TransitionSpan span;
        std::string spanError;
        if (!ResolveTransitionSpan(sequence, transition, span, spanError)) {
            error = context + " " + spanError;
            return false;
        }
        size_t leftIndex = track->clips.size();
        size_t rightIndex = track->clips.size();
        for (size_t clipIndex = 0; clipIndex < track->clips.size();
             ++clipIndex) {
            if (!transition.left_clip_id.empty() &&
                track->clips[clipIndex].id == transition.left_clip_id)
                leftIndex = clipIndex;
            if (!transition.right_clip_id.empty() &&
                track->clips[clipIndex].id == transition.right_clip_id)
                rightIndex = clipIndex;
        }
        try {
            // R5 -- a colour dip lays a veil over a montage it does not
            // move: it reads no handle and extends no clip, so a shot with
            // no frame to spare -- or a retimed one -- takes one all the
            // same. Only the dissolve reads past the cut.
            if (!dip) {
                const DocumentClip& left = track->clips[leftIndex];
                const DocumentClip& right = track->clips[rightIndex];
                const RationalTime pre{
                    span.pre_frames * sequence.frame_rate.den,
                    sequence.frame_rate.num};
                const RationalTime post{
                    span.post_frames * sequence.frame_rate.den,
                    sequence.frame_rate.num};
                // MOTION-2026-09 M4 -- a motion clip's handles are the
                // frames of its composition around the clip.
                const std::optional<DocumentSource> leftSource =
                    ClipSource(*this, left);
                // Criterion 4 -- through the clip's own integral, so that a
                // ramp's handles are measured at its speed before the
                // dissolve is refused on it below.
                if (!leftSource ||
                    ClipSourceTimeAt(left, left.duration.add(post)) >
                        leftSource->duration ||
                    ClipSourceTimeAt(right, RationalTime{0, 1}.sub(pre)).value <
                        0) {
                    error = context + " exceeds available media handles";
                    return false;
                }
                // R3 -- docs/roadmap/REEL-2026-09.md. A dissolve reads both
                // shots past their cut, and Timeline reads those handles
                // from a source time it derives itself. Naming that frame
                // exactly on a shot that is not at 1/1 is refused by name
                // rather than dissolved one frame off. R5 -- a colour dip
                // reads no handle at all, so it is allowed there.
                if (ClipHasSpeed(left) || ClipHasSpeed(right)) {
                    error =
                        context +
                        " joins a clip whose speed is not 1/1; a transition "
                        "on a speed-changed clip is refused (R3)";
                    return false;
                }
            } else {
                // R5 -- the veil has to fall on the clips it names, or half
                // of it would paint whatever else the track holds.
                if (leftIndex != track->clips.size() &&
                    span.first_frame() <
                        track->clips[leftIndex].timeline_in.to_frames(
                            sequence.frame_rate.num, sequence.frame_rate.den)) {
                    error = context +
                            " reaches back past the start of the clip it "
                            "covers";
                    return false;
                }
                if (rightIndex != track->clips.size() &&
                    span.end_frame() >
                        track->clips[rightIndex]
                            .timeline_in.add(track->clips[rightIndex].duration)
                            .to_frames(sequence.frame_rate.num,
                                       sequence.frame_rate.den)) {
                    error =
                        context + " reaches past the end of the clip it covers";
                    return false;
                }
            }
            // R5 -- two transitions of one track covering the same frame
            // would each claim it, and nothing downstream could say which
            // one that frame shows. Refused where it is written rather than
            // resolved by list order.
            for (size_t other = 0; other < index; ++other) {
                const DocumentTransition& earlier = sequence.transitions[other];
                if (earlier.track_id != transition.track_id) continue;
                TransitionSpan earlierSpan;
                std::string ignored;
                if (!ResolveTransitionSpan(sequence, earlier, earlierSpan,
                                           ignored))
                    continue;
                if (span.first_frame() < earlierSpan.end_frame() &&
                    earlierSpan.first_frame() < span.end_frame()) {
                    error = context + " overlaps transition " +
                            std::to_string(other) + " ('" + earlier.id +
                            "') on the same track";
                    return false;
                }
            }
        } catch (const std::exception& exception) {
            error = context + " has invalid rational time arithmetic: " +
                    exception.what();
            return false;
        }
    }
    for (size_t index = 0; index < sequence.caption_styles.size(); ++index) {
        const CaptionStyle& style = sequence.caption_styles[index];
        const std::string context = "caption style " + std::to_string(index);
        if (!RegisterId(style.id, context, ids, error)) return false;
        std::string invalid;
        if (!ValidateCaptionStyle(style, invalid)) {
            error = context;
            error += ' ';
            error += invalid;
            return false;
        }
    }
    for (size_t groupIndex = 0; groupIndex < sequence.multicam_groups.size();
         ++groupIndex) {
        const MulticamGroup& group = sequence.multicam_groups[groupIndex];
        const std::string groupContext =
            "multicam group " + std::to_string(groupIndex);
        if (!RegisterId(group.id, groupContext, ids, error)) return false;
        if (group.name.empty()) {
            error = groupContext + " has an empty name";
            return false;
        }
        std::set<Ulid> angleIds;
        std::set<Ulid> angleClipIds;
        for (size_t angleIndex = 0; angleIndex < group.angles.size();
             ++angleIndex) {
            const MulticamAngle& angle = group.angles[angleIndex];
            const std::string angleContext =
                groupContext + " angle " + std::to_string(angleIndex);
            if (!RegisterId(angle.id, angleContext, ids, error)) return false;
            angleIds.insert(angle.id);
            if (!angleClipIds.insert(angle.clip_id).second) {
                error = angleContext + " ('" + angle.id +
                        "') references a clip_id already used by another "
                        "angle in this group";
                return false;
            }
            const DocumentTrack* angleTrack = FindTrackForClip(angle.clip_id);
            if (!angleTrack || angleTrack->kind != "video") {
                error = angleContext + " ('" + angle.id +
                        "') must reference a clip on a video track";
                return false;
            }
        }
        if (!group.active_angle_id.empty() &&
            angleIds.find(group.active_angle_id) == angleIds.end()) {
            error = groupContext + " active_angle_id '" +
                    group.active_angle_id +
                    "' does not reference one of its own angles";
            return false;
        }
    }
    // MOTION-2026-09 M3 -- a composition, its nodes and its keyframes share
    // the document's one ID space, the library's included.
    for (size_t index = 0; index < compositions.size(); ++index) {
        const DocumentComposition& composition = compositions[index];
        const std::string context = "composition " + std::to_string(index);
        for (const Ulid& id : CompositionIdentities(composition)) {
            // AlreadyChecked: valid, distinct from one another and from the
            // library and the sources, as the first pass found them; what is
            // left to meet is the sequence's own IDs. Nothing below reads
            // `ids`, so they are looked up rather than registered.
            if (compositionCheck == CompositionCheck::AlreadyChecked) {
                if (ids.count(id) != 0) {
                    error = "duplicate ID '" + id + "' at " + context;
                    return false;
                }
                continue;
            }
            if (!RegisterId(id, context, ids, error)) {
                return false;
            }
            if (libraryIds.count(id) != 0) {
                error = "duplicate ID '";
                error += id;
                error += "' at ";
                error += context;
                return false;
            }
        }
        MotionError motion;
        if (compositionCheck == CompositionCheck::Check &&
            !ValidateDocumentComposition(composition, motion)) {
            error = context + " ('" + composition.id + "'): " + motion.code +
                    (motion.path.empty() ? "" : " at " + motion.path) + ": " +
                    motion.message;
            return false;
        }
    }
    // REMOTION-PARITY-2026-09 REM2 -- what compositions name of the project:
    // media, and one another through their instances.
    if (compositionCheck == CompositionCheck::Check && !compositions.empty()) {
        std::set<Ulid> mediaIds(libraryIds.begin(), libraryIds.end());
        for (const DocumentSource& source : sources) {
            mediaIds.insert(source.id);
        }
        MotionError motion;
        if (!ValidateCompositionReferences(compositions, mediaIds, motion)) {
            error = motion.code +
                    (motion.path.empty() ? "" : " at " + motion.path) + ": " +
                    motion.message;
            return false;
        }
    }
    error.clear();
    return true;
}

const DocumentSource* Document::FindSource(const Ulid& id) const {
    for (const DocumentSource& source : sources) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

DocumentSource* Document::FindSource(const Ulid& id) {
    for (DocumentSource& source : sources) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

bool ValidateColorInputNames(const std::string& gamut,
                             const std::string& transfer,
                             const std::string& ycbcrMatrix,
                             const std::string& range, std::string& error) {
    if (!OneOf(gamut,
               {"rec709", "sony_sgamut3_cine", "sony_sgamut3", "rec2020"}) ||
        !OneOf(transfer, {"rec709", "sony_slog3", "linear", "hlg"}) ||
        !OneOf(ycbcrMatrix, {"auto", "bt709", "bt2020_ncl"}) ||
        !OneOf(range, {"auto", "full", "limited"})) {
        error = "unsupported colour input '" + gamut + "/" + transfer + "/" +
                ycbcrMatrix + "/" + range + "'";
        return false;
    }
    error.clear();
    return true;
}

bool IsSupportedColorInput(const std::string& gamut,
                           const std::string& transfer) {
    return (transfer == "sony_slog3" &&
            (gamut == "sony_sgamut3_cine" || gamut == "sony_sgamut3")) ||
           (transfer == "rec709" &&
            (gamut == "rec709" || gamut == "rec2020")) ||
           (transfer == "linear" &&
            (gamut == "rec709" || gamut == "rec2020")) ||
           (transfer == "hlg" && gamut == "rec2020");
}

ColorManagementSettings WithMediaColorInput(
    const ColorManagementSettings& project, const MediaColorInput& input) {
    if (input.graphics) {
        // DEC31 -- a graphic's code values are what it shows: no input
        // transform, read as Rec.709 video with its own matrix and range.
        ColorManagementSettings settings = project;
        settings.enabled = false;
        settings.input_gamut = "rec709";
        settings.input_transfer = "rec709";
        settings.input_ycbcr_matrix = input.ycbcr_matrix;
        settings.input_range = input.range;
        return settings;
    }
    if (!DeclaresColorInput(input)) return project;
    ColorManagementSettings settings = project;
    settings.input_gamut = input.gamut;
    settings.input_transfer = input.transfer;
    settings.input_ycbcr_matrix = input.ycbcr_matrix;
    settings.input_range = input.range;
    return settings;
}

ColorManagementSettings Document::ColorManagementFor(
    const Ulid& mediaId) const {
    const LibraryMedia* media = FindLibraryMedia(mediaId);
    return media == nullptr
               ? color_management
               : WithMediaColorInput(color_management, media->color_input);
}

bool Document::IsGraphicsMedia(const Ulid& mediaId) const {
    const LibraryMedia* media = FindLibraryMedia(mediaId);
    return media != nullptr && media->color_input.graphics;
}

const LibraryMedia* Document::FindLibraryMedia(const Ulid& id) const {
    for (const LibraryMedia& media : library) {
        if (media.id == id) return &media;
    }
    return nullptr;
}

LibraryMedia* Document::FindLibraryMedia(const Ulid& id) {
    for (LibraryMedia& media : library) {
        if (media.id == id) return &media;
    }
    return nullptr;
}

const DocumentBin* Document::FindBin(const Ulid& id) const {
    for (const DocumentBin& bin : bins) {
        if (bin.id == id) return &bin;
    }
    return nullptr;
}

DocumentBin* Document::FindBin(const Ulid& id) {
    for (DocumentBin& bin : bins) {
        if (bin.id == id) return &bin;
    }
    return nullptr;
}

const DocumentMarker* Document::FindMarker(const Ulid& id) const {
    for (const DocumentMarker& marker : sequence.markers) {
        if (marker.id == id) return &marker;
    }
    return nullptr;
}

DocumentMarker* Document::FindMarker(const Ulid& id) {
    for (DocumentMarker& marker : sequence.markers) {
        if (marker.id == id) return &marker;
    }
    return nullptr;
}

const DocumentTransition* Document::FindTransition(const Ulid& id) const {
    for (const DocumentTransition& transition : sequence.transitions) {
        if (transition.id == id) return &transition;
    }
    return nullptr;
}

DocumentTransition* Document::FindTransition(const Ulid& id) {
    for (DocumentTransition& transition : sequence.transitions) {
        if (transition.id == id) return &transition;
    }
    return nullptr;
}

const CaptionStyle* Document::FindCaptionStyle(const Ulid& id) const {
    for (const CaptionStyle& style : sequence.caption_styles) {
        if (style.id == id) return &style;
    }
    return nullptr;
}

CaptionStyle* Document::FindCaptionStyle(const Ulid& id) {
    for (CaptionStyle& style : sequence.caption_styles) {
        if (style.id == id) return &style;
    }
    return nullptr;
}

const MulticamGroup* Document::FindMulticamGroup(const Ulid& id) const {
    for (const MulticamGroup& group : sequence.multicam_groups) {
        if (group.id == id) return &group;
    }
    return nullptr;
}

MulticamGroup* Document::FindMulticamGroup(const Ulid& id) {
    for (MulticamGroup& group : sequence.multicam_groups) {
        if (group.id == id) return &group;
    }
    return nullptr;
}

const DocumentTrack* Document::FindTrack(const Ulid& id) const {
    for (const DocumentTrack& track : sequence.tracks) {
        if (track.id == id) return &track;
    }
    return nullptr;
}

DocumentTrack* Document::FindTrack(const Ulid& id) {
    for (DocumentTrack& track : sequence.tracks) {
        if (track.id == id) return &track;
    }
    return nullptr;
}

const DocumentClip* Document::FindClip(const Ulid& id) const {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == id) return &clip;
        }
    }
    return nullptr;
}

DocumentClip* Document::FindClip(const Ulid& id) {
    for (DocumentTrack& track : sequence.tracks) {
        for (DocumentClip& clip : track.clips) {
            if (clip.id == id) return &clip;
        }
    }
    return nullptr;
}

const DocumentTrack* Document::FindTrackForClip(const Ulid& clipId) const {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == clipId) return &track;
        }
    }
    return nullptr;
}

DocumentTrack* Document::FindTrackForClip(const Ulid& clipId) {
    for (DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.id == clipId) return &track;
        }
    }
    return nullptr;
}

const DocumentComposition* Document::FindComposition(const Ulid& id) const {
    for (const DocumentComposition& composition : compositions) {
        if (composition.id == id) {
            return &composition;
        }
    }
    return nullptr;
}

DocumentComposition* Document::FindComposition(const Ulid& id) {
    for (DocumentComposition& composition : compositions) {
        if (composition.id == id) {
            return &composition;
        }
    }
    return nullptr;
}

// STRUCTURE-2026-11 ST5 -- moved out of Document.h, where every file that
// includes it compiled them.

bool operator==(const MediaColorInput& left, const MediaColorInput& right) {
    return left.gamut == right.gamut && left.transfer == right.transfer &&
           left.ycbcr_matrix == right.ycbcr_matrix &&
           left.range == right.range && left.graphics == right.graphics;
}

bool operator==(const MediaGeneration& left, const MediaGeneration& right) {
    return left.service == right.service && left.model == right.model &&
           left.kind == right.kind && left.prompt == right.prompt &&
           left.seed == right.seed && left.created_at == right.created_at &&
           left.announced_cost == right.announced_cost &&
           left.request_id == right.request_id &&
           left.source_media_id == right.source_media_id &&
           left.reference_text == right.reference_text;
}

bool HoldsMotionClip(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (IsMotionClip(clip) || !clip.motion_props.IsNull()) {
                return true;
            }
        }
    }
    return false;
}

bool HoldsClipSpeed(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.speed.num != 1 || clip.speed.den != 1) {
                return true;
            }
        }
    }
    return false;
}

bool ClipHasFraming(const DocumentClip& clip) {
    const ClipFraming defaults;
    const auto same = [](const EffectParamValue& left,
                         const EffectParamValue& right) {
        return left.num == right.num && left.den == right.den;
    };
    return clip.framing.mode != defaults.mode ||
           !same(clip.framing.scale, defaults.scale) ||
           !same(clip.framing.position_x, defaults.position_x) ||
           !same(clip.framing.position_y, defaults.position_y) ||
           !same(clip.framing.crop_left, defaults.crop_left) ||
           !same(clip.framing.crop_right, defaults.crop_right) ||
           !same(clip.framing.crop_top, defaults.crop_top) ||
           !same(clip.framing.crop_bottom, defaults.crop_bottom);
}

bool HoldsClipFraming(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (ClipHasFraming(clip)) {
                return true;
            }
        }
    }
    return false;
}

bool ClipPlaysGeneratedMedia(const Document& document,
                             const DocumentClip& clip) {
    if (IsMotionClip(clip)) {
        return false;
    }
    const LibraryMedia* media = document.FindLibraryMedia(clip.source_id);
    return media != nullptr && !media->generation.service.empty();
}

bool HoldsClipAnimation(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (ClipHasAnimation(clip)) {
                return true;
            }
        }
    }
    return false;
}

bool HoldsSpeedRamp(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (!clip.speed_ramp.empty()) {
                return true;
            }
        }
    }
    return false;
}

bool HoldsColorTransition(const DocumentSequence& sequence) {
    for (const DocumentTransition& transition : sequence.transitions) {
        if (IsColorTransition(transition)) {
            return true;
        }
    }
    return false;
}

bool HoldsSequenceLut(const DocumentSequence& sequence) {
    static const SequenceLut defaults;
    return sequence.look_lut.digest != defaults.digest ||
           sequence.look_lut.name != defaults.name ||
           sequence.look_lut.space != defaults.space;
}

bool HoldsTemperature(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            for (const ClipEffect& effect : clip.effects)
                if (effect.type == "color.temperature") return true;
    return false;
}

bool HoldsMediaColorInput(const std::vector<LibraryMedia>& library) {
    static const MediaColorInput defaults;
    for (const LibraryMedia& media : library)
        if (media.color_input != defaults) return true;
    return false;
}

bool HoldsGeneratedMedia(const std::vector<LibraryMedia>& library) {
    static const MediaGeneration defaults;
    for (const LibraryMedia& media : library)
        if (media.generation != defaults) return true;
    return false;
}

bool HoldsHueStages(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            for (const ClipEffect& effect : clip.effects)
                if (effect.type == "color.hue" ||
                    effect.type == "color.hue_curves" ||
                    (effect.type == "color.contrast" &&
                     effect.params.count("pivot") != 0))
                    return true;
    return false;
}

bool HoldsResolvePrimaries(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            for (const ClipEffect& effect : clip.effects)
                if (effect.type == "color.resolve_primaries") return true;
    return false;
}

bool HoldsDisabledClip(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (!clip.enabled) {
                return true;
            }
        }
    }
    return false;
}

bool HoldsPersonMask(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            if (ClipHasPersonMask(clip)) return true;
    return false;
}

bool HoldsClipBlur(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            if (ClipHasBlur(clip)) return true;
    return false;
}

bool HoldsClipMask(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            if (ClipHasMask(clip)) return true;
    return false;
}

int32_t DocumentFormatVersion(bool hasCompositions, bool hasMotionClips,
                              bool hasClipSpeed, bool hasClipFraming,
                              bool hasClipAnimation, bool hasColorTransition,
                              bool hasSequenceLut, bool hasWhiteBalance,
                              bool hasMediaColorInput, bool hasHueStages,
                              bool hasSpeedRamp, bool hasShotDescriptions,
                              bool hasGeneratedMedia, bool hasGraphicsInput,
                              bool hasResolvePrimaries, bool hasDisabledClips,
                              bool hasTimelineEffects, bool hasVoiceReference,
                              bool hasClipMask, bool hasPersonMask) {
    // PA15 -- a mask that follows the people is the newest.
    if (hasPersonMask) {
        return kDocumentVersionWithPersonMask;
    }
    // PA8 -- then a clip's mask.
    if (hasClipMask) {
        return kDocumentVersionWithClipMask;
    }
    if (hasVoiceReference) {
        return kDocumentVersionWithVoiceReference;
    }
    if (hasTimelineEffects) {
        return kDocumentVersionWithTimelineEffects;
    }
    if (hasDisabledClips) {
        return kDocumentVersionWithDisabledClips;
    }
    if (hasResolvePrimaries) {
        return kDocumentVersionWithResolvePrimaries;
    }
    if (hasGraphicsInput) {
        return kDocumentVersionWithGraphicsInput;
    }
    if (hasGeneratedMedia) {
        return kDocumentVersionWithGeneratedMedia;
    }
    if (hasShotDescriptions) {
        return kDocumentVersionWithShotDescriptions;
    }
    if (hasSpeedRamp) {
        return kDocumentVersionWithSpeedRamp;
    }
    if (hasHueStages) {
        return kDocumentVersionWithHueStages;
    }
    if (hasMediaColorInput) {
        return kDocumentVersionWithMediaColorInput;
    }
    if (hasWhiteBalance) {
        return kDocumentVersionWithWhiteBalance;
    }
    if (hasSequenceLut) {
        return kDocumentVersionWithSequenceLut;
    }
    if (hasColorTransition) {
        return kDocumentVersionWithColorTransition;
    }
    if (hasClipAnimation) {
        return kDocumentVersionWithClipAnimation;
    }
    if (hasClipFraming) {
        return kDocumentVersionWithClipFraming;
    }
    if (hasClipSpeed) {
        return kDocumentVersionWithClipSpeed;
    }
    if (hasMotionClips) {
        return kDocumentVersionWithMotionClips;
    }
    return hasCompositions ? kDocumentVersionWithCompositions
                           : Document().version;
}
