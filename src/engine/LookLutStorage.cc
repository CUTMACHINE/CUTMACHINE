#include "LookLutStorage.h"

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace {

namespace fs = std::filesystem;

bool ReadWholeFile(const fs::path& path, std::string& text,
                   std::string& error) {
    std::error_code pathError;
    if (!fs::is_regular_file(path, pathError) || pathError) {
        error = "'" + path.string() + "' is not a readable file";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to read '" + path.string() + "'";
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
    if (input.bad()) {
        error = "unable to read '" + path.string() + "'";
        return false;
    }
    return true;
}

}  // namespace

std::string LookLutDigest(const std::string& bytes) {
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
    std::ostringstream result;
    for (const unsigned char byte : digest) {
        result << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(byte);
    }
    return result.str();
}

bool ReadLookLutFile(const std::string& path, std::string& text,
                     std::string& digest, LookLutSamples& samples,
                     std::string& error) {
    text.clear();
    digest.clear();
    samples = LookLutSamples{};
    error.clear();
    if (!ReadWholeFile(fs::path(path), text, error)) return false;
    if (!ParseLookLutCube(text, samples, error)) {
        error = "'" + path + "': " + error;
        text.clear();
        return false;
    }
    digest = LookLutDigest(text);
    return true;
}

bool ImportLookLutIntoPackage(const std::string& projectPath,
                              const std::string& cubePath,
                              const std::string& space,
                              const ColorManagementSettings& color,
                              SequenceLut& lut, std::string& error) {
    lut = SequenceLut{};
    error.clear();
    const fs::path project(projectPath);
    std::error_code pathError;
    const fs::path package = project.parent_path();
    if (!fs::is_directory(package, pathError) || pathError) {
        error = "the project package directory is not accessible";
        return false;
    }
    std::string text;
    std::string digest;
    LookLutSamples samples;
    if (!ReadLookLutFile(cubePath, text, digest, samples, error)) return false;
    SequenceLut candidate;
    candidate.digest = digest;
    candidate.space = space;
    candidate.name = fs::path(cubePath).filename().string();
    LookLutStage stage = LookLutStage::None;
    if (!ResolveLookLutStage(candidate, color, stage, error)) return false;
    const fs::path relative(LookLutPackagePath(digest));
    const fs::path destination = package / relative;
    if (!fs::create_directories(destination.parent_path(), pathError) &&
        pathError) {
        error = "unable to create the package LUT directory: " +
                pathError.message();
        return false;
    }
    // FI13 -- docs/roadmap/FIABILITE-2026-10.md. Check the bytes, not just
    // their name. Repair a damaged copy by rename so an existing reader
    // retains its file; a healthy copy keeps its inode and modification time.
    pathError.clear();
    const fs::file_status status = fs::symlink_status(destination, pathError);
    if (pathError && pathError != std::errc::no_such_file_or_directory) {
        error = "unable to inspect the package LUT: " + pathError.message();
        return false;
    }
    if (fs::exists(status) && !fs::is_regular_file(status) &&
        !fs::is_symlink(status)) {
        error = "the package LUT is not a regular file";
        return false;
    }
    std::string stored;
    std::string readError;
    const bool intact = fs::is_regular_file(status) &&
                        ReadWholeFile(destination, stored, readError) &&
                        LookLutDigest(stored) == digest;
    if (!intact) {
        pathError.clear();
        const fs::path staging = destination.parent_path() /
                                 (destination.filename().string() +
                                  ".cutmachine-" + GenerateUlid() + ".tmp");
        {
            std::ofstream output(staging, std::ios::binary | std::ios::trunc);
            output << text;
            output.close();
            if (!output) {
                std::error_code ignored;
                fs::remove(staging, ignored);
                error = "unable to write the LUT into the project package";
                return false;
            }
        }
        fs::rename(staging, destination, pathError);
        if (pathError) {
            std::error_code ignored;
            fs::remove(staging, ignored);
            error = "unable to publish the LUT into the project package: " +
                    pathError.message();
            return false;
        }
    }
    lut = candidate;
    return true;
}

bool LoadSequenceLookLut(const DocumentSequence& sequence,
                         const ColorManagementSettings& color,
                         const std::string& projectDirectory,
                         LoadedLookLut& output, std::string& error) {
    output = LoadedLookLut{};
    error.clear();
    LookLutStage stage = LookLutStage::None;
    if (!ResolveLookLutStage(sequence.look_lut, color, stage, error))
        return false;
    if (stage == LookLutStage::None) return true;
    const fs::path path = fs::path(projectDirectory) /
                          LookLutPackagePath(sequence.look_lut.digest);
    std::string text;
    if (!ReadWholeFile(path, text, error)) {
        error =
            "the sequence's look LUT is missing from the project "
            "package: " +
            error;
        return false;
    }
    if (LookLutDigest(text) != sequence.look_lut.digest) {
        error = "the look LUT '" + path.string() +
                "' no longer matches the digest the sequence names";
        return false;
    }
    if (!ParseLookLutCube(text, output.samples, error)) {
        error = "'" + path.string() + "': " + error;
        return false;
    }
    output.stage = stage;
    output.digest = sequence.look_lut.digest;
    output.path = fs::absolute(path).lexically_normal().string();
    return true;
}
