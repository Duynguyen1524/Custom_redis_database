# toyredis -- a Redis-style in-memory key/value server.
#
# Layout:
#   final/     server, client, benchmark (src/include layout)
#   scripts/   memtrack + benchmark runner
#   tests/     functional smoke test
#
# Top-level targets:
#   make          -> build server, client, benchmark
#   make test     -> python smoke test (server must already be running)
#   make bench    -> build then run scripts/run_bench.sh
#   make clean

CXX      ?= g++
# gnu++17 is required: container_of in common.h uses GCC's typeof + statement-exprs
CXXFLAGS ?= -std=gnu++17 -Wall -Wextra -O2 -g
LDFLAGS  := -pthread

BIN := bin

.PHONY: all clean test bench

all: $(BIN)/server $(BIN)/client $(BIN)/benchmark

FINAL_DIR     := final
FINAL_INC     := -I$(FINAL_DIR)/include
FINAL_CORE    := \
    $(FINAL_DIR)/src/core/avl.cpp \
    $(FINAL_DIR)/src/core/hashtable.cpp \
    $(FINAL_DIR)/src/core/heap.cpp \
    $(FINAL_DIR)/src/core/thread_pool.cpp \
    $(FINAL_DIR)/src/core/zset.cpp
FINAL_SERVER  := $(FINAL_DIR)/src/server.cpp
FINAL_CLIENT  := $(FINAL_DIR)/client/client.cpp
FINAL_BENCH   := $(FINAL_DIR)/client/benchmark.cpp

$(BIN)/server: $(FINAL_SERVER) $(FINAL_CORE) | $(BIN)
	$(CXX) $(CXXFLAGS) $(FINAL_INC) $^ -o $@ $(LDFLAGS)

$(BIN)/client: $(FINAL_CLIENT) | $(BIN)
	$(CXX) $(CXXFLAGS) $(FINAL_INC) $< -o $@

$(BIN)/benchmark: $(FINAL_BENCH) | $(BIN)
	$(CXX) $(CXXFLAGS) $(FINAL_INC) $< -o $@ $(LDFLAGS)

$(BIN):
	@mkdir -p $(BIN)

# functional smoke test (assumes a server is already listening on 1234)
test:
	cd tests && python3 test_cmds.py

# end-to-end performance + memory benchmark
bench: all
	chmod +x scripts/run_bench.sh scripts/memtrack.sh
	./scripts/run_bench.sh

clean:
	rm -rf $(BIN) bench_out
