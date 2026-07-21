#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "cbtree/types.hpp"
#include "cbtree/node.hpp"
#include "cbtree/ssd_page_store.hpp"
#include "cbtree/adaptive_policy.hpp"

namespace cbtree {

class Tree {
 public:
  explicit Tree(const std::string& ssd_path);
  ~Tree();

  Tree(const Tree&) = delete;
  Tree& operator=(const Tree&) = delete;

  Tree(Tree&&) = default;
  Tree& operator=(Tree&&) = default;

  Status put(Key k, Value v);
  LookupResult get(Key k);
  std::vector<std::pair<Key, Value>> scan(Key lo, Key hi);

  // Test hooks (progressively exposed in later tasks)
  void set_probabilities(double p_parent, double p_placeholder);
  int debug_height() const;
  bool debug_parent_cache_contains(Key k) const;
  Status debug_flush_all();
  bool debug_all_leaves_have_cache() const;
  bool debug_root_has_cache() const;

  // Debug: eviction hooks (Task 11)
  void debug_clear_all_caches();
  bool debug_leaf_index_empty() const;
  bool debug_some_keys_in_leaf_cache() const;

  // Debug factory: creates a height=2 tree with two leaf children
  static Tree DebugTwoLeaves(const std::string& ssd_path);

  // Debug: verify height >= 3 nodes have no cache
  bool debug_height3_nodes_have_no_cache() const;

 private:
  Node* descend_to_leaf(Key k,
                        std::vector<std::pair<Node*, uint64_t>>& versions);
  void split_leaf(Node* leaf);
  void split_internal(Node* node);
  static void collect_leaves(const Node* node, std::vector<const Node*>& leaves);
  static void collect_leaves(Node* node, std::vector<Node*>& leaves);
  static void collect_leaves_in_range(Node* node, Key lo, Key hi,
                                      std::vector<Node*>& leaves);

  // Task 11: eviction helpers
  Node* find_leaf_for_key(Node* parent, Key k);
  void register_in_leaf_index(Node* leaf, Key k);
  Status evict_leaf_if_needed(Node* leaf);
  Status evict_parent_if_needed(Node* parent);

  Node* root_;
  std::unique_ptr<SsDPageStore> ssd_;
  AdaptivePolicy adaptive_policy_;
  double p_parent_{kDefaultPParent};
  double p_placeholder_{kDefaultPPlaceholder};
};

}  // namespace cbtree
