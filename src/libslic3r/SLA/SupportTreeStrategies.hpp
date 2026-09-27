///|/ Copyright (c) Prusa Research 2022 Tomáš Mészáros @tamasmeszaros
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SUPPORTTREESTRATEGIES_HPP
#define SUPPORTTREESTRATEGIES_HPP

#include <memory>

namespace Slic3r { namespace sla {

enum class SupportTreeType { Default, Branching, Organic };
enum class PillarConnectionMode { zigzag, cross, dynamic };

// CHITUBOX-style shapes of the support parts. The defaults are PrusaSlicer's
// original geometry.
enum class HeadShape { ball, cone };        // tip touching the model
enum class PillarShape { round, square };   // pillar cross-section
enum class BaseShape { cone, cylinder };    // foot on the pad or build plate

// CHITUBOX-style raft: a solid plate ("skate") or a plate with square
// openings ("cross grid").
enum class PadShape { solid, grid };

struct SupportShapes
{
    HeadShape   head   = HeadShape::ball;
    PillarShape pillar = PillarShape::round;
    BaseShape   base   = BaseShape::cone;
};

}} // namespace Slic3r::sla

#endif // SUPPORTTREESTRATEGIES_HPP
