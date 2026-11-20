#include "ClipBadges.h"

#include "ClipSpeed.h"
#include "Multicam.h"
#include "UiTheme.h"

#include <cstdint>
#include <numeric>
#include <utility>

// PRD24 -- ROADMAP.md. See ClipBadges.h for what this decides and why it is
// decided here rather than in the display list.

namespace {

// The interface speaks French (AGENTS.md). "ÉTALO" is spelled with its
// bytes so that no editor can turn the É into an E followed by a combining
// accent, which the atlas would draw as two glyphs, the second a "?".
constexpr char kMotionLabel[] = "MOTION";
constexpr char kAnimationLabel[] = "ANIM";
constexpr char kFramingLabel[] = "CADRE";
constexpr char kGradeLabel[] =
    "\xC3\x89"
    "TALO";
// PRD25 -- the agent's mark, named as the panel the person reads the
// agent's journal in.
constexpr char kAgentLabel[] = "AGENT";
// DEC36 -- a generated picture, "GÉNÉRÉ" spelled with its bytes as "ÉTALO"
// is, for the same reason: three precomposed É the atlas holds (Latin-1).
constexpr char kGeneratedLabel[] =
    "G\xC3\x89"
    "N\xC3\x89"
    "R\xC3\x89";

}  // namespace

std::string ClipSpeedBadgeLabel(const EffectParamValue& speed) {
    if (speed.num <= 0 || speed.den <= 0) {
        return {};
    }
    // In 64 bits: a hundred times the largest numerator a document may
    // carry (ClipSpeed.h) fits a 32-bit integer, a hand-written one may not.
    const int64_t divisor = std::gcd<int64_t, int64_t>(speed.num, speed.den);
    const int64_t num = speed.num / divisor;
    const int64_t den = speed.den / divisor;
    if (num == den) {
        return {};
    }
    if (den == 1) {
        return std::to_string(num) + "x";
    }
    if ((100 * num) % den == 0) {
        return std::to_string(100 * num / den) + "%";
    }
    return std::to_string(num) + "/" + std::to_string(den) + "x";
}

std::string ClipGainBadgeLabel(const EffectParamValue& gainDb) {
    if (gainDb.num == 0 || gainDb.den <= 0) {
        return {};
    }
    // Tenths of a decibel, rounded half away from zero, in 64 bits.
    const int64_t scaled = static_cast<int64_t>(gainDb.num) * 10;
    const int64_t magnitude = scaled < 0 ? -scaled : scaled;
    const int64_t den = gainDb.den;
    const int64_t tenths = ((2 * magnitude) + den) / (2 * den);
    if (tenths == 0) {
        return {};
    }
    const std::string sign = scaled < 0 ? "-" : "+";
    std::string label = sign + std::to_string(tenths / 10);
    if (tenths % 10 != 0) {
        label += "." + std::to_string(tenths % 10);
    }
    return label + " dB";
}

std::string ClipSpeedRampBadgeLabel(const DocumentClip& clip) {
    if (clip.speed_ramp.empty()) {
        return {};
    }
    EffectParamValue slowest = clip.speed_ramp.front().value;
    EffectParamValue fastest = slowest;
    const auto below = [](const EffectParamValue& left,
                          const EffectParamValue& right) {
        return static_cast<int64_t>(left.num) * right.den <
               static_cast<int64_t>(right.num) * left.den;
    };
    for (const ClipKeyframe& key : clip.speed_ramp) {
        if (below(key.value, slowest)) {
            slowest = key.value;
        }
        if (below(fastest, key.value)) {
            fastest = key.value;
        }
    }
    // The bounds as ClipSpeedBadgeLabel spells a speed, 1/1 included: the
    // unit speed that label leaves out is half of a ramp's story.
    const auto spell = [](const EffectParamValue& speed) {
        if (speed.num == 0) {
            return std::string("0%");
        }
        const std::string label = ClipSpeedBadgeLabel(speed);
        return label.empty() ? std::string("100%") : label;
    };
    return spell(slowest) + "-" + spell(fastest);
}

std::string ClipBadgeLabel(ClipBadgeKind kind) {
    switch (kind) {
        case ClipBadgeKind::Agent:
            return kAgentLabel;
        case ClipBadgeKind::Generated:
            return kGeneratedLabel;
        case ClipBadgeKind::Speed:
        case ClipBadgeKind::Gain:
            return {};
        case ClipBadgeKind::Motion:
            return kMotionLabel;
        case ClipBadgeKind::Animation:
            return kAnimationLabel;
        case ClipBadgeKind::Framing:
            return kFramingLabel;
        case ClipBadgeKind::Grade:
            return kGradeLabel;
    }
    return {};
}

ui::theme::Color ClipBadgeFillColor(ClipBadgeKind kind) {
    return kind == ClipBadgeKind::Agent ? ui::theme::kAgentMark
                                        : ui::theme::kClipBadgeFill;
}

ui::theme::Color ClipBadgeTextColor(ClipBadgeKind kind) {
    return kind == ClipBadgeKind::Agent ? ui::theme::kTextOnAccent
                                        : ui::theme::kClipBadgeText;
}

ClipSignage ClipSignageFor(const Document& document, const DocumentClip& clip,
                           bool agentMarked) {
    ClipSignage signage;
    const auto carry = [&](ClipBadgeKind kind) {
        signage.badges.push_back({kind, ClipBadgeLabel(kind)});
    };
    if (agentMarked) {
        carry(ClipBadgeKind::Agent);
    }
    if (ClipPlaysGeneratedMedia(document, clip)) {
        carry(ClipBadgeKind::Generated);
    }
    if (ClipHasSpeed(clip)) {
        std::string label = ClipHasSpeedRamp(clip)
                                ? ClipSpeedRampBadgeLabel(clip)
                                : ClipSpeedBadgeLabel(clip.speed);
        if (!label.empty()) {
            signage.badges.push_back({ClipBadgeKind::Speed, std::move(label)});
        }
    }
    if (std::string label = ClipGainBadgeLabel(clip.audio_gain_db);
        !label.empty()) {
        signage.badges.push_back({ClipBadgeKind::Gain, std::move(label)});
    }
    if (IsMotionClip(clip)) {
        carry(ClipBadgeKind::Motion);
    }
    if (ClipHasAnimation(clip)) {
        carry(ClipBadgeKind::Animation);
    }
    if (ClipHasFraming(clip)) {
        carry(ClipBadgeKind::Framing);
    }
    if (!clip.effects.empty()) {
        carry(ClipBadgeKind::Grade);
    }
    signage.hidden_angle = MulticamHidesClip(document.sequence, clip.id);
    signage.disabled = !clip.enabled;
    return signage;
}

int ClipBadgeGlyphCount(const std::string& label) {
    int glyphs = 0;
    // A UTF-8 continuation byte is 10xxxxxx; every other byte starts a code
    // point.
    for (const char byte : label) {
        if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) {
            ++glyphs;
        }
    }
    return glyphs;
}

double ClipBadgeWidth(const std::string& label) {
    return ClipBadgeGlyphCount(label) *
               ui::theme::kTimelineClipBadgeGlyphAdvance +
           2.0 * ui::theme::kTimelineClipBadgePadding;
}

PlacedClipBadge PlaceClipPlate(const std::string& label, double x) {
    PlacedClipBadge plate;
    plate.label = label;
    plate.x = x;
    plate.y = ui::theme::kTimelineClipBadgeTop;
    plate.width = ClipBadgeWidth(label);
    plate.height = ui::theme::kTimelineClipBadgeHeight;
    plate.text_x = plate.x + ui::theme::kTimelineClipBadgeTextInset;
    plate.text_y = plate.y - ui::theme::kTimelineClipBadgeTextRise;
    // The atlas stops at the first glyph that would cross this width, and
    // compares in floating point: the last point of padding is handed to it
    // so that the last glyph is never the one it drops.
    plate.text_width = plate.width - ui::theme::kTimelineClipBadgeTextInset;
    return plate;
}

std::vector<PlacedClipBadge> PlaceClipBadges(
    const std::vector<ClipBadge>& badges, double clipWidth,
    double leadingWidth) {
    std::vector<PlacedClipBadge> placed;
    // Written so that a width that is not a number places nothing.
    if (!(clipWidth > 0.0)) {
        return placed;
    }
    double cursor = ui::theme::kTimelineClipBadgeInset;
    if (leadingWidth > 0.0) {
        cursor += leadingWidth + ui::theme::kTimelineClipBadgeGap;
    }
    const double limit = clipWidth - ui::theme::kTimelineClipBadgeEndInset;
    for (const ClipBadge& badge : badges) {
        const int glyphs = ClipBadgeGlyphCount(badge.label);
        if (glyphs == 0) {
            continue;
        }
        const double width = ClipBadgeWidth(badge.label);
        if (cursor + width > limit) {
            break;
        }
        PlacedClipBadge box = PlaceClipPlate(badge.label, cursor);
        box.kind = badge.kind;
        placed.push_back(std::move(box));
        cursor += width + ui::theme::kTimelineClipBadgeGap;
    }
    return placed;
}
