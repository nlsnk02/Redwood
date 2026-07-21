#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "cbtree/types.hpp"
#include "cbtree/node.hpp"
#include "cbtree/ssd_page_store.hpp"

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

  // Test hooks (progressively exposed in later tasks)
  void set_probabilities(double p_parent, double p_placeholder);
  int debug_height() const;
  bool debug_parent_cache_contains(Key k) const;
  Status debug_flush_all();
  bool debug_all_leaves_have_cache() const;
  bool debug_root_has_cache() const;

  // Debug factory: creates a height=2 tree with two leaf children
  static Tree DebugTwoLeaves(const std::string& ssd_path);

 private:
  Node* descend_to_leaf(Key k,
                        std::vector<std::pair<Node*, uint64_t>>& versions);
  void split_leaf(Node* leaf);
  void split_internal(Node* node);
  static void collect_leaves(const Node* node, std::vector<const Node*>& leaves);

  Node* root_;
  std::unique_ptr<SsDPageStore> ssd_;
  double p_parent_{kDefaultPParent};
  double p_placeholder_{kDefaultPPlaceholder};
};

}  // namespace cbtree
