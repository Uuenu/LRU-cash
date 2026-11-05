# LRU cache (C++17, header-only)

`include/lru_cache.hpp`:

- `lru::LRUCache<K, V, Hash, Eq>`: single-threaded, O(1) `get` / `put` / `erase` (hash map + doubly linked list).
- `lru::ThreadSafeLRUCache<K, V, Hash, Eq>`: the same API behind a mutex, plus an atomic `get_or_put`.

```cpp
#include "lru_cache.hpp"

lru::LRUCache<int, std::string> cache(1000);
cache.put(1, "one");
if (auto v = cache.get(1)) use(*v);   // get() promotes to most-recently-used
cache.peek(1);                         // read without promoting
cache.erase(1);
```

## Guarantees

- `put()` gives the **strong exception guarantee**: if hashing, allocation, or copying K/V throws, the cache is left unchanged.
- The optional eviction callback `(const K&, V&&)` runs only after the cache is consistent again, so a callback that throws cannot corrupt it.
- `get()` returns a copy, so no references or iterators into the cache ever leak out.
- Capacity 0 is rejected (`std::invalid_argument`). Calling `set_capacity()` with a smaller value evicts LRU entries.
- Copy (deep, keeps order), move, and swap are supported.

## Build / test / bench

```sh
make test        # functional + randomized differential + concurrency tests
make test-asan   # under AddressSanitizer + UBSan
make test-tsan   # under ThreadSanitizer
make bench
```

## Benchmark (Apple M3 Pro, clang 21, -O3)

Read-through workload: `get`, then `put` on a miss. The key universe is 10× the capacity.

| implementation          | workload    | capacity | ns/op | Mops/s | hits  |
|-------------------------|-------------|---------:|------:|-------:|------:|
| lru::LRUCache           | zipf s=0.99 |    1,000 |  33.3 |  30.0  | 66.4% |
| lru::ThreadSafeLRUCache | zipf s=0.99 |    1,000 |  39.1 |  25.6  | 66.4% |
| naive (list::remove)    | zipf s=0.99 |    1,000 | 749.0 |   1.3  | 66.4% |
| lru::LRUCache           | uniform     |    1,000 |  62.4 |  16.0  | 10.0% |
| lru::LRUCache           | zipf s=0.99 |  100,000 |  42.3 |  23.7  | 76.5% |
| lru::LRUCache           | uniform     |  100,000 | 119.4 |   8.4  | 10.1% |
| lru::LRUCache           | zipf s=0.99 |1,000,000 |  30.2 |  33.1  | 71.1% |
| lru::LRUCache           | uniform     |1,000,000 | 173.2 |   5.8  |  8.1% |

With a single global mutex, `ThreadSafeLRUCache` stays correct under contention but doesn't scale: total throughput goes from 24 Mops/s on 1 thread to 2.8 Mops/s on 12. For many-core read-heavy workloads, shard the cache across N independent caches by key hash.
