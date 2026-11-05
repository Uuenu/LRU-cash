// Benchmark: lru::LRUCache vs a naive LRU, plus mutex-wrapped and multithreaded runs.
#include "lru_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <list>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;

// Naive baseline seen in a lot of code: list + map, but list::remove() on every
// access, which is O(n) in the cache size.
template <class K, class V>
class NaiveLRU {
public:
    explicit NaiveLRU(size_t cap) : cap_(cap) {}
    bool get(const K& k, V& out) {
        auto it = map_.find(k);
        if (it == map_.end()) return false;
        order_.remove(k);
        order_.push_front(k);
        out = it->second;
        return true;
    }
    void put(const K& k, const V& v) {
        if (map_.count(k)) order_.remove(k);
        else if (map_.size() >= cap_) { map_.erase(order_.back()); order_.pop_back(); }
        order_.push_front(k);
        map_[k] = v;
    }
private:
    size_t cap_;
    std::list<K> order_;
    std::unordered_map<K, V> map_;
};

// Zipf(s) key sampler over [0, n) via an inverse CDF table: realistic skewed access.
static std::vector<uint64_t> zipf_keys(size_t n, double s, size_t count, uint32_t seed) {
    std::vector<double> cdf(n);
    double sum = 0;
    for (size_t i = 0; i < n; ++i) cdf[i] = (sum += 1.0 / std::pow(double(i + 1), s));
    for (auto& c : cdf) c /= sum;
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    // Scramble ranks so hot keys are not adjacent integers (fairer to the hash).
    std::vector<uint64_t> perm(n);
    for (size_t i = 0; i < n; ++i) perm[i] = i * 0x9E3779B97F4A7C15ull;
    std::vector<uint64_t> keys(count);
    for (auto& k : keys)
        k = perm[std::lower_bound(cdf.begin(), cdf.end(), u(rng)) - cdf.begin()];
    return keys;
}

static std::vector<uint64_t> uniform_keys(size_t n, size_t count, uint32_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> d(0, n - 1);
    std::vector<uint64_t> keys(count);
    for (auto& k : keys) k = d(rng) * 0x9E3779B97F4A7C15ull;
    return keys;
}

struct Result { double ns_per_op; double hit_rate; };

// Read-through workload: get(); on miss, put(). This is how caches are used.
template <class Cache>
static Result run(Cache& c, const std::vector<uint64_t>& keys) {
    uint64_t hits = 0, sink = 0, v = 0;
    // Warm up on the first 10% of keys (not timed).
    size_t warm = keys.size() / 10;
    for (size_t i = 0; i < warm; ++i)
        if (!c.get(keys[i], v)) c.put(keys[i], keys[i]);
    auto t0 = Clock::now();
    for (size_t i = warm; i < keys.size(); ++i) {
        if (c.get(keys[i], v)) { ++hits; sink += v; }
        else c.put(keys[i], keys[i]);
    }
    auto t1 = Clock::now();
    asm volatile("" : : "r"(sink) : "memory");  // keep `sink` alive
    size_t n = keys.size() - warm;
    return {std::chrono::duration<double, std::nano>(t1 - t0).count() / n, double(hits) / n};
}

template <class Make>
static Result best_of(int reps, const std::vector<uint64_t>& keys, Make make) {
    Result best{1e18, 0};
    for (int r = 0; r < reps; ++r) {
        auto c = make();
        Result res = run(*c, keys);
        if (res.ns_per_op < best.ns_per_op) best = res;
    }
    return best;
}

static void row(const char* impl, const char* workload, size_t cap, Result r) {
    std::printf("| %-26s | %-14s | %9zu | %8.1f | %9.2f | %6.1f%% |\n", impl, workload, cap,
                r.ns_per_op, 1e3 / r.ns_per_op, r.hit_rate * 100);
}

int main() {
    const size_t kOps = 2'000'000;
    std::printf("Single-threaded, read-through (get, put on miss), best of 5\n\n");
    std::printf("| %-26s | %-14s | %9s | %8s | %9s | %7s |\n", "implementation", "workload",
                "capacity", "ns/op", "Mops/s", "hits");
    std::printf("|%s|%s|%s|%s|%s|%s|\n", std::string(28, '-').c_str(),
                std::string(16, '-').c_str(), std::string(11, '-').c_str(),
                std::string(10, '-').c_str(), std::string(11, '-').c_str(),
                std::string(9, '-').c_str());

    for (size_t cap : {1'000ul, 100'000ul, 1'000'000ul}) {
        size_t universe = cap * 10;
        struct W { const char* name; std::vector<uint64_t> keys; };
        std::vector<W> workloads = {
            {"zipf s=0.99", zipf_keys(universe, 0.99, kOps, 1)},
            {"uniform", uniform_keys(universe, kOps, 2)},
        };
        for (auto& w : workloads) {
            using C = lru::LRUCache<uint64_t, uint64_t>;
            using T = lru::ThreadSafeLRUCache<uint64_t, uint64_t>;
            row("lru::LRUCache", w.name, cap,
                best_of(5, w.keys, [&] { return std::make_unique<C>(cap); }));
            row("lru::ThreadSafeLRUCache", w.name, cap,
                best_of(5, w.keys, [&] { return std::make_unique<T>(cap); }));
            if (cap <= 1'000) {
                auto shorter = std::vector<uint64_t>(w.keys.begin(), w.keys.begin() + 200'000);
                row("naive (list::remove)", w.name, cap,
                    best_of(3, shorter, [&] { return std::make_unique<NaiveLRU<uint64_t, uint64_t>>(cap); }));
            }
        }
    }

    // ---- multithreaded: aggregate throughput of ThreadSafeLRUCache ----
    std::printf("\nMulti-threaded ThreadSafeLRUCache, capacity 100000, zipf s=0.99\n\n");
    std::printf("| threads | total Mops/s | ns/op per thread |\n|---------|--------------|------------------|\n");
    const size_t cap = 100'000, per_thread = 1'000'000;
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned threads : {1u, 2u, 4u, 8u, hw}) {
        if (threads > hw) continue;
        std::vector<std::vector<uint64_t>> keys;
        for (unsigned t = 0; t < threads; ++t) keys.push_back(zipf_keys(cap * 10, 0.99, per_thread, 100 + t));
        lru::ThreadSafeLRUCache<uint64_t, uint64_t> c(cap);
        std::vector<std::thread> ts;
        auto t0 = Clock::now();
        for (unsigned t = 0; t < threads; ++t)
            ts.emplace_back([&, t] {
                uint64_t v;
                for (auto k : keys[t]) if (!c.get(k, v)) c.put(k, k);
            });
        for (auto& th : ts) th.join();
        double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        double total = double(threads) * per_thread;
        std::printf("| %7u | %12.2f | %16.1f |\n", threads, total / secs / 1e6, secs * 1e9 / per_thread);
        if (threads == hw) break;
    }
    return 0;
}
