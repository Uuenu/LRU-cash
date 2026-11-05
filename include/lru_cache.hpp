// lru_cache.hpp — header-only LRU cache, C++17.
//
//   lru::LRUCache<K, V>            single-threaded, O(1) get / put / erase
//   lru::ThreadSafeLRUCache<K, V>  same API, guarded by a mutex
//
// Guarantees
//   * All operations are O(1) average (hash map + doubly linked list).
//   * put() gives the strong exception guarantee: if hashing, allocation, or
//     copying/moving K/V throws, the cache is left unchanged.
//   * Eviction callbacks run after the cache is back in a consistent state,
//     so a throwing callback cannot corrupt the cache.
//   * Iterators/references are never handed out across calls, so callers
//     cannot hold dangling pointers into the cache. get() returns a copy.
#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace lru {

template <class Key, class Value, class Hash = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>>
class LRUCache {
public:
    using key_type = Key;
    using mapped_type = Value;
    using size_type = std::size_t;
    // Called with (key, value) of each entry evicted because of capacity.
    // Not called for erase() / clear() / overwrite by put().
    using EvictionCallback = std::function<void(const Key&, Value&&)>;

    explicit LRUCache(size_type capacity, EvictionCallback on_evict = {})
        : capacity_(check_capacity(capacity)), on_evict_(std::move(on_evict)) {
        index_.reserve(capacity_ + 1);  // +1: put() briefly holds cap+1 entries
    }

    LRUCache(const LRUCache& other)
        : capacity_(other.capacity_), on_evict_(other.on_evict_) {
        index_.reserve(capacity_ + 1);
        for (const auto& entry : other.items_) {
            items_.push_back(entry);
            index_.emplace(items_.back().first, std::prev(items_.end()));
        }
    }
    LRUCache& operator=(const LRUCache& other) {
        if (this != &other) {
            LRUCache tmp(other);  // copy-and-swap: strong guarantee
            swap(tmp);
        }
        return *this;
    }
    // std::list::splice/move keeps iterators valid, so moving is safe.
    LRUCache(LRUCache&&) = default;
    LRUCache& operator=(LRUCache&&) = default;
    ~LRUCache() = default;

    void swap(LRUCache& other) noexcept {
        using std::swap;
        swap(capacity_, other.capacity_);
        swap(on_evict_, other.on_evict_);
        items_.swap(other.items_);
        index_.swap(other.index_);
    }

    // Insert or overwrite. The entry becomes most-recently-used.
    // Returns true if a new key was inserted, false if an existing one was updated.
    template <class V>
    bool put(const Key& key, V&& value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = std::forward<V>(value);  // may throw: nothing changed yet
            touch(it->second);
            return false;  // updated
        }
        // Allocate the node first, then index it; roll back if indexing throws.
        items_.emplace_front(key, std::forward<V>(value));
        try {
            index_.emplace(items_.front().first, items_.begin());
        } catch (...) {
            items_.pop_front();
            throw;
        }
        if (items_.size() > capacity_) evict_one();
        return true;
    }

    // Returns a copy of the value and marks the entry most-recently-used.
    std::optional<Value> get(const Key& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;
        touch(it->second);
        return it->second->second;
    }

    // Like get() but writes into `out`, avoiding the optional wrapper.
    bool get(const Key& key, Value& out) {
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        touch(it->second);
        out = it->second->second;
        return true;
    }

    // Read without changing recency.
    std::optional<Value> peek(const Key& key) const {
        auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;
        return it->second->second;
    }

    bool contains(const Key& key) const { return index_.find(key) != index_.end(); }

    bool erase(const Key& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        auto node = it->second;
        index_.erase(it);
        items_.erase(node);
        return true;
    }

    void clear() noexcept {
        index_.clear();
        items_.clear();
    }

    // Shrinking evicts least-recently-used entries (invoking the callback).
    void set_capacity(size_type capacity) {
        capacity_ = check_capacity(capacity);
        while (items_.size() > capacity_) evict_one();
        index_.reserve(capacity_ + 1);
    }

    size_type size() const noexcept { return items_.size(); }
    size_type capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return items_.empty(); }

    // Visit entries from most- to least-recently-used without changing order.
    template <class F>
    void for_each(F&& f) const {
        for (const auto& entry : items_) f(entry.first, entry.second);
    }

    // Least-recently-used key (next to be evicted), if any.
    std::optional<Key> lru_key() const {
        if (items_.empty()) return std::nullopt;
        return items_.back().first;
    }

private:
    using Entry = std::pair<Key, Value>;
    using List = std::list<Entry>;
    using ListIt = typename List::iterator;

    static size_type check_capacity(size_type c) {
        if (c == 0) throw std::invalid_argument("LRUCache capacity must be > 0");
        return c;
    }

    void touch(ListIt node) noexcept {
        if (node != items_.begin()) items_.splice(items_.begin(), items_, node);
    }

    void evict_one() {
        // Detach the tail first so the cache is consistent before user code runs.
        List victim;
        victim.splice(victim.begin(), items_, std::prev(items_.end()));
        index_.erase(victim.front().first);
        if (on_evict_) on_evict_(victim.front().first, std::move(victim.front().second));
    }

    size_type capacity_;
    EvictionCallback on_evict_;
    List items_;  // front = most recently used
    std::unordered_map<Key, ListIt, Hash, KeyEqual> index_;
};

template <class K, class V, class H, class E>
void swap(LRUCache<K, V, H, E>& a, LRUCache<K, V, H, E>& b) noexcept {
    a.swap(b);
}

// Every operation (including get, which reorders) mutates state, so a plain
// mutex is the right tool; a shared_mutex would buy nothing.
// The eviction callback runs while the lock is held: it must not call back
// into the same cache.
template <class Key, class Value, class Hash = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>>
class ThreadSafeLRUCache {
    using Inner = LRUCache<Key, Value, Hash, KeyEqual>;

public:
    using size_type = typename Inner::size_type;
    using EvictionCallback = typename Inner::EvictionCallback;

    explicit ThreadSafeLRUCache(size_type capacity, EvictionCallback on_evict = {})
        : cache_(capacity, std::move(on_evict)) {}

    ThreadSafeLRUCache(const ThreadSafeLRUCache&) = delete;
    ThreadSafeLRUCache& operator=(const ThreadSafeLRUCache&) = delete;

    template <class V>
    bool put(const Key& key, V&& value) {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.put(key, std::forward<V>(value));
    }
    std::optional<Value> get(const Key& key) {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.get(key);
    }
    bool get(const Key& key, Value& out) {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.get(key, out);
    }
    std::optional<Value> peek(const Key& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.peek(key);
    }
    bool contains(const Key& key) const {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.contains(key);
    }
    bool erase(const Key& key) {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.erase(key);
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.clear();
    }
    void set_capacity(size_type c) {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.set_capacity(c);
    }
    size_type size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.size();
    }
    size_type capacity() const {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.capacity();
    }

    // Atomic get-or-compute. `make` runs under the lock; keep it cheap.
    template <class F>
    Value get_or_put(const Key& key, F&& make) {
        std::lock_guard<std::mutex> lock(mu_);
        if (auto hit = cache_.get(key)) return std::move(*hit);
        Value v = std::forward<F>(make)();
        cache_.put(key, v);
        return v;
    }

private:
    mutable std::mutex mu_;
    Inner cache_;
};

}  // namespace lru
