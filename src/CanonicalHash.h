#pragma once

// The `doc_hash` every edit result reports (FNV-1a, 64 bits, over the
// canonical document text), so that every surface names the same state the
// same way.
//
// PL1 -- ROADMAP.md. Moved out of Cli.cc once the journal needed it too: an
// intervention mark records the state it starts from by this hash, and
// folding the intervention checks that one undo leads back to it. It tells
// states apart; it does not authenticate them.

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

inline std::string CanonicalHash(const std::string& json) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : json) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}
