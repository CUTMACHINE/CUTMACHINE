#include "InspectorSound.h"

#include "ClipBadges.h"
#include "TimelineView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

// GitHub #330 -- see InspectorSound.h for what this decides and why.

namespace ui::inspector {
namespace {

// Lower case, without spaces: " -4,5 dB " reads "-4,5db".
std::string Compact(const std::string& typed) {
    std::string compact;
    for (const char character : typed) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) != 0) continue;
        compact.push_back(static_cast<char>(std::tolower(byte)));
    }
    return compact;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

bool AllDigits(const std::string& text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](char character) {
               return std::isdigit(static_cast<unsigned char>(character)) != 0;
           });
}

// An unsigned decimal, a point or a comma before its decimals: its digits
// as an integer and the power of ten under them ("4,25" is 425 and 100).
// At most nine digits either side, so that nothing below overflows.
bool ParseDecimal(const std::string& text, int64_t& mantissa, int64_t& scale) {
    const size_t separator = text.find_first_of(".,");
    const std::string whole =
        separator == std::string::npos ? text : text.substr(0, separator);
    const std::string decimals =
        separator == std::string::npos ? "" : text.substr(separator + 1);
    if (whole.empty() && decimals.empty()) return false;
    if ((!whole.empty() && !AllDigits(whole)) ||
        (separator != std::string::npos && !AllDigits(decimals)) ||
        whole.size() > 9 || decimals.size() > 9)
        return false;
    mantissa = 0;
    scale = 1;
    for (const char digit : whole) mantissa = (mantissa * 10) + (digit - '0');
    for (const char digit : decimals) {
        mantissa = (mantissa * 10) + (digit - '0');
        scale *= 10;
    }
    return true;
}

// The nominal frames a second the trim counter counts in (TrimReadoutText).
int64_t NominalFps(const MediaRate& rate) {
    return rate.num > 0 && rate.den > 0
               ? std::max<int64_t>(
                     1, std::llround(static_cast<double>(rate.num) / rate.den))
               : 25;
}

const char* FadeName(bool fadeIn) {
    return fadeIn ? "le fondu d’entrée" : "le fondu de sortie";
}

}  // namespace

std::optional<SoundTarget> InspectorSoundTarget(const Document& document,
                                                const Ulid& anchorClipId) {
    const DocumentClip* anchor =
        anchorClipId.empty() ? nullptr : document.FindClip(anchorClipId);
    const DocumentTrack* anchorTrack =
        anchor != nullptr ? document.FindTrackForClip(anchor->id) : nullptr;
    if (anchor == nullptr || anchorTrack == nullptr) return std::nullopt;
    if (anchorTrack->kind == "audio")
        return SoundTarget{anchor->id, anchorTrack->id, false,
                           anchorTrack->locked};
    if (anchorTrack->kind != "video" || anchor->link_group_id.empty())
        return std::nullopt;
    const RationalTime start = anchor->timeline_in;
    const RationalTime end = anchor->timeline_in.add(anchor->duration);
    for (const DocumentTrack* track : TimelineTracksInDisplayOrder(document)) {
        if (track->kind != "audio") continue;
        const DocumentClip* first = nullptr;
        for (const DocumentClip& clip : track->clips) {
            if (clip.link_group_id != anchor->link_group_id ||
                !(clip.timeline_in < end) ||
                !(start < clip.timeline_in.add(clip.duration)))
                continue;
            if (first == nullptr || clip.timeline_in < first->timeline_in)
                first = &clip;
        }
        if (first != nullptr)
            return SoundTarget{first->id, track->id, true, track->locked};
    }
    return std::nullopt;
}

EffectParamValue QuantizeClipGain(float sliderValue) {
    const double clamped = std::clamp(static_cast<double>(sliderValue),
                                      double{kInspectorGainMinimumDb},
                                      double{kInspectorGainMaximumDb});
    int32_t num = static_cast<int32_t>(std::lround(clamped * 10.0));
    int32_t den = 10;
    if (num == 0) return {0, 1};
    const int32_t common = std::gcd(num, den);
    return {num / common, den / common};
}

float CurrentClipGain(const EffectParamValue& gainDb) {
    return gainDb.den > 0
               ? static_cast<float>(gainDb.num) / static_cast<float>(gainDb.den)
               : 0.0f;
}

std::string ClipGainText(const EffectParamValue& gainDb) {
    const std::string label = ClipGainBadgeLabel(gainDb);
    return label.empty() ? "0 dB" : label;
}

bool ParseClipGain(const std::string& typed, EffectParamValue& gainDb,
                   std::string& refusal) {
    std::string text = Compact(typed);
    if (EndsWith(text, "db")) text.resize(text.size() - 2);
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.erase(0, 1);
    }
    int64_t mantissa = 0;
    int64_t scale = 1;
    if (!ParseDecimal(text, mantissa, scale)) {
        refusal = "« " + typed +
                  " » n’est pas un gain : un nombre de dB, comme -18 ou -4,5";
        return false;
    }
    // Tenths, half away from zero.
    const int64_t tenths =
        scale >= 10 ? ((mantissa / (scale / 10)) +
                       ((mantissa % (scale / 10)) * 2 >= scale / 10 ? 1 : 0))
                    : mantissa * (10 / scale);
    const int64_t signedTenths = negative ? -tenths : tenths;
    if (signedTenths < int64_t{kInspectorGainMinimumDb} * 10 ||
        signedTenths > int64_t{kInspectorGainMaximumDb} * 10) {
        refusal = "le gain va de " + std::to_string(kInspectorGainMinimumDb) +
                  " à +" + std::to_string(kInspectorGainMaximumDb) + " dB";
        return false;
    }
    if (signedTenths == 0) {
        gainDb = {0, 1};
        return true;
    }
    const int64_t common = std::gcd<int64_t, int64_t>(signedTenths, 10);
    gainDb = {static_cast<int32_t>(signedTenths / common),
              static_cast<int32_t>(10 / common)};
    return true;
}

int64_t FadeFramesOf(const RationalTime& time, const MediaRate& rate,
                     bool nearest) {
    if (rate.num <= 0 || rate.den <= 0 || time.rate <= 0 || time.value <= 0)
        return 0;
    const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(time.rate) * rate.den;
    const __int128 whole =
        nearest ? ((2 * numerator) + denominator) / (2 * denominator)
                : numerator / denominator;
    return static_cast<int64_t>(
        std::min<__int128>(whole, std::numeric_limits<int32_t>::max()));
}

RationalTime FadeOfFrames(int64_t frames, const MediaRate& rate) {
    if (frames <= 0 || rate.num <= 0 || rate.den <= 0) return {0, 1};
    return {frames * rate.den, rate.num};
}

std::string FadeLengthText(const RationalTime& fade, const MediaRate& rate) {
    if (fade.rate <= 0 || fade.value <= 0) return "0";
    const int64_t frames = FadeFramesOf(fade, rate, false);
    if (rate.num > 0 && rate.den > 0 && FadeOfFrames(frames, rate) == fade) {
        const int64_t fps = NominalFps(rate);
        if (frames < fps) return std::to_string(frames);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld",
                      static_cast<long long>(frames / fps),
                      static_cast<long long>(frames % fps));
        return buffer;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f s",
                  static_cast<double>(fade.value) / fade.rate);
    return buffer;
}

bool ParseFadeLength(const std::string& typed, const MediaRate& rate,
                     RationalTime& fade, std::string& refusal) {
    const auto refuse = [&]() {
        refusal = "« " + typed +
                  " » n’est pas une durée : 12 (images), 1:05 "
                  "(secondes:images) ou 0,5 s";
        return false;
    };
    if (rate.num <= 0 || rate.den <= 0) return refuse();
    std::string text = Compact(typed);
    for (const char* suffix : {"images", "image", "img", "im", "i", "f"}) {
        if (EndsWith(text, suffix) &&
            AllDigits(text.substr(0, text.size() - strlen(suffix)))) {
            text.resize(text.size() - strlen(suffix));
            break;
        }
    }
    int64_t frames = 0;
    const size_t colon = text.find(':');
    if (colon != std::string::npos) {
        const std::string seconds = text.substr(0, colon);
        const std::string rest = text.substr(colon + 1);
        const int64_t fps = NominalFps(rate);
        if (!AllDigits(seconds) || !AllDigits(rest) || seconds.size() > 6 ||
            rest.size() > 6 || std::stoll(rest) >= fps)
            return refuse();
        frames = (std::stoll(seconds) * fps) + std::stoll(rest);
    } else if (AllDigits(text)) {
        if (text.size() > 9) return refuse();
        frames = std::stoll(text);
    } else {
        // Seconds: a unit, or decimals.
        if (EndsWith(text, "s")) text.resize(text.size() - 1);
        int64_t mantissa = 0;
        int64_t scale = 1;
        if (!ParseDecimal(text, mantissa, scale)) return refuse();
        frames = FadeFramesOf(
            RationalTime{mantissa, static_cast<int32_t>(scale)}, rate, true);
    }
    fade = FadeOfFrames(frames, rate);
    return true;
}

int64_t FadeRoomFrames(const DocumentClip& clip, bool fadeIn,
                       const MediaRate& rate) {
    const RationalTime other =
        fadeIn ? clip.audio_fade_out : clip.audio_fade_in;
    try {
        return FadeFramesOf(clip.duration.sub(other), rate, false);
    } catch (const std::exception&) {
        return 0;
    }
}

std::optional<SetClipAudioOperation> WithClipFade(const DocumentClip& clip,
                                                  bool fadeIn,
                                                  const RationalTime& fade,
                                                  const MediaRate& rate,
                                                  std::string& refusal) {
    const int64_t room = FadeRoomFrames(clip, fadeIn, rate);
    if (FadeFramesOf(fade, rate, true) > room) {
        // Named as the editor sees it: the other fade when it takes part of
        // the clip, the clip alone when it does not.
        const RationalTime& other =
            fadeIn ? clip.audio_fade_out : clip.audio_fade_in;
        const std::string limit =
            other.value > 0
                ? std::string("ce que ") + FadeName(!fadeIn) + " laisse du plan"
                : std::string("la durée du plan");
        refusal = std::string(FadeName(fadeIn)) + " ne peut pas dépasser " +
                  limit + " : " +
                  FadeLengthText(FadeOfFrames(room, rate), rate) + " au plus";
        return std::nullopt;
    }
    SetClipAudioOperation operation{clip.id, clip.audio_gain_db,
                                    clip.audio_fade_in, clip.audio_fade_out};
    (fadeIn ? operation.fade_in : operation.fade_out) = fade;
    return operation;
}

SetClipAudioOperation WithClipGain(const DocumentClip& clip,
                                   const EffectParamValue& gainDb) {
    return SetClipAudioOperation{clip.id, gainDb, clip.audio_fade_in,
                                 clip.audio_fade_out};
}

}  // namespace ui::inspector
