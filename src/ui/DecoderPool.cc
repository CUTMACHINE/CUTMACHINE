#include "DecoderPool.h"

#include "Document.h"
#include "Multicam.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace decoder_pool {

namespace {

bool IsZero(const RationalTime& time) { return time.value == 0; }

}  // namespace

std::vector<Need> UpcomingNeeds(const Document& document, RationalTime position,
                                int direction, RationalTime lookahead) {
    std::vector<Need> needs;
    if (position.rate <= 0 || lookahead.rate <= 0 || lookahead.value < 0) {
        return needs;
    }
    const int64_t speed = direction == 0 ? 1 : std::abs(direction);
    const bool forward = direction >= 0;
    const RationalTime horizon{lookahead.value * speed, lookahead.rate};
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind == "video" && track.visible) {
            tracks.push_back(&track);
        }
    }
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    const auto consider = [&](const DocumentClip& clip, RationalTime distance) {
        // A motion clip's videos are read by MotionMedia, not by a decoder
        // of the pool; a hidden angle is not decoded at all (Multicam.h).
        if (IsMotionClip(clip) ||
            MulticamHidesClip(document.sequence, clip.id)) {
            return;
        }
        needs.push_back({clip.source_id, distance});
    };
    for (const DocumentTrack* track : tracks) {
        const std::vector<DocumentClip>& clips = track->clips;
        // Clips are ordered and never overlap (Document::Validate): the one
        // before the first clip starting after `position` is the only one
        // that can contain it.
        const auto after = std::upper_bound(
            clips.begin(), clips.end(), position,
            [](const RationalTime& value, const DocumentClip& clip) {
                return value < clip.timeline_in;
            });
        bool containing = false;
        if (after != clips.begin()) {
            const DocumentClip& clip = *std::prev(after);
            if (clip.timeline_in.add(clip.duration) > position) {
                containing = true;
                consider(clip, RationalTime{0, 1});
            }
        }
        if (forward) {
            for (auto clip = after; clip != clips.end(); ++clip) {
                const RationalTime distance = clip->timeline_in.sub(position);
                if (distance > horizon) {
                    break;
                }
                consider(*clip, distance);
            }
            continue;
        }
        // Played backward, a clip is entered by its end.
        auto clip = std::make_reverse_iterator(after);
        if (containing) {
            ++clip;
        }
        for (; clip != clips.rend(); ++clip) {
            const RationalTime distance =
                position.sub(clip->timeline_in.add(clip->duration));
            if (distance > horizon) {
                break;
            }
            consider(*clip, distance);
        }
    }
    // Soonest first, equal distances in track order; each source once, at
    // its soonest.
    std::stable_sort(needs.begin(), needs.end(),
                     [](const Need& left, const Need& right) {
                         return left.distance < right.distance;
                     });
    std::set<Ulid> seen;
    std::vector<Need> unique;
    unique.reserve(needs.size());
    for (Need& need : needs) {
        if (seen.insert(need.source).second) {
            unique.push_back(std::move(need));
        }
    }
    return unique;
}

Plan PlanOpens(const std::vector<Need>& needs, const PoolState& state,
               size_t capacity, size_t maxOpening) {
    Plan plan;
    std::set<Ulid> kept;
    for (const Need& need : needs) {
        if (state.offline.count(need.source) != 0 ||
            kept.count(need.source) != 0) {
            continue;
        }
        // What is on screen is held whatever the capacity, as Usage never
        // closes it; what comes next fills the rest, soonest first.
        if (!IsZero(need.distance) && plan.keep.size() >= capacity) {
            continue;
        }
        kept.insert(need.source);
        plan.keep.push_back(need.source);
    }
    for (const Ulid& source : plan.keep) {
        if (state.open.count(source) == 0 && state.opening.count(source) == 0 &&
            state.closing.count(source) == 0) {
            plan.open.push_back(source);
        }
    }
    size_t running = 0;
    for (const auto& opening : state.opening) {
        if (opening.second < kStaleOpen) {
            ++running;
        }
    }
    if (running < maxOpening && state.opening.size() < kMaxOpeningInFlight) {
        plan.budget = std::min(maxOpening - running,
                               kMaxOpeningInFlight - state.opening.size());
    }
    return plan;
}

}  // namespace decoder_pool
