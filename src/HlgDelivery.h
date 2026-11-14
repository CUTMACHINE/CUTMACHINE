#pragma once

#include <array>

#include "ColorManagement.h"

// BH3 -- docs/roadmap/BLANC-HLG-2026-10.md. What an HLG delivery does to the
// picture. Through the ACES 1.1 HDR view, 18 % grey left at 30 % of the HLG
// signal (about 15 nits on a 1000-nit screen); DaVinci Resolve Studio 21.1
// puts it at 37 %, as BT.2408 does (38 %), so a film delivered from here came
// out 0.5 to 0.6 stop darker than the same grade from Resolve. The author chose on
// 20 October 2026 to follow Resolve.
//
// The curve is Resolve's own, measured that day on an S-Log3 chart of 30
// greys (0.25 % to 3200 % reflectance) rendered in the CUTMACHINE copy of an
// PRJH project: colour management v2, DaVinci WG/Intermediate timeline,
// Rec.2020 HLG out, HDR 1000. A toe under 5 %, BT.2408 in the midtones, a
// shoulder that reaches the peak around 17x white. It is applied per channel
// in DaVinci Wide Gamut, where that timeline works: of four models tried on
// the chart's 21 colour patches it fits Resolve best (the worst patch within
// 9.8 points of signal out of 100, skin within 1.4).

// Delivered HLG signal (0..1) of a neutral of this scene reflectance (1.0 is
// a white card, 0.18 mid grey). Strictly increasing from 0 at 0 to 1 at 32;
// 1 above.
double HlgDeliverySignal(double reflectance);

// The same curve as scene light: the reflectance HlgDeliveryEncode turns into
// HlgDeliverySignal(reflectance). This is the tone curve applied per channel
// in DaVinci Wide Gamut. Negative values pass unchanged.
double HlgDeliveryTone(double reflectance);

// BT.2408's HLG encoding of scene light: reflectance 1.0 at the HLG reference
// white, 75 % of the signal; clamped to 0..1.
double HlgDeliveryEncode(double reflectance);

// Linear light, row-major: Rec.2020 to DaVinci Wide Gamut and back, both
// D65, built from the published primaries.
using ColorMatrix3 = std::array<std::array<double, 3>, 3>;
const ColorMatrix3& Rec2020ToDaVinciWideGamut();
const ColorMatrix3& DaVinciWideGamutToRec2020();

// A linear Rec.2020 colour to its delivered HLG signal: to DaVinci Wide
// Gamut, HlgDeliveryTone per channel, back to Rec.2020, HlgDeliveryEncode.
// The CPU reference of the OpenColorIO chain in ColorManagement.cc.
RgbColor HlgDeliverySignalOf(RgbColor rec2020Linear);
