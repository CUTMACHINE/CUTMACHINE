#include "ReadAhead.h"

#include <algorithm>

namespace read_ahead {

Window DecodedWindow(size_t budgetBytes, size_t frameBytes, size_t sources) {
    if (frameBytes == 0) return {};
    const size_t sharing = std::max<size_t>(1, sources);
    const __int128 numerator = static_cast<__int128>(budgetBytes) * 70;
    const __int128 denominator =
        static_cast<__int128>(frameBytes) * 100 * sharing;
    const size_t total = std::max<size_t>(
        1, static_cast<size_t>((numerator + denominator - 1) / denominator));
    if (total == 1) return {};
    const size_t ahead = std::min<size_t>(total - 1, (total * 3 + 2) / 4);
    return Window{ahead, total - 1 - ahead, total};
}

int64_t AheadLimit(int64_t frame, int direction, size_t ahead,
                   const std::optional<SourceSpan>& span, int64_t frameCount) {
    const int64_t lastFrame = std::max<int64_t>(0, frameCount - 1);
    const int64_t count = static_cast<int64_t>(ahead);
    if (direction >= 0) {
        int64_t limit = std::min<int64_t>(lastFrame, frame + count);
        if (span) limit = std::min(limit, span->last);
        return std::max(limit, frame);
    }
    int64_t limit = std::max<int64_t>(0, frame - count);
    if (span) limit = std::max(limit, span->first);
    return std::min(limit, frame);
}

FrameRange BehindFill(int64_t frame, int direction, size_t behind, bool playing,
                      int64_t frameCount) {
    if (playing || behind == 0 || frameCount <= 0) return {};
    const int64_t count = static_cast<int64_t>(behind);
    const int64_t lastFrame = frameCount - 1;
    if (direction >= 0)
        return {std::max<int64_t>(0, frame - count),
                std::min<int64_t>(lastFrame, frame - 1)};
    return {std::max<int64_t>(0, frame + 1),
            std::min<int64_t>(lastFrame, frame + count)};
}

FrameRange FollowOnFill(int64_t frame, size_t ahead,
                        const std::optional<SourceSpan>& span,
                        std::optional<int64_t> next, int64_t frameCount) {
    if (!span || !next || frameCount <= 0 || frame < span->first ||
        frame > span->last || *next < 0 || *next >= frameCount)
        return {};
    const int64_t remaining =
        static_cast<int64_t>(ahead) - (span->last - frame);
    if (remaining <= 0) return {};
    return {*next, std::min<int64_t>(frameCount - 1, *next + remaining - 1)};
}

Advice NextByteAdvice(int64_t frame, int direction,
                      const std::optional<SourceSpan>& span,
                      const Advice& advised, int64_t horizon, int64_t chunk,
                      int64_t frameCount) {
    if (horizon <= 0 || frameCount <= 0 || frame < 0 || frame >= frameCount)
        return {};
    if (span && (frame < span->first || frame > span->last)) return {};
    const int64_t reach = std::max<int64_t>(0, horizon - 1);
    if (direction >= 0) {
        int64_t end = std::min<int64_t>(frameCount - 1, frame + reach);
        bool bounded = end < frame + reach;
        if (span && span->last <= end) {
            end = span->last;
            bounded = true;
        }
        const bool following = !advised.Empty() && advised.first <= frame &&
                               frame <= advised.last + 1;
        const int64_t start = following ? advised.last + 1 : frame;
        if (start > end) return {};
        if (following && end - start + 1 < std::max<int64_t>(1, chunk) &&
            !bounded)
            return {};
        return {start, end};
    }
    int64_t end = std::max<int64_t>(0, frame - reach);
    bool bounded = end > frame - reach;
    if (span && span->first >= end) {
        end = span->first;
        bounded = true;
    }
    const bool following =
        !advised.Empty() && advised.first - 1 <= frame && frame <= advised.last;
    const int64_t start = following ? advised.first - 1 : frame;
    if (start < end) return {};
    if (following && start - end + 1 < std::max<int64_t>(1, chunk) && !bounded)
        return {};
    return {end, start};
}

Advice Merge(const Advice& advised, const Advice& next) {
    if (next.Empty()) return advised;
    if (advised.Empty() || next.first > advised.last + 1 ||
        next.last < advised.first - 1)
        return next;
    return {std::min(advised.first, next.first),
            std::max(advised.last, next.last)};
}

}  // namespace read_ahead
