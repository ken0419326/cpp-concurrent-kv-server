CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread
CPPFLAGS ?= -Isrc
LDFLAGS ?= -pthread

BUILD_DIR := build
COMMON_SRC := src/kv_store.cpp src/protocol.cpp src/thread_pool.cpp
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
EPOLL_TARGET := $(BUILD_DIR)/kv_server_epoll
endif

.PHONY: all clean test integration benchmark benchmark-compare sanitize

all: $(BUILD_DIR)/kv_server $(EPOLL_TARGET) $(BUILD_DIR)/kv_benchmark $(BUILD_DIR)/unit_tests

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/kv_server: src/server.cpp $(COMMON_SRC) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/kv_server_epoll: src/epoll_server.cpp src/kv_store.cpp src/protocol.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/kv_benchmark: src/benchmark.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BUILD_DIR)/unit_tests: tests/unit_tests.cpp $(COMMON_SRC) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

test: all
	./$(BUILD_DIR)/unit_tests
	python3 tests/integration_test.py --server $(BUILD_DIR)/kv_server
ifeq ($(UNAME_S),Linux)
	python3 tests/integration_test.py --server $(BUILD_DIR)/kv_server_epoll
else
	@echo 'SKIP epoll tests: Linux required; run scripts/docker_verify.sh'
endif

benchmark: all
	python3 scripts/run_benchmarks.py

benchmark-compare: all
ifeq ($(UNAME_S),Linux)
	BENCHMARK_CXXFLAGS='$(CXXFLAGS)' CXX='$(CXX)' python3 scripts/compare_servers.py
else
	@echo 'Linux required for benchmark-compare; use native Linux or Docker explicitly' >&2
	@exit 1
endif

sanitize:
	$(MAKE) clean
	$(MAKE) CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-pthread -fsanitize=address,undefined" all
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD_DIR)/unit_tests
	ASAN_OPTIONS=detect_leaks=0 python3 tests/integration_test.py --server $(BUILD_DIR)/kv_server
ifeq ($(UNAME_S),Linux)
	ASAN_OPTIONS=detect_leaks=0 python3 tests/integration_test.py --server $(BUILD_DIR)/kv_server_epoll
else
	@echo 'SKIP epoll sanitizers: Linux required; run scripts/docker_verify.sh'
endif

clean:
	$(RM) -r $(BUILD_DIR)
