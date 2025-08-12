# ------------------------------------------------------------------
#  Makefile – cache simulator & trace replayer
# ------------------------------------------------------------------

# ▣ Toolchain
CXX      := g++
CXXFLAGS := -Wall -O2 -g -rdynamic -fno-omit-frame-pointer \
            -std=c++17 -DBOOST_STACKTRACE_USE_BACKTRACE
LIBS     := -lboost_stacktrace_backtrace -ldl -lunwind -laio

# ▣ Target-별 소스 목록 ------------------------------------------------
SRCS_cache_sim     := cache_sim.cpp trace_parser.cpp allocator.cpp \
                      icache.cpp lru_cache.cpp fifo_cache.cpp         \
                      log_cache.cpp evict_policy_greedy.cpp evict_policy_fifo.cpp \
					  evict_policy_cost_benefit.cpp evict_policy_lambda.cpp evict_policy_fifo_zero.cpp \
					  evict_policy_selective_fifo.cpp evict_policy_k_cost_benefit.cpp evict_policy_multiqueue.cpp \
					  ftl.cpp log_fifo_cache.cpp \
					  istream.cpp sepbit.cpp hot_cold.cpp

SRCS_trace_replayer:= trace_replayer.cpp trace_parser.cpp

# ▣ 자동 파생 객체 목록 ------------------------------------------------
OBJS_cache_sim      := $(SRCS_cache_sim:.cpp=.o)
OBJS_trace_replayer := $(SRCS_trace_replayer:.cpp=.o)

# ▣ 기본 규칙 ----------------------------------------------------------
.PHONY: all clean
all: cache_sim trace_replayer

cache_sim: $(OBJS_cache_sim)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

trace_replayer: $(OBJS_trace_replayer)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	$(RM) $(OBJS_cache_sim) $(OBJS_trace_replayer) cache_sim trace_replayer