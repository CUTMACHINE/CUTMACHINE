#pragma once

// REMOTION-PARITY-2026-09 REM3 -- docs/roadmap/REMOTION-PARITY-2026-09.md.
// Where an evaluated node lands in the frame once its transforms and its
// ancestors' are applied, computed the way the painter applies them
// (MotionRender.mm): translate, rotate and scale around the centre of the
// box, the order of CSS individual transforms, an instance's canvas mapped
// into its box. One geometry for what is drawn, what an effect covers
// (REM3) and what a click selects (REM5): the contract's constraint 6, no
// second clickable geometry written by hand.
//
// Portable: no framework.

#include "MotionEvaluate.h"

#include <vector>

// x' = a x + c y + tx, y' = b x + d y + ty -- Core Graphics' convention.
struct MotionAffine {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double tx = 0.0;
    double ty = 0.0;

    // `this` applied after `inner`.
    MotionAffine After(const MotionAffine& inner) const;
    MotionAffine Inverse() const;
    void Apply(double x, double y, double& outX, double& outY) const;
    // How much the transform scales an area, as a length factor:
    // sqrt(|det|), what a blur's deviation is multiplied by.
    double LengthScale() const;
};

// REMOTION-PARITY-2026-09 REM4 -- a projective map of the plane, 3x3,
// row-major: (x, y) -> (m0 x + m1 y + m2, m3 x + m4 y + m5) / (m6 x + m7 y
// + m8). What a node turned in depth becomes once flattened onto its
// parent, as CSS flattens it; an affine map is one whose last row is
// (0, 0, 1).
struct MotionHomography {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    static MotionHomography FromAffine(const MotionAffine& affine);
    MotionHomography After(const MotionHomography& inner) const;
    MotionHomography Inverse() const;
    // False when the point lands behind the viewer (w <= 0).
    bool Apply(double x, double y, double& outX, double& outY) const;
    // sqrt(|det J|) at (x, y): how much a length there is scaled.
    double LocalScale(double x, double y) const;
};

// MOTION-WEB-2026-10 MW5 -- the point a node's scale, rotate and skew turn
// about, in composition pixels: its transformOrigin in its box, the centre
// by default (bit for bit the centre it always was).
double MotionPivotX(const MotionEvaluatedNode& node);
double MotionPivotY(const MotionEvaluatedNode& node);

// REM4 -- a node's transform in its parent's space, 4x4 row-major, for
// points of its plane: translate, rotateX, rotateY, rotate, skew (MW5),
// scale about its transform origin (the centre of its box by default),
// then `perspective` (0: none) about (originX, originY), its parent's
// centre.
void MotionNodeMatrix3D(const MotionEvaluatedNode& node, double perspective,
                        double originX, double originY, double matrix[16]);

// REM4 -- that matrix restricted to the node's plane (z = 0).
MotionHomography MotionPlaneHomography(const double matrix[16]);

// The node's own transform, in its parent's space: its affine map, or,
// when it turns in depth, the plane's projective map (REM4).
MotionHomography MotionNodeMapping(const MotionEvaluatedNode& node);

// The node's own transform, in its parent's space.
MotionAffine MotionNodeTransform(const MotionEvaluatedNode& node);

// An instance's canvas into its box (MotionInstanceState).
MotionAffine MotionInstanceTransform(const MotionEvaluatedNode& node);

// The frame-space bounds of `node`'s box and of every descendant's, each
// through its transforms, `parent` being the transform of the space the
// node sits in. What the node paints can spill past a box (a glyph's
// overhang, a stroke's width): the effects add their reach to this.
MotionBox MotionSubtreeBounds(const MotionEvaluatedNode& node,
                              const MotionAffine& parent);

// REM4 -- the same through a projective parent: a subtree under a node
// turned in depth.
MotionBox MotionSubtreeBounds(const MotionEvaluatedNode& node,
                              const MotionHomography& parent);

// REM3 -- how far each effect of a list reaches past what it applies to, in
// the node's own pixels (before its transform): three deviations of a blur,
// a shadow's offset and blur, a glow's blur, a displacement's amplitude.
double MotionFilterReach(const std::vector<MotionFilter>& filters);

// REM3 -- fills `filter_domain` on every node that carries effects: its
// subtree's frame bounds, grown by their reach at its scale. The pixels an
// effect may write, and what the descriptor gives a reader. REM4 -- and
// `quad` on every node turned in depth: where its corners land.
void ResolveMotionEffectDomains(MotionFrame& frame);
