#pragma once

// R6 -- docs/roadmap/REEL-2026-09.md. The half of the look LUT that touches
// the disk: the digest of a file, its copy into the project package, and
// the read-back the monitor and the export share.
//
// Kept out of LookLut.h, which stays portable, for the reason ProjectStorage
// is separate: SHA-256 comes from CommonCrypto here, as it does for the
// collection manifest, and a digest is what the two builds have to agree on
// byte for byte.
//
// The copy is content-addressed -- Luts/<digest>.cube inside the package,
// beside Media/ and Timelines/ -- so a package moved, copied or collected
// carries its look with it and resolves it by the same name at the new
// path. That is the ticket's "reopened from a copy of the package at
// another path, identical picture", and it is why nothing stores an
// absolute path to a .cube.

#include "Document.h"
#include "LookLut.h"

#include <string>

// SHA-256 of `bytes`, lowercase hexadecimal: the name the file takes in the
// package and the reference the document carries.
std::string LookLutDigest(const std::string& bytes);

// Reads a .cube, parses it (every refusal of ParseLookLutCube applies) and
// digests the bytes as they are on disk. `text` is what was read, verbatim:
// the copy into the package is a copy of these bytes, so the digest stays
// the digest of what both engines will read.
bool ReadLookLutFile(const std::string& path, std::string& text,
                     std::string& digest, LookLutSamples& samples,
                     std::string& error);

// Copies an author's .cube into the package that holds `projectPath` and
// returns the reference the operation will carry. A file already there
// under the same digest is verified and left alone when intact. A damaged
// copy is replaced atomically with the author's verified bytes (FI13).
//
// `color` is the project's colour management, and the declared space is
// checked against it (ResolveLookLutStage) BEFORE anything is written: a
// refused import leaves no file behind. It is the same check
// Document::Validate runs, so the import cannot accept what the document
// would then refuse.
bool ImportLookLutIntoPackage(const std::string& projectPath,
                              const std::string& cubePath,
                              const std::string& space,
                              const ColorManagementSettings& color,
                              SequenceLut& lut, std::string& error);

// The sequence's look, resolved against the package directory that holds
// the document: where it stands in the chain, the file FFmpeg is handed,
// and the samples Metal uploads. Refuses by name a missing file and a file
// whose bytes no longer match the digest -- the second is the whole point
// of storing a digest, since a LUT replaced under the same name would
// otherwise change the picture in silence.
struct LoadedLookLut {
    LookLutStage stage = LookLutStage::None;
    std::string digest;
    std::string path;
    LookLutSamples samples;
};

bool LoadSequenceLookLut(const DocumentSequence& sequence,
                         const ColorManagementSettings& color,
                         const std::string& projectDirectory,
                         LoadedLookLut& output, std::string& error);
