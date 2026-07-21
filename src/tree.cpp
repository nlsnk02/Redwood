// src/tree.cpp
#include "cbtree/tree.hpp"
#include "cbtree/cache_attachment.hpp"
#include <algorithm>
#include <map>
#include <random>
#include <set>

namespace cbtree {

Tree::Tree(const std::string& ssd_path)
    : ssd_(std::make_unique<SsDPageStore>(ssd_path)) {
  root_ = new Node{};
  root_->height = 1;
  root_->cache = std::make_unique<CacheAttachment>();
  root_->page_id = ssd_->alloc_page();
}

static void delete_subtree(Node* node) {
  if (!node) return;
  for (Node* child : node->children) {
    delete_subtree(child);
  }
  delete node;
}

Tree::~Tree() {
  delete_subtree(root_);
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

Node* Tree::find_leaf_for_key(Node* parent, Key k) {
  Node* cur = parent;
  while (cur->height > 1) {
    auto it = std::upper_bound(cur->separators.begin(), cur->separators.end(), k);
    size_t idx = it - cur->separators.begin();
    cur = cur->children[idx];
  }
  return cur;
}

void Tree::register_in_leaf_index(Node* leaf, Key k) {
  std::lock_guard<std::mutex> lock(leaf->leaf_index_mutex);
  auto it = std::lower_bound(leaf->leaf_keys.begin(), leaf->leaf_keys.end(), k);
  if (it != leaf->leaf_keys.end() && *it == k) return;  // already exists
  size_t idx = it - leaf->leaf_keys.begin();
  leaf->leaf_keys.insert(it, k);
  leaf->leaf_page_ids.insert(leaf->leaf_page_ids.begin() + idx, leaf->page_id);
}

Status Tree::evict_leaf_if_needed(Node* leaf) {
  if (leaf->cache->occupied_count() <=
      static_cast<int>(kCacheSlots * kLeafFillThreshold))
    return Status::Ok;
  Key victim_key = 0;
  Value victim_val = 0;
  bool victim_dirty = false;
  int victim_idx = -1;
  if (leaf->cache->find_clock_victim(&victim_key, &victim_val, &victim_dirty,
                                     &victim_idx) != Status::Ok)
    return Status::Ok;

  if (victim_dirty) {
    // Write to SSD BEFORE clearing the slot (prevents get() from missing)
    Status write_s = ssd_->put_record(leaf->page_id, victim_key, victim_val);
    if (write_s != Status::Ok) return Status::Ok;  // leave slot intact on failure
    register_in_leaf_index(leaf, victim_key);
  }
  // Now clear the slot (verified against victim_key)
  leaf->cache->evict_slot(victim_idx, victim_key);
  return Status::Ok;
}

Status Tree::evict_parent_if_needed(Node* parent) {
  if (parent->cache->occupied_count() <=
      static_cast<int>(kCacheSlots * kParentFillThreshold))
    return Status::Ok;
  Key victim_key = 0;
  Value victim_val = 0;
  bool victim_dirty = false;
  int victim_idx = -1;
  if (parent->cache->find_clock_victim(&victim_key, &victim_val, &victim_dirty,
                                       &victim_idx) != Status::Ok)
    return Status::Ok;

  if (victim_dirty) {
    // Demote to leaf: find which leaf owns this key
    Node* leaf = find_leaf_for_key(parent, victim_key);
    // If leaf cache is full, evict from it first
    evict_leaf_if_needed(leaf);
    // Insert into leaf cache
    leaf->cache->upsert(victim_key, victim_val);
  }
  // Now clear the slot (verified against victim_key)
  parent->cache->evict_slot(victim_idx, victim_key);
  return Status::Ok;
}

Status Tree::put(Key k, Value v) {
  thread_local std::mt19937_64 rng(std::random_device{}());

  // ---- Phase 1: check parent (root) cache for existing key ----
  // "沿途命中": if the key already exists in any upper cache, update it there.
  // Only height <= 2 nodes have caches.
  if (root_->height >= 2 && root_->cache) {
    LookupResult lr = root_->cache->lookup(k);
    bool exists_in_root =
        (lr.status == Status::Ok) || root_->cache->has_absent(k);

    if (exists_in_root) {
      // Key already has a slot in root cache -- update it in place.
      // upsert() will find the existing slot and update value/state.
      constexpr int kMaxParentRetries = 64;
      for (int retry = 0; retry < kMaxParentRetries; ++retry) {
        Status s = root_->cache->upsert(k, v);
        if (s == Status::Ok) {
          evict_parent_if_needed(root_);
          return Status::Ok;
        }

        if (s != Status::Full) return s;

        // Evict a victim and retry
        Key victim_key = 0;
        Value victim_val = 0;
        bool victim_dirty = false;
        int victim_idx = -1;
        Status evict_s = root_->cache->find_clock_victim(&victim_key, &victim_val,
                                                         &victim_dirty, &victim_idx);
        if (evict_s != Status::Ok) return Status::Full;
        if (victim_dirty) {
          Node* leaf = find_leaf_for_key(root_, victim_key);
          evict_leaf_if_needed(leaf);
          leaf->cache->upsert(victim_key, victim_val);
        }
        root_->cache->evict_slot(victim_idx, victim_key);
        // Loop back to retry upsert
      }
      return Status::Full;
    }

    // Key NOT in root cache.  Decide whether to insert here (P_parent).
    bool use_parent = false;
    if (p_parent_ >= 1.0) {
      use_parent = true;
    } else if (p_parent_ > 0.0) {
      use_parent = std::bernoulli_distribution{p_parent_}(rng);
    }

    if (use_parent) {
      // Try to insert into root cache (retry on Full).
      constexpr int kMaxParentRetries = 64;
      for (int retry = 0; retry < kMaxParentRetries; ++retry) {
        Status s = root_->cache->upsert(k, v);
        if (s == Status::Ok) {
          evict_parent_if_needed(root_);
          return Status::Ok;
        }

        if (s != Status::Full) return s;

        // Evict a victim from root cache and demote dirty data to leaf.
        Key victim_key = 0;
        Value victim_val = 0;
        bool victim_dirty = false;
        int victim_idx = -1;
        Status evict_s = root_->cache->find_clock_victim(&victim_key, &victim_val,
                                                         &victim_dirty, &victim_idx);
        if (evict_s != Status::Ok) return Status::Full;
        if (victim_dirty) {
          Node* leaf = find_leaf_for_key(root_, victim_key);
          evict_leaf_if_needed(leaf);
          leaf->cache->upsert(victim_key, victim_val);
        }
        root_->cache->evict_slot(victim_idx, victim_key);
        // Loop back to retry upsert
      }
      return Status::Full;
    }
    // P_parent not selected -- fall through to leaf
  }

  // ---- Phase 2: descend to leaf ----
  std::vector<std::pair<Node*, uint64_t>> versions;
  Node* leaf = descend_to_leaf(k, versions);

  // ---- Phase 3: leaf cache upsert (retry on Full) ----
  constexpr int kMaxUpsertRetries = 64;
  for (int retry = 0; retry < kMaxUpsertRetries; ++retry) {
    Status s = leaf->cache->upsert(k, v);
    if (s == Status::Ok) {
      evict_leaf_if_needed(leaf);
      return Status::Ok;
    }

    if (s != Status::Full) return s;

    // Cache is full -- evict one slot via CLOCK and retry
    Key victim_key = 0;
    Value victim_val = 0;
    bool victim_dirty = false;
    int victim_idx = -1;

    Status evict_s = leaf->cache->find_clock_victim(&victim_key, &victim_val,
                                                    &victim_dirty, &victim_idx);
    if (evict_s != Status::Ok) {
      return Status::Full;  // nothing to evict (should not happen)
    }

    // Write dirty victim to SSD BEFORE clearing the slot
    if (victim_dirty) {
      Status write_s = ssd_->put_record(leaf->page_id, victim_key, victim_val);
      if (write_s != Status::Ok) {
        return Status::Error;
      }
      register_in_leaf_index(leaf, victim_key);
    }
    // Now clear the slot (verified against victim_key)
    leaf->cache->evict_slot(victim_idx, victim_key);
    // Loop back to retry upsert
  }
  return Status::Full;
}

void Tree::collect_leaves_in_range(Node* node, Key lo, Key hi,
                                    std::vector<Node*>& leaves) {
  if (!node) return;
  if (node->height == 1) {
    leaves.push_back(node);
    return;
  }

  // Determine which children overlap [lo, hi]
  // separator[i] partitions children[i] (< s[i]) and children[i+1] (>= s[i])
  // upper_bound(k) returns first separator > k, which is the child index for k
  size_t lo_idx =
      std::upper_bound(node->separators.begin(), node->separators.end(), lo) -
      node->separators.begin();
  size_t hi_idx =
      std::upper_bound(node->separators.begin(), node->separators.end(), hi) -
      node->separators.begin();

  for (size_t i = lo_idx; i <= hi_idx && i < node->children.size(); ++i) {
    collect_leaves_in_range(node->children[i], lo, hi, leaves);
  }
}

std::vector<std::pair<Key, Value>> Tree::scan(Key lo, Key hi) {
  // 1. Find leaves overlapping [lo, hi]
  std::vector<Node*> leaves;
  collect_leaves_in_range(root_, lo, hi, leaves);

  if (leaves.empty()) return {};

  // 2. Build result map: key -> (value, authority)
  //    authority: 0 = parent cache (highest), 1 = leaf cache, 2 = SSD (lowest)
  //    A negative authority (-1) means the key was marked ABSENT by a cache.
  struct AuthEntry {
    Value value;
    int authority;  // -1 = absent, 0 = parent, 1 = leaf, 2 = SSD
  };
  std::map<Key, AuthEntry> merged;

  std::set<CacheAttachment*> processed_parents;

  for (Node* leaf : leaves) {
    // ---- Parent cache (authority 0, highest) ----
    if (leaf->parent && leaf->parent->height == 2 && leaf->parent->cache) {
      CacheAttachment* pc = leaf->parent->cache.get();
      if (processed_parents.insert(pc).second) {
        // Ensure sorted for consistent iteration
        if (!pc->sorted_flag()) pc->sort_and_set_flag();
        // Occupied entries
        auto occ = pc->occupied_sorted();
        for (const auto& [k, v] : occ) {
          if (k >= lo && k <= hi) {
            merged[k] = {v, 0};
          }
        }
        // Absent entries suppress lower levels
        auto abs = pc->absent_keys();
        for (Key k : abs) {
          if (k >= lo && k <= hi) {
            merged[k] = {0, -1};
          }
        }
      }
    }

    // ---- Leaf cache (authority 1) ----
    CacheAttachment* lc = leaf->cache.get();
    if (!lc->sorted_flag()) lc->sort_and_set_flag();
    auto occ = lc->occupied_sorted();
    for (const auto& [k, v] : occ) {
      if (k >= lo && k <= hi) {
        auto it = merged.find(k);
        if (it == merged.end()) {
          merged[k] = {v, 1};
        }
        // If parent cache has it (authority 0 or -1), keep parent's result
      }
    }
    auto abs = lc->absent_keys();
    for (Key k : abs) {
      if (k >= lo && k <= hi) {
        auto it = merged.find(k);
        if (it == merged.end()) {
          merged[k] = {0, -1};
        }
      }
    }

    // ---- SSD (authority 2, lowest) ----
    std::vector<std::pair<Key, Value>> page_data;
    ssd_->dump_sorted(leaf->page_id, &page_data);
    for (const auto& [k, v] : page_data) {
      if (k >= lo && k <= hi) {
        auto it = merged.find(k);
        if (it == merged.end()) {
          merged[k] = {v, 2};
        }
      }
    }
  }

  // 3. Build result: iterate sorted map, skip absent entries
  std::vector<std::pair<Key, Value>> result;
  for (const auto& [k, entry] : merged) {
    if (entry.authority >= 0) {  // not absent
      result.emplace_back(k, entry.value);
    }
  }

  return result;
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
    // Only height <= 2 nodes have caches.
    if (root_->height >= 2 && root_->cache) {
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
  if (root_->height < 2 || !root_->cache) return false;
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

void Tree::collect_leaves(Node* node, std::vector<Node*>& leaves) {
  if (!node) return;
  if (node->height == 1) {
    leaves.push_back(node);
    return;
  }
  for (Node* child : node->children) {
    collect_leaves(child, leaves);
  }
}

Status Tree::debug_flush_all() {
  std::vector<Node*> leaves;
  collect_leaves(root_, leaves);

  for (Node* leaf : leaves) {
    std::lock_guard<std::mutex> lock(leaf->leaf_index_mutex);

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

  // 2. Pick mid from leaf_keys
  Key mid = leaf->leaf_keys[leaf->leaf_keys.size() / 2];

  // 3. Split SSD data (must succeed before we mutate tree structure)
  PageId new_right_id = 0;
  Status split_s = ssd_->split_page(leaf->page_id, mid, &new_right_id);
  if (split_s != Status::Ok) {
    // Revert version and abort — disk failure leaves tree unchanged
    leaf->version.fetch_add(1, std::memory_order_acq_rel);
    return;
  }

  // 4. Create right leaf node
  Node* L_right = new Node{};
  L_right->height = 1;
  L_right->cache = std::make_unique<CacheAttachment>();
  L_right->page_id = new_right_id;

  // 5. Split leaf index (leaf_keys, leaf_page_ids)
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

  // 6. Split cache entries
  leaf->cache->split_into(mid, L_right->cache.get());

  // 7. Update parent
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

  // 8. Set version back to even (stable)
  leaf->version.fetch_add(1, std::memory_order_acq_rel);

  // 9. If parent overflows, recursively split
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

  // 3. Cache handling for height >= 3: push down to children if present (safety net)
  if (node->height >= 3 && node->cache) {
    // Push occupied entries down to children's caches
    if (!node->cache->sorted_flag()) node->cache->sort_and_set_flag();
    auto occ = node->cache->occupied_sorted();
    for (const auto& [k, v] : occ) {
      auto it = std::upper_bound(node->separators.begin(), node->separators.end(), k);
      size_t child_idx = static_cast<size_t>(it - node->separators.begin());
      Node* child = node->children[child_idx];
      if (child->cache) {
        child->cache->upsert(k, v);
      }
    }
    // Push absent entries down to children's caches
    auto abs = node->cache->absent_keys();
    for (Key k : abs) {
      auto it = std::upper_bound(node->separators.begin(), node->separators.end(), k);
      size_t child_idx = static_cast<size_t>(it - node->separators.begin());
      Node* child = node->children[child_idx];
      if (child->cache) {
        child->cache->mark_absent(k);
      }
    }
    node->cache = nullptr;
  }

  // 4. Create new right internal node
  Node* new_node = new Node{};
  new_node->height = node->height;
  if (new_node->height == 2) {
    new_node->cache = std::make_unique<CacheAttachment>();
  }
  // height >= 3: new_node->cache stays nullptr (default unique_ptr)

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

  // 5. Split cache for height == 2 (both keep cache, distribute entries by mid)
  if (node->height == 2) {
    node->cache->split_into(mid, new_node->cache.get());
  }

  // 6. Update parent
  if (node == root_) {
    Node* new_root = new Node{};
    new_root->height = node->height + 1;
    // Only height 1 and 2 nodes have caches
    if (new_root->height < 3) {
      new_root->cache = std::make_unique<CacheAttachment>();
    }
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

  // 7. Set version back to even
  node->version.fetch_add(1, std::memory_order_acq_rel);

  // 8. Recursively split parent if overflow
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

bool Tree::debug_height3_nodes_have_no_cache() const {
  std::vector<const Node*> stack;
  stack.push_back(root_);
  while (!stack.empty()) {
    const Node* node = stack.back();
    stack.pop_back();
    if (!node) continue;
    if (node->height >= 3 && node->cache) return false;
    for (Node* child : node->children) {
      stack.push_back(child);
    }
  }
  return true;
}

void Tree::debug_clear_all_caches() {
  std::vector<Node*> leaves;
  collect_leaves(root_, leaves);
  for (Node* leaf : leaves) {
    if (leaf->cache) leaf->cache->clear();
  }
  if (root_->cache) root_->cache->clear();
}

bool Tree::debug_leaf_index_empty() const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (!leaf->leaf_keys.empty()) return false;
  }
  return true;
}

bool Tree::debug_some_keys_in_leaf_cache() const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (leaf->cache && leaf->cache->occupied_count() > 0) return true;
  }
  return false;
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
