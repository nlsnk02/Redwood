#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include "cbtree/types.hpp"
#include "cbtree/cache_attachment.hpp"

namespace cbtree {

struct Node {
  std::atomic<uint64_t> version{0};  // even = stable, odd = structural change in progress
  int height{1};                      // leaf = 1
  Node* parent{nullptr};

  // Leaf fields (valid when height == 1)
  std::vector<Key> leaf_keys;         // ordered key list (populated only during flush)
  std::vector<PageId> leaf_page_ids;  // page id for each leaf_keys[i]
  PageId page_id{0};                  // SSD page for this leaf

  // Internal node fields (valid when height >= 2)
  std::vector<Key> separators;        // separator keys
  std::vector<Node*> children;

  // Cache (mounted when height == 1 or height == 2; nullptr for height >= 3)
  std::unique_ptr<CacheAttachment> cache;
};

}  // namespace cbtree
