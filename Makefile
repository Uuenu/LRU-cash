CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude
BUILD    := build

.PHONY: all test test-asan test-tsan bench clean

all: $(BUILD)/test_lru $(BUILD)/bench_lru

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_lru: tests/test_lru.cpp include/lru_cache.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -O1 -g $< -o $@ -pthread

$(BUILD)/test_lru_asan: tests/test_lru.cpp include/lru_cache.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $< -o $@ -pthread

$(BUILD)/test_lru_tsan: tests/test_lru.cpp include/lru_cache.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=thread $< -o $@ -pthread

$(BUILD)/bench_lru: bench/bench_lru.cpp include/lru_cache.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG $< -o $@ -pthread

test: $(BUILD)/test_lru
	./$<

test-asan: $(BUILD)/test_lru_asan
	./$<

test-tsan: $(BUILD)/test_lru_tsan
	./$<

bench: $(BUILD)/bench_lru
	./$<

clean:
	rm -rf $(BUILD)
