///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLA_SUPPORTTUNING_HPP
#define SLA_SUPPORTTUNING_HPP

#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

namespace sla {

// The support settings shown in the support points panel, for the active tree
// type prefix ("" or "branching"). The pad keys have no prefix.
std::vector<std::string> support_tuning_keys(const std::string &prefix);

// Keeps the values SLAPrint::validate accepts: the tip no wider than the
// pillar and the penetration no longer than the head. Returns true if a value
// had to change.
bool clamp_support_tuning(DynamicPrintConfig &cfg, const std::string &prefix);

// CHITUBOX's raft choice, stored as pad_enable plus pad_shape.
enum class RaftChoice { none, skate, grid };

RaftChoice raft_choice(const DynamicPrintConfig &cfg);
void set_raft_choice(DynamicPrintConfig &cfg, RaftChoice choice);

// Copies the panel keys that object_cfg overrides into the print profile and
// returns them.
std::vector<std::string> copy_tuning_to_profile(const DynamicPrintConfig &object_cfg,
                                                DynamicPrintConfig &profile,
                                                const std::string &prefix);

}} // namespace Slic3r::sla

#endif // SLA_SUPPORTTUNING_HPP
