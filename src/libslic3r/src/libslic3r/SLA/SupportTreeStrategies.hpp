#ifndef SUPPORTTREESTRATEGIES_HPP
#define SUPPORTTREESTRATEGIES_HPP

#include <memory>

#include "Slic3r/Domain/ConfigDefsSLA.hpp"

namespace Slic3r { namespace sla {

enum class SupportTreeType { Default, Branching, Organic };
enum class PillarConnectionMode { zigzag, cross, dynamic };

// CHITUBOX-style shapes, defined with the config options in the domain.
using HeadShape   = Domain::sla::HeadShape;
using PillarShape = Domain::sla::PillarShape;
using BaseShape   = Domain::sla::BaseShape;
using PadShape    = Domain::sla::PadShape;

struct SupportShapes
{
    HeadShape   head   = HeadShape::ball;
    PillarShape pillar = PillarShape::round;
    BaseShape   base   = BaseShape::cone;
};

}} // namespace Slic3r::sla

#endif // SUPPORTTREESTRATEGIES_HPP
