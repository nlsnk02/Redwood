// src/tree.cpp
#include "cbtree/tree.hpp"
#include "cbtree/cache_attachment.hpp"
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

Status Tree::put(Key k, Value v) {
  // Height-1 minimum version (single-threaded).
  // upsert() internally acquires the per-key lock via KeyLockTable.

  Status s = root_->cache->upsert(k, v);
  if (s == Status::Ok) {
    // Do NOT update leaf_keys (lazy index -- populated during flush)
    return Status::Ok;
  }

  // Cache is full -- evict one slot via CLOCK and retry
  if (s == Status::Full) {
    Key victim_key = 0;
    Value victim_val = 0;
    bool victim_dirty = false;

    Status evict_s = root_->cache->pick_clock_victim(&victim_key, &victim_val,
                                                      &victim_dirty);
    if (evict_s != Status::Ok) {
      return Status::Full;  // nothing to evict (should not happen)
    }

    // Write dirty victim to SSD (single-leaf, height=1)
    if (victim_dirty) {
      Status write_s = ssd_->put_record(root_->page_id, victim_key, victim_val);
      if (write_s != Status::Ok) {
        return Status::Error;
      }
    }

    // Retry upsert now that a slot is free
    // Do NOT update leaf_keys (lazy index)
    s = root_->cache->upsert(k, v);
    if (s == Status::Ok) {
      return Status::Ok;
    }
    return s;
  }

  return s;
}

LookupResult Tree::get(Key k) {
  constexpr int kMaxRetries = 64;

  // Per-call random engine for placeholder probability
  thread_local std::mt19937_64 rng(std::random_device{}());

  for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
    // Step 1: read version before lookup (must be even)
    uint64_t v = root_->version.load(std::memory_order_acquire);
    if (v & 1) {  // odd = structural change in progress
      continue;   // spin/retry
    }

    // Step 2: cache lookup (top-down, includes fingerprint pre-filter)
    LookupResult r = root_->cache->lookup(k);
    if (r.status == Status::Ok) {
      return r;  // OCCUPIED hit -- return value
    }
    if (root_->cache->has_absent(k)) {
      return {Status::NotFound};  // ABSENT hit -- return NotFound
    }

    // Step 3: cache miss -- try place placeholder with P_placeholder probability
    // At most one placeholder per get (has_placed flag)
    bool has_placed = false;
    int placeholder_idx = -1;
    if (p_placeholder_ >= 1.0) {
      // Deterministic: always place (test hook)
      Status ps = root_->cache->try_place_placeholder(k, &placeholder_idx);
      has_placed = (ps == Status::Ok);
    } else if (p_placeholder_ > 0.0) {
      if (std::bernoulli_distribution{p_placeholder_}(rng)) {
        Status ps = root_->cache->try_place_placeholder(k, &placeholder_idx);
        has_placed = (ps == Status::Ok);
      }
    }

    // Step 4: query SSD
    r = ssd_->get_record(root_->page_id, k);

    // Step 5: post-SSD secondary cache recheck
    LookupResult r2 = root_->cache->lookup(k);
    if (r2.status == Status::Ok) {
      // Cache hit after SSD read -- prefer cache
      // If we placed a placeholder, fill it with the cached value
      if (has_placed) {
        root_->cache->fill_placeholder(placeholder_idx, r2.value);
      }
      // Version check -- only return if stable
      if (root_->version.load(std::memory_order_acquire) == v) {
        return r2;
      }
      continue;  // version changed, retry
    }

    // Step 6: fill placeholder based on SSD result
    if (has_placed) {
      if (r.status == Status::Ok) {
        root_->cache->fill_placeholder(placeholder_idx, r.value);
      } else {
        root_->cache->fill_placeholder_absent(placeholder_idx);
      }
    }

    // Step 7: version check after read (SSD or post-SSD cache paths)
    if (root_->version.load(std::memory_order_acquire) == v) {
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

}  // namespace cbtree
