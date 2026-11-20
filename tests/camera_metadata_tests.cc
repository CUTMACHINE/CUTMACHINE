// G7, G3 -- docs/roadmap/GRADE-2026-09.md. Reading a Sony camera's white
// balance, capture gamma and primaries out of one rtmd packet, on packets
// built by hand in the layout an FX30 writes: a lens set, then the camera
// unit set and its local tags, labels copied from a real rush.
#include "CameraMetadata.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

using Bytes = std::vector<uint8_t>;

const Bytes kLensSet = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x53, 0x01, 0x01,
                        0x0C, 0x02, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00};
const Bytes kCameraSet = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x53, 0x01, 0x01,
                          0x0C, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00, 0x00};

Bytes Tag(uint16_t tag, const Bytes& value) {
    Bytes out = {static_cast<uint8_t>(tag >> 8), static_cast<uint8_t>(tag),
                 static_cast<uint8_t>(value.size() >> 8),
                 static_cast<uint8_t>(value.size())};
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

// A set: its label, a BER length (long form, three bytes, as the FX30
// writes it, unless `shortForm`), its tags.
Bytes Set(const Bytes& label, const std::vector<Bytes>& tags,
          bool shortForm = false) {
    Bytes body;
    for (const Bytes& tag : tags)
        body.insert(body.end(), tag.begin(), tag.end());
    Bytes out = label;
    if (shortForm) {
        out.push_back(static_cast<uint8_t>(body.size()));
    } else {
        out.insert(out.end(),
                   {0x83, 0x00, static_cast<uint8_t>(body.size() >> 8),
                    static_cast<uint8_t>(body.size())});
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes Packet(const std::vector<Bytes>& sets) {
    Bytes out = {0x00, 0x1C, 0x01, 0x00};  // A header before the sets.
    for (const Bytes& set : sets) out.insert(out.end(), set.begin(), set.end());
    return out;
}

const Bytes kSLog3 = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x06,
                      0x0E, 0x06, 0x04, 0x01, 0x01, 0x01, 0x06, 0x05};
const Bytes kSGamut3Cine = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x06,
                            0x0E, 0x06, 0x04, 0x01, 0x01, 0x03, 0x01, 0x05};
// The SMPTE label of BT.709 coding equations, which the FX30 writes under
// 0x321A: a registered label this reader has no reason to map.
const Bytes kBt709Coding = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01,
                            0x04, 0x01, 0x01, 0x01, 0x02, 0x02, 0x00, 0x00};

bool Read(const Bytes& packet, SonyRtmdCamera& camera) {
    camera.white_balance_kelvin = -1;
    camera.transfer = "stale";
    return ParseSonyRtmdCamera(packet.data(), packet.size(), camera);
}

}  // namespace

int main() {
    SonyRtmdCamera camera;
    // The lens set carries a 0x810E- and a 0x3210-looking pair too: only the
    // camera set's count.
    const Bytes lens =
        Set(kLensSet, {Tag(0x8000, {0x02, 0xC5}), Tag(0x810E, {0x0F, 0xA0}),
                       Tag(0x3210, kSLog3)});
    const Bytes fx30 =
        Set(kCameraSet,
            {Tag(0x8115, {0x09, 0xC4}),               // ISO 2500
             Tag(0x8106, {0, 0, 0, 25, 0, 0, 0, 1}),  // 25 fps
             Tag(0x810E, {0x11, 0x94}),               // 4500 K
             Tag(0x3210, kSLog3), Tag(0x3219, kSGamut3Cine),
             Tag(0x321A, kBt709Coding), Tag(0x8114, {'T', 'E', 'S', 'T'})});
    Check(Read(Packet({lens, fx30}), camera) &&
              camera.white_balance_kelvin == 4500 &&
              camera.transfer == "sony_slog3" &&
              camera.gamut == "sony_sgamut3_cine",
          "the camera set says 4500 K, S-Log3, S-Gamut3.Cine: " +
              std::to_string(camera.white_balance_kelvin) + " " +
              camera.transfer + " " + camera.gamut);
    Check(Read(Packet({lens, Set(kCameraSet, {Tag(0x810E, {0x11, 0x94})})}),
               camera) &&
              camera.transfer.empty() && camera.gamut.empty(),
          "the lens set's S-Log3 label is not the picture's");
    Check(Read(Packet({Set(kCameraSet, {Tag(0x810E, {0x0E, 0x21})}, true)}),
               camera) &&
              camera.white_balance_kelvin == 3617,
          "a short BER length reads the same");
    Check(!Read(Packet({lens}), camera), "no camera set, nothing read");
    Check(
        Read(Packet({Set(kCameraSet, {Tag(0x8115, {0x09, 0xC4})})}), camera) &&
            camera.white_balance_kelvin == 0 && camera.transfer.empty(),
        "a camera set without the tags says nothing");
    Check(
        Read(Packet({Set(kCameraSet, {Tag(0x810E, {0x01, 0xF4})})}), camera) &&
            camera.white_balance_kelvin == 0,
        "500 K is no camera's white balance");
    Check(Read(Packet({Set(kCameraSet, {Tag(0x810E, {0x11, 0x94, 0x00})})}),
               camera) &&
              camera.white_balance_kelvin == 0,
          "a white balance of three bytes is not the tag this reads");
    Bytes otherGamma = kSLog3;
    otherGamma.back() = 0x04;
    Bytes shortGamut(kSGamut3Cine.begin(), kSGamut3Cine.end() - 1);
    Check(Read(Packet({Set(kCameraSet,
                           {Tag(0x3210, otherGamma), Tag(0x3219, shortGamut),
                            Tag(0x3219, kBt709Coding)})}),
               camera) &&
              camera.transfer.empty() && camera.gamut.empty(),
          "a label no file has proved is left unread, never guessed");
    Bytes truncated = Packet({fx30});
    truncated.resize(truncated.size() - 10);
    Check(!Read(truncated, camera) && camera.white_balance_kelvin == 0 &&
              camera.transfer.empty(),
          "a set longer than the packet is refused, and says nothing");
    Check(!ParseSonyRtmdCamera(nullptr, 0, camera), "no packet, nothing");

    if (failures == 0) std::cout << "camera_metadata: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
