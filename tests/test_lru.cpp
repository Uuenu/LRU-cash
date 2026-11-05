#include "lru_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ---- tiny test harness ------------------------------------------------------
static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)
#define CHECK_THROWS(expr, Ex)                 \
    do {                                       \
        bool thrown_ = false;                  \
        try { expr; } catch (const Ex&) { thrown_ = true; } \
        CHECK(thrown_ && #expr " throws " #Ex); \
    } while (0)

struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
struct Reg { Reg(const char* n, void (*f)()) { registry().push_back({n, f}); } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

using lru::LRUCache;
using lru::ThreadSafeLRUCache;

// Helper: keys in MRU -> LRU order.
template <class C>
static std::vector<int> order(const C& c) {
    std::vector<int> out;
    c.for_each([&](const int& k, const auto&) { out.push_back(k); });
    return out;
}

// ---- functional tests -------------------------------------------------------
TEST(basic_put_get) {
    LRUCache<int, std::string> c(2);
    CHECK(c.empty());
    CHECK(c.put(1, "one"));
    CHECK(c.put(2, "two"));
    CHECK(c.size() == 2);
    CHECK(c.get(1) == std::string("one"));
    CHECK(c.get(2) == std::string("two"));
    CHECK(!c.get(3).has_value());
}

TEST(evicts_least_recently_used) {
    LRUCache<int, int> c(2);
    c.put(1, 10);
    c.put(2, 20);
    c.get(1);        // 1 is now MRU, 2 is LRU
    c.put(3, 30);    // evicts 2
    CHECK(!c.contains(2));
    CHECK(c.contains(1));
    CHECK(c.contains(3));
    CHECK(c.size() == 2);
}

TEST(update_existing_key_promotes_and_does_not_grow) {
    LRUCache<int, int> c(2);
    c.put(1, 10);
    c.put(2, 20);
    CHECK(!c.put(1, 11));  // update returns false
    CHECK(c.size() == 2);
    c.put(3, 30);          // 2 is LRU now
    CHECK(!c.contains(2));
    CHECK(c.get(1) == 11);
}

TEST(peek_and_contains_do_not_promote) {
    LRUCache<int, int> c(2);
    c.put(1, 10);
    c.put(2, 20);
    CHECK(c.peek(1) == 10);
    CHECK(c.contains(1));
    c.put(3, 30);  // 1 still LRU -> evicted
    CHECK(!c.contains(1));
    CHECK(!c.peek(1).has_value());
}

TEST(get_out_param_overload) {
    LRUCache<int, int> c(1);
    int v = -1;
    CHECK(!c.get(5, v));
    CHECK(v == -1);
    c.put(5, 50);
    CHECK(c.get(5, v));
    CHECK(v == 50);
}

TEST(erase_and_clear) {
    LRUCache<int, int> c(3);
    c.put(1, 1); c.put(2, 2); c.put(3, 3);
    CHECK(c.erase(2));
    CHECK(!c.erase(2));
    CHECK(c.size() == 2);
    CHECK((order(c) == std::vector<int>{3, 1}));
    c.clear();
    CHECK(c.empty());
    CHECK(!c.lru_key().has_value());
    c.put(4, 4);  // usable after clear
    CHECK(c.get(4) == 4);
}

TEST(capacity_one) {
    LRUCache<int, int> c(1);
    c.put(1, 1);
    c.put(2, 2);
    CHECK(!c.contains(1));
    CHECK(c.get(2) == 2);
    c.put(2, 3);
    CHECK(c.size() == 1);
    CHECK(c.get(2) == 3);
}

TEST(zero_capacity_rejected) {
    CHECK_THROWS((LRUCache<int, int>(0)), std::invalid_argument);
    LRUCache<int, int> c(1);
    CHECK_THROWS(c.set_capacity(0), std::invalid_argument);
    CHECK(c.capacity() == 1);
}

TEST(order_tracking) {
    LRUCache<int, int> c(4);
    for (int i = 1; i <= 4; ++i) c.put(i, i);
    CHECK((order(c) == std::vector<int>{4, 3, 2, 1}));
    c.get(2);
    CHECK((order(c) == std::vector<int>{2, 4, 3, 1}));
    c.get(1);
    CHECK((order(c) == std::vector<int>{1, 2, 4, 3}));
    CHECK(c.lru_key() == 3);
}

TEST(eviction_callback) {
    std::vector<std::pair<int, std::string>> evicted;
    LRUCache<int, std::string> c(2, [&](const int& k, std::string&& v) {
        evicted.emplace_back(k, std::move(v));
    });
    c.put(1, "a");
    c.put(2, "b");
    c.put(1, "a2");  // overwrite: no callback
    c.erase(2);      // erase: no callback
    CHECK(evicted.empty());
    c.put(3, "c");
    c.put(4, "d");   // evicts 1
    CHECK(evicted.size() == 1);
    CHECK(evicted[0].first == 1 && evicted[0].second == "a2");
}

TEST(throwing_eviction_callback_leaves_cache_consistent) {
    LRUCache<int, int> c(2, [](const int&, int&&) { throw std::runtime_error("boom"); });
    c.put(1, 1);
    c.put(2, 2);
    CHECK_THROWS(c.put(3, 3), std::runtime_error);
    // 3 was inserted, 1 was evicted before the callback threw.
    CHECK(c.size() == 2);
    CHECK(c.contains(3) && c.contains(2) && !c.contains(1));
}

TEST(set_capacity_shrink_and_grow) {
    std::vector<int> evicted;
    LRUCache<int, int> c(5, [&](const int& k, int&&) { evicted.push_back(k); });
    for (int i = 1; i <= 5; ++i) c.put(i, i);
    c.set_capacity(2);
    CHECK(c.size() == 2);
    CHECK((evicted == std::vector<int>{1, 2, 3}));
    CHECK((order(c) == std::vector<int>{5, 4}));
    c.set_capacity(10);
    for (int i = 6; i <= 13; ++i) c.put(i, i);
    CHECK(c.size() == 10);
}

TEST(copy_and_move) {
    LRUCache<int, std::string> a(3);
    a.put(1, "x"); a.put(2, "y"); a.put(3, "z");
    a.get(1);

    LRUCache<int, std::string> b(a);  // deep copy, same order
    CHECK(order(b) == order(a));
    b.put(4, "w");  // must not affect a
    CHECK(a.contains(2) && !b.contains(2));

    LRUCache<int, std::string> m(std::move(b));
    CHECK((order(m) == std::vector<int>{4, 1, 3}));
    m.get(3);  // iterators must still be valid after move
    CHECK((order(m) == std::vector<int>{3, 4, 1}));

    LRUCache<int, std::string> d(1);
    d = a;
    CHECK(d.capacity() == 3 && order(d) == order(a));
    d = std::move(m);
    CHECK((order(d) == std::vector<int>{3, 4, 1}));
    d.put(5, "v");
    CHECK(!d.contains(1));
}

struct Counted {
    static inline int copies = 0;
    int v = 0;
    Counted() = default;
    explicit Counted(int x) : v(x) {}
    Counted(const Counted& o) : v(o.v) { ++copies; }
    Counted(Counted&&) noexcept = default;
    Counted& operator=(const Counted& o) { v = o.v; ++copies; return *this; }
    Counted& operator=(Counted&&) noexcept = default;
};

TEST(move_only_insertion_value) {
    // Values are moved in, not copied, on put(key, rvalue).
    LRUCache<int, Counted> c(2);
    c.put(1, Counted(1));
    c.put(1, Counted(2));
    CHECK(Counted::copies == 0);
}

TEST(custom_hash_and_equality) {
    struct CaseInsensitiveHash {
        size_t operator()(const std::string& s) const {
            size_t h = 0;
            for (char ch : s) h = h * 31 + static_cast<size_t>(std::tolower(ch));
            return h;
        }
    };
    struct CaseInsensitiveEq {
        bool operator()(const std::string& a, const std::string& b) const {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(x) == std::tolower(y);
                   });
        }
    };
    LRUCache<std::string, int, CaseInsensitiveHash, CaseInsensitiveEq> c(4);
    c.put("Hello", 1);
    CHECK(c.get("HELLO") == 1);
    CHECK(!c.put("hello", 2));
    CHECK(c.size() == 1);
}

// ---- exception safety -------------------------------------------------------
struct ThrowConfig { static inline int countdown = -1; };  // throw when hits 0
struct ThrowingValue {
    int v = 0;
    ThrowingValue() = default;
    explicit ThrowingValue(int x) : v(x) {}
    ThrowingValue(const ThrowingValue& o) : v(o.v) { maybe_throw(); }
    ThrowingValue& operator=(const ThrowingValue& o) { maybe_throw(); v = o.v; return *this; }
    static void maybe_throw() {
        if (ThrowConfig::countdown >= 0 && ThrowConfig::countdown-- == 0)
            throw std::runtime_error("copy failed");
    }
};

TEST(strong_guarantee_on_value_copy_failure) {
    LRUCache<int, ThrowingValue> c(2);
    ThrowingValue a(1), b(2), x(99);
    c.put(1, a);
    c.put(2, b);
    auto before = order(c);

    ThrowConfig::countdown = 0;  // next copy throws: new-key insert
    CHECK_THROWS(c.put(3, x), std::runtime_error);
    CHECK(order(c) == before);
    CHECK(c.size() == 2);

    ThrowConfig::countdown = 0;  // overwrite of existing key
    CHECK_THROWS(c.put(1, x), std::runtime_error);
    CHECK(order(c) == before);
    CHECK(c.peek(1)->v == 1);
    ThrowConfig::countdown = -1;
}

struct ThrowingHash {
    static inline int countdown = -1;  // throw when it reaches 0
    size_t operator()(int k) const {
        if (countdown >= 0 && countdown-- == 0) throw std::runtime_error("hash failed");
        return std::hash<int>{}(k);
    }
};

TEST(strong_guarantee_on_hash_failure) {
    LRUCache<int, int, ThrowingHash> c(2);
    c.put(1, 1);
    c.put(2, 2);
    // 1st hash: find() at the start of put -> nothing touched yet.
    ThrowingHash::countdown = 0;
    CHECK_THROWS(c.put(3, 3), std::runtime_error);
    CHECK((order(c) == std::vector<int>{2, 1}));
    // 2nd hash: inside index_.emplace(), after the list node was created ->
    // exercises the rollback path.
    ThrowingHash::countdown = 1;
    CHECK_THROWS(c.put(3, 3), std::runtime_error);
    ThrowingHash::countdown = -1;
    CHECK(!c.contains(3));
    CHECK(c.size() == 2);
    CHECK((order(c) == std::vector<int>{2, 1}));
    CHECK(c.get(1) == 1 && c.get(2) == 2);
}

// ---- randomized differential test vs. a trivially-correct model -------------
struct ReferenceLRU {
    size_t cap;
    std::deque<std::pair<int, int>> q;  // front = MRU
    explicit ReferenceLRU(size_t c) : cap(c) {}
    auto find(int k) {
        return std::find_if(q.begin(), q.end(), [&](auto& p) { return p.first == k; });
    }
    std::optional<int> get(int k) {
        auto it = find(k);
        if (it == q.end()) return std::nullopt;
        auto p = *it; q.erase(it); q.push_front(p);
        return p.second;
    }
    void put(int k, int v) {
        auto it = find(k);
        if (it != q.end()) q.erase(it);
        q.push_front({k, v});
        if (q.size() > cap) q.pop_back();
    }
    bool erase(int k) {
        auto it = find(k);
        if (it == q.end()) return false;
        q.erase(it);
        return true;
    }
    std::vector<int> keys() const {
        std::vector<int> out;
        for (auto& p : q) out.push_back(p.first);
        return out;
    }
};

TEST(randomized_against_reference_model) {
    std::mt19937 rng(12345);
    for (size_t cap : {1u, 2u, 3u, 7u, 16u, 64u}) {
        LRUCache<int, int> c(cap);
        ReferenceLRU ref(cap);
        std::uniform_int_distribution<int> key(0, static_cast<int>(cap * 3));
        std::uniform_int_distribution<int> op(0, 99);
        bool ok = true;
        for (int i = 0; i < 20000 && ok; ++i) {
            int k = key(rng), o = op(rng);
            if (o < 45) { c.put(k, i); ref.put(k, i); }
            else if (o < 90) { ok = c.get(k) == ref.get(k); }
            else if (o < 98) { ok = c.erase(k) == ref.erase(k); }
            else { ok = c.peek(k) == [&]() -> std::optional<int> {
                       auto it = ref.find(k);
                       return it == ref.q.end() ? std::nullopt : std::optional<int>(it->second);
                   }(); }
            ok = ok && c.size() == ref.q.size() && order(c) == ref.keys();
        }
        CHECK(ok);
    }
}

// ---- concurrency ------------------------------------------------------------
TEST(thread_safe_stress) {
    ThreadSafeLRUCache<int, int> c(128);
    const int threads = 8, iters = 50000;
    std::atomic<bool> bad{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            std::mt19937 rng(t);
            std::uniform_int_distribution<int> key(0, 511);
            for (int i = 0; i < iters; ++i) {
                int k = key(rng);
                switch (i % 4) {
                    case 0: c.put(k, k * 2); break;
                    case 1: if (auto v = c.get(k); v && *v != k * 2) bad = true; break;
                    case 2: if (auto v = c.peek(k); v && *v != k * 2) bad = true; break;
                    case 3: if (i % 64 == 3) c.erase(k); break;
                }
                if (c.size() > 128) bad = true;
            }
        });
    }
    for (auto& th : ts) th.join();
    CHECK(!bad);
    CHECK(c.size() <= 128);
}

TEST(thread_safe_get_or_put_computes_once_per_key) {
    ThreadSafeLRUCache<int, int> c(1024);
    std::atomic<int> computed{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([&] {
            for (int k = 0; k < 500; ++k)
                if (c.get_or_put(k, [&] { ++computed; return k + 1; }) != k + 1) std::abort();
        });
    for (auto& th : ts) th.join();
    CHECK(computed == 500);
}

// ---- main -------------------------------------------------------------------
int main() {
    for (auto& tc : registry()) {
        int before = g_failures;
        tc.fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", tc.name);
    }
    std::printf("\n%zu tests, %d checks, %d failures\n", registry().size(), g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
