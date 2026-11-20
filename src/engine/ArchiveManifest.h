#pragma once

// DEC44 -- docs/roadmap/DONNEES-2026-10.md. "Git in SQLite": how a capture
// of the project archive is cut into chunks stored once each.
//
// Schema 2 kept every capture as the whole canonical project JSON. A save
// changes one timeline in general, yet each capture carried all of them:
// 3 222 captures weighed 1 316 MB on 24 October 2026, for 197 MB of
// distinct content. A capture is now a manifest -- the SHA-256 digests of
// its chunks and where each goes back -- and each chunk, one timeline, one
// composition or the rest of the document, is stored once under its digest
// and compressed (ProjectArchive.cc).
//
// The cut is made in the canonical bytes themselves, never by reserializing
// a parsed project: a capture written by an older serializer keeps its own
// bytes, and putting the chunks back together gives exactly the document
// that was captured. SplitArchiveDocument checks that before answering, and
// keeps the document whole (one chunk, no parts) when it cannot cut it.
//
// The manifest is a canonical JSON object whose format is described once,
// by DocumentTable<ArchiveManifest> in DocumentFormat.h: a SQLite client
// reads it with json_each, without the engine.
//
// Plain C++: SHA-256 from CommonCrypto (OpenSSL in the headless build, as
// for LookLutStorage.cc), and the pinned zstd (cmake/CutmachineZstd.cmake).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

inline constexpr const char* kArchiveManifestFormat =
    "cutmachine.archive-manifest.v1";

// The two kinds of part: elements of the document's top-level "timelines"
// and "compositions" arrays.
inline constexpr const char* kArchiveTimelinePart = "timeline";
inline constexpr const char* kArchiveCompositionPart = "composition";

// One chunk cut out of the document.
struct ArchivePart {
    // kArchiveTimelinePart or kArchiveCompositionPart.
    std::string kind;
    // The element's own top-level "id", as a string; empty when it has none.
    // Informative: the digest is the address.
    std::string id;
    // Byte offset, in the whole document, where the part begins.
    int64_t at = 0;
    // SHA-256 of the part's bytes, 64 lowercase hexadecimal digits.
    std::string digest;
};

struct ArchiveManifest {
    std::string format = kArchiveManifestFormat;
    // Length of the whole document, in bytes.
    int64_t bytes = 0;
    // SHA-256 of the document with every part cut out.
    std::string rest;
    // In document order.
    std::vector<ArchivePart> parts;
};

// A document cut into its manifest and the chunk bytes it names.
struct ArchiveSplit {
    ArchiveManifest manifest;
    std::string rest;
    // Parallel to manifest.parts.
    std::vector<std::string> parts;
};

// Lowercase hexadecimal SHA-256 of `bytes`.
std::string ArchiveDigest(std::string_view bytes);

// True for 64 lowercase hexadecimal digits.
bool IsArchiveDigest(std::string_view digest);

// Cuts every element of the top-level "timelines" and "compositions" arrays
// out of `document`. Never fails: a document it cannot cut, or whose chunks
// would not give it back byte for byte, is kept whole as the rest.
ArchiveSplit SplitArchiveDocument(const std::string& document);

// The document a manifest names, from the rest and the parts' bytes in
// manifest order. Refuses offsets that do not fit and a result whose length
// is not manifest.bytes. Digests are the caller's to check.
bool AssembleArchiveDocument(const ArchiveManifest& manifest,
                             std::string_view rest,
                             const std::vector<std::string_view>& parts,
                             std::string& document, std::string& error);

// Canonical JSON, written and read by DocumentTable<ArchiveManifest>
// (DocumentFormat.cc). The reader is strict -- every key, no other -- and
// refuses a manifest no writer produces: another format, a digest that is
// not one, an unknown kind, a negative size or offsets out of order.
std::string SerializeArchiveManifest(const ArchiveManifest& manifest);
bool ParseArchiveManifest(const std::string& json, ArchiveManifest& manifest,
                          std::string& error);

// The archive's chunk codec: one zstd frame, which records its content
// size. kArchiveCodec names it in the database, so that another codec can
// be told apart later without guessing.
inline constexpr const char* kArchiveCodec = "zstd";
// Level 9: on the 3 349 distinct chunks of 24 October 2026 (197 MB), 9.0 MB
// in 1.5 s, against 10.2 MB at level 3 and 8.4 MB at level 19 in 46 s.
inline constexpr int kArchiveCompressionLevel = 9;
// A chunk is never larger than a capture (ProjectArchive.cc's 64 MiB).
inline constexpr size_t kMaxArchiveChunkBytes = size_t{64} * 1024 * 1024;

bool CompressArchiveChunk(std::string_view bytes, std::string& frame,
                          std::string& error);
// Refuses a frame that is not one complete zstd frame, or whose content is
// not exactly `expectedBytes` long.
bool DecompressArchiveChunk(std::string_view frame, size_t expectedBytes,
                            std::string& bytes, std::string& error);
