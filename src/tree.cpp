// src/tree.cpp
#include "cbtree/tree.hpp"
#include "cbtree/cache_attachment.hpp"
#include <algorithm>
#include <random>

namespace cbtree {

Tree::Tree(const std::string& ssd_path)
    : ssd_(std::make_unique<SsDPageStore>(ssd_path)) {
  root_ = new Node{};
  root_->height = 1;
  root_->cache = std::make_unique<CacheAttachment>();
  root_->page_id = ssd_->alloc_page();
}

Tree::~Tree() {
  delete root_;
  root_ = nullptr;
}

Node* Tree::descend_to_leaf(Key k,
                            std::vector<std::pair<Node*, uint64_t>>& versions) {
  Node* cur = root_;
  while (cur->height > 1) {
    versions.emplace_back(cur, cur->version.load(std::memory_order_acquire));
    // Binary search separators to determine child index
    auto it = std::upper_bound(cur->separators.begin(), cur->separators.end(), k);
    size_t idx = it - cur->separators.begin();
    cur = cur->children[idx];
  }
  versions.emplace_back(cur, cur->version.load(std::memory_order_acquire));
  return cur;  // leaf node
}

Status Tree::put(Key k, Value v) {
  thread_local std::mt19937_64 rng(std::random_device{}());

  // ---- Phase 1: check parent (root) cache for existing key ----
  // "沿途命中": if the key already exists in any upper cache, update it there.
  if (root_->height >= 2) {
    LookupResult lr = root_->cache->lookup(k);
    bool exists_in_root =
        (lr.status == Status::Ok) || root_->cache->has_absent(k);

    if (exists_in_root) {
      // Key already has a slot in root cache -- update it in place.
      // upsert() will find the existing slot and update value/state.
      Status s = root_->cache->upsert(k, v);
      if (s == Status::Ok) return Status::Ok;
      // If upsert failed (Full), evict a victim and retry.
      if (s == Status::Full) {
        Key victim_key = 0;
        Value victim_val = 0;
        bool victim_dirty = false;
        Status evict_s = root_->cache->pick_clock_victim(&victim_key, &victim_val,
                                                         &victim_dirty);
        if (evict_s != Status::Ok) return Status::Full;
        s = root_->cache->upsert(k, v);
        if (s == Status::Ok) return Status::Ok;
        return s;
      }
      return s;
    }

    // Key NOT in root cache.  Decide whether to insert here (P_parent).
    bool use_parent = false;
    if (p_parent_ >= 1.0) {
      use_parent = true;
    } else if (p_parent_ > 0.0) {
      use_parent = std::bernoulli_distribution{p_parent_}(rng);
    }

    if (use_parent) {
      // Try to insert into root cache.
      Status s = root_->cache->upsert(k, v);
      if (s == Status::Ok) return Status::Ok;

      if (s == Status::Full) {
        // Evict a victim from root cache (no SSD write -- root cache is
        // purely in-memory).
        Key victim_key = 0;
        Value victim_val = 0;
        bool victim_dirty = false;
        Status evict_s = root_->cache->pick_clock_victim(&victim_key, &victim_val,
                                                         &victim_dirty);
        if (evict_s != Status::Ok) return Status::Full;
        s = root_->cache->upsert(k, v);
        if (s == Status::Ok) return Status::Ok;
        return s;
      }
      return s;
    }
    // P_parent not selected -- fall through to leaf
  }

  // ---- Phase 2: descend to leaf ----
  std::vector<std::pair<Node*, uint64_t>> versions;
  Node* leaf = descend_to_leaf(k, versions);

  // ---- Phase 3: leaf cache upsert ----
  Status s = leaf->cache->upsert(k, v);
  if (s == Status::Ok) {
    // Do NOT update leaf_keys (lazy index -- populated during flush)
    return Status::Ok;
  }

  // Cache is full -- evict one slot via CLOCK and retry
  if (s == Status::Full) {
    Key victim_key = 0;
    Value victim_val = 0;
    bool victim_dirty = false;

    Status evict_s = leaf->cache->pick_clock_victim(&victim_key, &victim_val,
                                                    &victim_dirty);
    if (evict_s != Status::Ok) {
      return Status::Full;  // nothing to evict (should not happen)
    }

    // Write dirty victim to SSD
    if (victim_dirty) {
      Status write_s = ssd_->put_record(leaf->page_id, victim_key, victim_val);
      if (write_s != Status::Ok) {
        return Status::Error;
      }
    }

    // Retry upsert now that a slot is free
    s = leaf->cache->upsert(k, v);
    if (s == Status::Ok) {
      return Status::Ok;
    }
    return s;
  }

  return s;
}

LookupResult Tree::get(Key k) {
  constexpr int kMaxRetries = 64;

  // Per-thread random engine for placeholder probability
  thread_local std::mt19937_64 rng(std::random_device{}());

  for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
    // Step 1: read version before lookup (must be even)
    uint64_t v = root_->version.load(std::memory_order_acquire);
    if (v & 1) {  // odd = structural change in progress
      continue;   // spin/retry
    }

    // Descend to leaf (record versions along the path)
    std::vector<std::pair<Node*, uint64_t>> versions;
    Node* leaf = descend_to_leaf(k, versions);
    uint64_t leaf_v = versions.back().second;  // leaf version snapshot

    // Step 2: cache lookup -- top-down (parent cache first, then leaf cache)
    if (root_->height >= 2) {
      LookupResult pr = root_->cache->lookup(k);
      if (pr.status == Status::Ok) {
        return pr;  // OCCUPIED hit in parent cache
      }
      if (root_->cache->has_absent(k)) {
        return {Status::NotFound};  // ABSENT hit in parent cache
      }
    }

    LookupResult r = leaf->cache->lookup(k);
    if (r.status == Status::Ok) {
      return r;  // OCCUPIED hit in leaf cache
    }
    if (leaf->cache->has_absent(k)) {
      return {Status::NotFound};  // ABSENT hit in leaf cache
    }

    // Step 3: cache miss -- try place placeholder with P_placeholder probability
    bool has_placed = false;
    int placeholder_idx = -1;
    if (p_placeholder_ >= 1.0) {
      Status ps = leaf->cache->try_place_placeholder(k, &placeholder_idx);
      has_placed = (ps == Status::Ok);
    } else if (p_placeholder_ > 0.0) {
      if (std::bernoulli_distribution{p_placeholder_}(rng)) {
        Status ps = leaf->cache->try_place_placeholder(k, &placeholder_idx);
        has_placed = (ps == Status::Ok);
      }
    }

    // Step 4: query SSD (leaf's page)
    r = ssd_->get_record(leaf->page_id, k);

    // Step 5: post-SSD secondary cache recheck (leaf cache only)
    LookupResult r2 = leaf->cache->lookup(k);
    if (r2.status == Status::Ok) {
      if (has_placed) {
        leaf->cache->fill_placeholder(placeholder_idx, r2.value);
      }
      // Verify leaf version is stable (root version check at step 1 covers structural changes)
      if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
        return r2;
      }
      continue;  // version changed, retry
    }
    if (leaf->cache->has_absent(k)) {
      if (has_placed) {
        leaf->cache->fill_placeholder_absent(placeholder_idx);
      }
      if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
        return {Status::NotFound};
      }
      continue;  // version changed, retry
    }

    // Step 6: fill placeholder based on SSD result
    if (has_placed) {
      if (r.status == Status::Ok) {
        leaf->cache->fill_placeholder(placeholder_idx, r.value);
      } else {
        leaf->cache->fill_placeholder_absent(placeholder_idx);
      }
    }

    // Step 7: version check after read (leaf version for leaf-level ops)
    if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
      return r;
    }
    // Version changed -- retry
  }

  return {Status::Retry};
}

void Tree::set_probabilities(double p_parent, double p_placeholder) {
  p_parent_ = p_parent;
  p_placeholder_ = p_placeholder;
}

int Tree::debug_height() const {
  return root_->height;
}

bool Tree::debug_parent_cache_contains(Key k) const {
  if (root_->height < 2) return false;
  // Check Occupied state via lookup
  LookupResult r = root_->cache->lookup(k);
  if (r.status == Status::Ok) return true;
  // Check Absent state
  return root_->cache->has_absent(k);
}

void Tree::collect_leaves(const Node* node, std::vector<const Node*>& leaves) {
  if (!node) return;
  if (node->height == 1) {
    leaves.push_back(node);
    return;
  }
  for (const Node* child : node->children) {
    collect_leaves(child, leaves);
  }
}

Status Tree::debug_flush_all() {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);

  for (const Node* leaf_const : leaves) {
    Node* leaf = const_cast<Node*>(leaf_const);

    // Write all dirty Occupied entries to SSD and register in leaf index
    std::vector<std::pair<Key, Value>> dirty_entries;
    leaf->cache->flush_dirty(dirty_entries);
    for (const auto& [k, v] : dirty_entries) {
      ssd_->put_record(leaf->page_id, k, v);
      leaf->leaf_keys.push_back(k);
      leaf->leaf_page_ids.push_back(leaf->page_id);
    }

    // Sort and deduplicate leaf_keys
    std::sort(leaf->leaf_keys.begin(), leaf->leaf_keys.end());
    auto last = std::unique(leaf->leaf_keys.begin(), leaf->leaf_keys.end());
    leaf->leaf_keys.erase(last, leaf->leaf_keys.end());
    leaf->leaf_page_ids.assign(leaf->leaf_keys.size(), leaf->page_id);

    // Check if leaf needs to split
    if (leaf->leaf_keys.size() > kLeafFanout) {
      split_leaf(leaf);
    }
  }
  return Status::Ok;
}

void Tree::split_leaf(Node* leaf) {
  // 1. Set version to odd (structural change in progress)
  leaf->version.fetch_add(1, std::memory_order_acq_rel);

  // 2. Sort cache
  (void)leaf->cache->occupied_sorted();
  leaf->cache->set_sorted_flag(true);

  // 3. Pick mid from leaf_keys
  Key mid = leaf->leaf_keys[leaf->leaf_keys.size() / 2];

  // 4. Allocate new page and split SSD data
  PageId new_right_id = 0;
  ssd_->split_page(leaf->page_id, mid, &new_right_id);

  // 5. Create right leaf node
  Node* L_right = new Node{};
  L_right->height = 1;
  L_right->cache = std::make_unique<CacheAttachment>();
  L_right->page_id = new_right_id;

  // 6. Split leaf index (leaf_keys, leaf_page_ids)
  std::vector<Key> left_keys;
  std::vector<PageId> left_pids;
  std::vector<Key> right_keys;
  std::vector<PageId> right_pids;

  for (size_t i = 0; i < leaf->leaf_keys.size(); ++i) {
    if (leaf->leaf_keys[i] < mid) {
      left_keys.push_back(leaf->leaf_keys[i]);
      left_pids.push_back(leaf->leaf_page_ids[i]);
    } else {
      right_keys.push_back(leaf->leaf_keys[i]);
      right_pids.push_back(leaf->leaf_page_ids[i]);
    }
  }
  leaf->leaf_keys = std::move(left_keys);
  leaf->leaf_page_ids = std::move(left_pids);
  L_right->leaf_keys = std::move(right_keys);
  L_right->leaf_page_ids = std::move(right_pids);

  // 7. Split cache entries
  leaf->cache->split_into(mid, L_right->cache.get());

  // 8. Update parent
  if (leaf == root_) {
    // Create new root
    Node* new_root = new Node{};
    new_root->height = 2;
    new_root->cache = std::make_unique<CacheAttachment>();
    new_root->separators.push_back(mid);
    new_root->children.push_back(leaf);
    new_root->children.push_back(L_right);
    leaf->parent = new_root;
    L_right->parent = new_root;
    root_ = new_root;
  } else {
    // Insert into existing parent
    Node* parent = leaf->parent;
    auto it = std::lower_bound(parent->separators.begin(), parent->separators.end(),
                               mid);
    size_t idx = static_cast<size_t>(it - parent->separators.begin());
    parent->separators.insert(it, mid);
    parent->children.insert(parent->children.begin() +
                                static_cast<long>(idx) + 1,
                            L_right);
    L_right->parent = parent;
  }

  // 9. Set version back to even (stable)
  leaf->version.fetch_add(1, std::memory_order_acq_rel);

  // 10. If parent overflows, recursively split
  if (leaf->parent && leaf->parent->children.size() > kInternalFanout) {
    split_internal(leaf->parent);
  }
}

void Tree::split_internal(Node* node) {
  // 1. Set version to odd
  node->version.fetch_add(1, std::memory_order_acq_rel);

  // 2. Pick mid separator
  size_t mid_idx = node->separators.size() / 2;
  Key mid = node->separators[mid_idx];

  // 3. Create new right internal node
  Node* new_node = new Node{};
  new_node->height = node->height;
  if (new_node->height == 2) {
    new_node->cache = std::make_unique<CacheAttachment>();
  }

  // Move separator and children >= mid to new node
  // Number of children = number of separators + 1
  // The separator at mid_idx goes up to parent
  new_node->separators.assign(node->separators.begin() + static_cast<long>(mid_idx) + 1,
                              node->separators.end());
  new_node->children.assign(node->children.begin() + static_cast<long>(mid_idx) + 1,
                            node->children.end());

  // Update parent pointers for moved children
  for (Node* child : new_node->children) {
    child->parent = new_node;
  }

  // Trim original node (keep separators before mid, children up to mid_idx+1)
  node->separators.resize(mid_idx);
  node->children.resize(mid_idx + 1);

  // 4. Update parent
  if (node == root_) {
    Node* new_root = new Node{};
    new_root->height = node->height + 1;
    new_root->cache = std::make_unique<CacheAttachment>();
    new_root->separators.push_back(mid);
    new_root->children.push_back(node);
    new_root->children.push_back(new_node);
    node->parent = new_root;
    new_node->parent = new_root;
    root_ = new_root;
  } else {
    Node* parent = node->parent;
    auto it = std::lower_bound(parent->separators.begin(), parent->separators.end(),
                               mid);
    size_t idx = static_cast<size_t>(it - parent->separators.begin());
    parent->separators.insert(it, mid);
    parent->children.insert(parent->children.begin() +
                                static_cast<long>(idx) + 1,
                            new_node);
    new_node->parent = parent;
  }

  // 5. Set version back to even
  node->version.fetch_add(1, std::memory_order_acq_rel);

  // 6. Recursively split parent if overflow
  if (node->parent && node->parent->children.size() > kInternalFanout) {
    split_internal(node->parent);
  }
}

bool Tree::debug_all_leaves_have_cache() const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (!leaf->cache) return false;
  }
  return true;
}

bool Tree::debug_root_has_cache() const {
  return root_->cache != nullptr;
}

Tree Tree::DebugTwoLeaves(const std::string& ssd_path) {
  Tree t(ssd_path);

  // The constructor created a single-node tree (root at height=1).
  // Delete that root and replace with a height=2 structure.
  delete t.root_;

  // Create two leaf nodes (height=1), each with its own cache and SSD page.
  Node* leaf0 = new Node{};
  leaf0->height = 1;
  leaf0->cache = std::make_unique<CacheAttachment>();
  leaf0->page_id = t.ssd_->alloc_page();

  Node* leaf1 = new Node{};
  leaf1->height = 1;
  leaf1->cache = std::make_unique<CacheAttachment>();
  leaf1->page_id = t.ssd_->alloc_page();

  // Create root node (height=2) with cache and two children.
  Node* root = new Node{};
  root->height = 2;
  root->cache = std::make_unique<CacheAttachment>();
  root->separators.push_back(50);
  root->children.push_back(leaf0);
  root->children.push_back(leaf1);

  leaf0->parent = root;
  leaf1->parent = root;

  t.root_ = root;
  return t;
}

}  // namespace cbtree
