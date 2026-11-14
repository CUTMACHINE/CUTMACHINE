#include "CameraMetadata.h"

#include "Document.h"

#include <algorithm>
#include <array>

namespace {

using Label = std::array<uint8_t, 16>;

// SMPTE universal label of the camera unit metadata set (RDD 18), as the
// FX30 writes it.
constexpr Label kCameraUnitSet = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x53,
                                  0x01, 0x01, 0x0C, 0x02, 0x01, 0x01,
                                  0x02, 0x01, 0x00, 0x00};

constexpr uint16_t kWhiteBalanceTag = 0x810E;
constexpr uint16_t kCaptureGammaTag = 0x3210;
constexpr uint16_t kCapturePrimariesTag = 0x3219;

// The labels the FX30 writes for S-Log3 and S-Gamut3.Cine (see the header).
constexpr Label kSLog3 = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x06,
                          0x0E, 0x06, 0x04, 0x01, 0x01, 0x01, 0x06, 0x05};
constexpr Label kSGamut3Cine = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x06,
                                0x0E, 0x06, 0x04, 0x01, 0x01, 0x03, 0x01, 0x05};

bool IsLabel(const uint8_t* value, size_t length, const Label& label) {
    return length == label.size() &&
           std::equal(label.begin(), label.end(), value);
}

}  // namespace

bool ParseSonyRtmdCamera(const uint8_t* data, size_t size,
                         SonyRtmdCamera& camera) {
    camera = SonyRtmdCamera{};
    if (data == nullptr) return false;
    const uint8_t* end = data + size;
    const uint8_t* set =
        std::search(data, end, kCameraUnitSet.begin(), kCameraUnitSet.end());
    if (set == end) return false;
    const uint8_t* at = set + kCameraUnitSet.size();
    if (at >= end) return false;
    // BER length: short form, or 0x8n followed by n bytes.
    size_t length = 0;
    if ((*at & 0x80) == 0) {
        length = *at;
        ++at;
    } else {
        const size_t bytes = *at & 0x7F;
        ++at;
        if (bytes == 0 || bytes > 4 || static_cast<size_t>(end - at) < bytes)
            return false;
        for (size_t index = 0; index < bytes; ++index)
            length = (length << 8) | at[index];
        at += bytes;
    }
    if (length > static_cast<size_t>(end - at)) return false;
    const uint8_t* setEnd = at + length;
    // Local tags: a 2-byte tag, a 2-byte length, the value.
    while (setEnd - at >= 4) {
        const uint16_t tag = static_cast<uint16_t>((at[0] << 8) | at[1]);
        const size_t valueLength = static_cast<size_t>((at[2] << 8) | at[3]);
        at += 4;
        if (valueLength > static_cast<size_t>(setEnd - at)) {
            camera = SonyRtmdCamera{};
            return false;
        }
        if (tag == kWhiteBalanceTag && valueLength == 2) {
            const int32_t value = (at[0] << 8) | at[1];
            if (value >= kMinimumMediaWhiteBalance &&
                value <= kMaximumMediaWhiteBalance)
                camera.white_balance_kelvin = value;
        } else if (tag == kCaptureGammaTag &&
                   IsLabel(at, valueLength, kSLog3)) {
            camera.transfer = "sony_slog3";
        } else if (tag == kCapturePrimariesTag &&
                   IsLabel(at, valueLength, kSGamut3Cine)) {
            camera.gamut = "sony_sgamut3_cine";
        }
        at += valueLength;
    }
    return true;
}
