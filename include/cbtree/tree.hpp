#pragma once
#include <memory>
#include <string>
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

  Status put(Key k, Value v);
  LookupResult get(Key k);

  // Test hooks (progressively exposed in later tasks)
  void set_probabilities(double p_parent, double p_placeholder);
  int debug_height() const;

 private:
  Node* root_;
  std::unique_ptr<SsDPageStore> ssd_;
  double p_parent_{kDefaultPParent};
  double p_placeholder_{kDefaultPPlaceholder};
};

}  // namespace cbtree
