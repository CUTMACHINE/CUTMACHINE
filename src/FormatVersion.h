#pragma once

// PRD20 -- ROADMAP.md. A project written by a more recent CUTMACHINE was
// refused as "unsupported document version 18": true, and no help. It did
// not say that the build is the thing out of date, nor how far this build
// reads. A project carries three versions -- its package's manifest, its
// project file, its documents -- and each reader refuses a newer one with
// this one sentence, so the three read alike.

#include <cstdint>
#include <string>

// "document version 18 is newer than this build reads (up to 17): the
// project was written by a more recent CUTMACHINE". `what` names the format:
// "document", "project", "project package".
inline std::string NewerFormatRefusal(const std::string& what, int64_t found,
                                      int64_t newest) {
    return what + " version " + std::to_string(found) +
           " is newer than this build reads (up to " + std::to_string(newest) +
           "): the project was written by a more recent CUTMACHINE";
}
