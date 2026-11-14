#include "ArchiveManifest.h"

#include "Json.h"

#include <CommonCrypto/CommonDigest.h>
#include <zstd.h>

#include <array>
#include <cstdio>
#include <string>
#include <utility>

// DEC44 -- docs/roadmap/DONNEES-2026-10.md. The cut, the reassembly and the
// chunk codec of the project archive. The manifest's own codec is in
// DocumentFormat.cc, beside its table.

std::string ArchiveDigest(std::string_view bytes) {
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (const unsigned char byte : digest) {
        text.push_back(kHex[byte >> 4]);
        text.push_back(kHex[byte & 0x0f]);
    }
    return text;
}

bool IsArchiveDigest(std::string_view digest) {
    return digest.size() == 64 &&
           digest.find_first_not_of("0123456789abcdef") ==
               std::string_view::npos;
}

namespace {

// A structural reader of JSON text, only as far as the cut needs: where a
// value ends. It does not validate -- the archive stores documents the
// engine wrote and SQLite's json_valid accepted -- and the cut is checked
// by putting the document back together, so a misread can only cost the
// deduplication of one capture, never a byte of it.
class Cutter {
public:
    explicit Cutter(const std::string& text) : text_(text) {}

    bool Cut(std::vector<std::pair<size_t, size_t>>& ranges,
             std::vector<std::string>& kinds) {
        SkipSpace();
        if (!Expect('{')) return false;
        while (true) {
            SkipSpace();
            if (Peek() == '}') return true;
            const size_t keyBegin = position_;
            if (!SkipString()) return false;
            const std::string_view key(text_.data() + keyBegin,
                                       position_ - keyBegin);
            SkipSpace();
            if (!Expect(':')) return false;
            SkipSpace();
            const char* kind = key == "\"timelines\"" ? kArchiveTimelinePart
                               : key == "\"compositions\""
                                   ? kArchiveCompositionPart
                                   : nullptr;
            if (kind != nullptr && Peek() == '[') {
                ++position_;
                SkipSpace();
                if (Peek() != ']') {
                    while (true) {
                        const size_t begin = position_;
                        if (!SkipValue()) return false;
                        ranges.emplace_back(begin, position_);
                        kinds.emplace_back(kind);
                        SkipSpace();
                        if (Peek() == ',') {
                            ++position_;
                            SkipSpace();
                            continue;
                        }
                        break;
                    }
                }
                if (!Expect(']')) return false;
            } else if (!SkipValue()) {
                return false;
            }
            SkipSpace();
            if (Peek() == ',') {
                ++position_;
                continue;
            }
            return Expect('}');
        }
    }

    // The string value of the top-level "id" key of the object at `begin`,
    // or empty.
    static std::string ObjectId(const std::string& text, size_t begin,
                                size_t end) {
        const std::string object = text.substr(begin, end - begin);
        Cutter cutter(object);
        if (!cutter.Expect('{')) return std::string();
        while (true) {
            cutter.SkipSpace();
            if (cutter.Peek() == '}') return std::string();
            const size_t keyBegin = cutter.position_;
            if (!cutter.SkipString()) return std::string();
            const bool isId =
                std::string_view(object.data() + keyBegin,
                                 cutter.position_ - keyBegin) == "\"id\"";
            cutter.SkipSpace();
            if (!cutter.Expect(':')) return std::string();
            cutter.SkipSpace();
            const size_t valueBegin = cutter.position_;
            if (!cutter.SkipValue()) return std::string();
            if (isId) {
                mcp_json::Value value;
                std::string error;
                if (mcp_json::Value::Parse(
                        object.substr(valueBegin,
                                      cutter.position_ - valueBegin),
                        value, error) &&
                    value.IsString()) {
                    return value.AsString();
                }
                return std::string();
            }
            cutter.SkipSpace();
            if (cutter.Peek() != ',') return std::string();
            ++cutter.position_;
        }
    }

private:
    char Peek() const {
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    bool Expect(char expected) {
        if (Peek() != expected) return false;
        ++position_;
        return true;
    }

    void SkipSpace() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\r' || text_[position_] == '\n')) {
            ++position_;
        }
    }

    bool SkipString() {
        if (!Expect('"')) return false;
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character == '\\') {
                position_ += 2;
                continue;
            }
            ++position_;
            if (character == '"') return true;
        }
        return false;
    }

    // Iterative, so that a deeply nested motion tree cannot exhaust the
    // stack.
    bool SkipValue() {
        const char first = Peek();
        if (first == '"') return SkipString();
        if (first == '{' || first == '[') {
            size_t depth = 0;
            while (position_ < text_.size()) {
                const char character = text_[position_];
                if (character == '"') {
                    if (!SkipString()) return false;
                    continue;
                }
                ++position_;
                if (character == '{' || character == '[') {
                    ++depth;
                } else if (character == '}' || character == ']') {
                    if (--depth == 0) return true;
                }
            }
            return false;
        }
        const size_t begin = position_;
        while (position_ < text_.size()) {
            const char character = text_[position_];
            if (character == ',' || character == '}' || character == ']' ||
                character == ' ' || character == '\t' || character == '\r' ||
                character == '\n') {
                break;
            }
            ++position_;
        }
        return position_ > begin;
    }

    const std::string& text_;
    size_t position_ = 0;
};

ArchiveSplit Whole(const std::string& document) {
    ArchiveSplit split;
    split.rest = document;
    split.manifest.bytes = static_cast<int64_t>(document.size());
    split.manifest.rest = ArchiveDigest(document);
    return split;
}

}  // namespace

ArchiveSplit SplitArchiveDocument(const std::string& document) {
    std::vector<std::pair<size_t, size_t>> ranges;
    std::vector<std::string> kinds;
    Cutter cutter(document);
    if (!cutter.Cut(ranges, kinds) || ranges.empty()) {
        return Whole(document);
    }
    ArchiveSplit split;
    split.manifest.bytes = static_cast<int64_t>(document.size());
    size_t previous = 0;
    for (size_t index = 0; index < ranges.size(); ++index) {
        const auto [begin, end] = ranges[index];
        split.rest.append(document, previous, begin - previous);
        std::string bytes = document.substr(begin, end - begin);
        ArchivePart part;
        part.kind = kinds[index];
        part.id = document[begin] == '{'
                      ? Cutter::ObjectId(document, begin, end)
                      : std::string();
        part.at = static_cast<int64_t>(begin);
        part.digest = ArchiveDigest(bytes);
        split.manifest.parts.push_back(std::move(part));
        split.parts.push_back(std::move(bytes));
        previous = end;
    }
    split.rest.append(document, previous, std::string::npos);
    split.manifest.rest = ArchiveDigest(split.rest);
    // The cut is proved, not assumed: what goes back must be the document.
    std::vector<std::string_view> views(split.parts.begin(), split.parts.end());
    std::string assembled;
    std::string error;
    if (!AssembleArchiveDocument(split.manifest, split.rest, views, assembled,
                                 error) ||
        assembled != document) {
        return Whole(document);
    }
    return split;
}

bool AssembleArchiveDocument(const ArchiveManifest& manifest,
                             std::string_view rest,
                             const std::vector<std::string_view>& parts,
                             std::string& document, std::string& error) {
    if (parts.size() != manifest.parts.size() || manifest.bytes < 0) {
        error = "archive manifest and chunks disagree";
        return false;
    }
    std::string output;
    output.reserve(static_cast<size_t>(manifest.bytes));
    size_t taken = 0;
    for (size_t index = 0; index < parts.size(); ++index) {
        const int64_t at = manifest.parts[index].at;
        if (at < static_cast<int64_t>(output.size())) {
            error = "archive part " + std::to_string(index) +
                    " begins before the previous one ends";
            return false;
        }
        const size_t gap = static_cast<size_t>(at) - output.size();
        if (gap > rest.size() - taken) {
            error = "archive part " + std::to_string(index) +
                    " begins past the rest of the document";
            return false;
        }
        output.append(rest.substr(taken, gap));
        taken += gap;
        output.append(parts[index]);
    }
    output.append(rest.substr(taken));
    if (static_cast<int64_t>(output.size()) != manifest.bytes) {
        error = "archive document is " + std::to_string(output.size()) +
                " bytes, its manifest says " + std::to_string(manifest.bytes);
        return false;
    }
    document = std::move(output);
    return true;
}

bool CompressArchiveChunk(std::string_view bytes, std::string& frame,
                          std::string& error) {
    if (bytes.size() > kMaxArchiveChunkBytes) {
        error = "archive chunk exceeds 64 MiB";
        return false;
    }
    std::string output(ZSTD_compressBound(bytes.size()), '\0');
    const size_t written =
        ZSTD_compress(output.data(), output.size(), bytes.data(), bytes.size(),
                      kArchiveCompressionLevel);
    if (ZSTD_isError(written)) {
        error = std::string("zstd compression failed: ") +
                ZSTD_getErrorName(written);
        return false;
    }
    output.resize(written);
    frame = std::move(output);
    return true;
}

bool DecompressArchiveChunk(std::string_view frame, size_t expectedBytes,
                            std::string& bytes, std::string& error) {
    if (expectedBytes > kMaxArchiveChunkBytes) {
        error = "archive chunk exceeds 64 MiB";
        return false;
    }
    const size_t consumed =
        ZSTD_findFrameCompressedSize(frame.data(), frame.size());
    if (ZSTD_isError(consumed) || consumed != frame.size()) {
        error = "archive chunk is not one complete zstd frame";
        return false;
    }
    const unsigned long long declared =
        ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (declared != expectedBytes) {
        error = "archive chunk does not declare its recorded size";
        return false;
    }
    std::string output(expectedBytes, '\0');
    const size_t written = ZSTD_decompress(output.data(), output.size(),
                                           frame.data(), frame.size());
    if (ZSTD_isError(written) || written != expectedBytes) {
        error =
            std::string("archive chunk cannot be decompressed: ") +
            (ZSTD_isError(written) ? ZSTD_getErrorName(written) : "wrong size");
        return false;
    }
    bytes = std::move(output);
    return true;
}
