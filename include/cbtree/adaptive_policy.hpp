#pragma once
#include "cbtree/types.hpp"
namespace cbtree {
struct Stats {
  double parent_hit_rate{0.0};
  double leaf_hit_rate{0.0};
  double clock_eviction_rate{0.0};
  double ssd_io_count{0.0};
};
struct Probabilities {
  double p_parent{kDefaultPParent};
  double p_placeholder{kDefaultPPlaceholder};
};
class AdaptivePolicy {
 public:
  Probabilities update(const Stats&) {
    return Probabilities{};  // v1: return defaults
  }
};
}  // namespace cbtree
