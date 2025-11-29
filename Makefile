# ------------------------------------------------------------------
#  Makefile – cache simulator & trace replayer
# ------------------------------------------------------------------

# ▣ Toolchain
CXX      := g++
CXXFLAGS := -Wall -O2 -g -rdynamic -fno-omit-frame-pointer \
            -std=c++17 -DBOOST_STACKTRACE_USE_BACKTRACE
LIBS     := -lboost_stacktrace_backtrace -ldl -lunwind -laio -pthread

# ▣ Target-별 소스 목록 ------------------------------------------------
SRCS_cache_sim     := cache_sim.cpp trace_parser.cpp allocator.cpp \
                      icache.cpp lru_cache.cpp fifo_cache.cpp         \
                      log_cache.cpp midas_cache.cpp midas_hf.cpp midas_model.cpp evict_policy_greedy.cpp evict_policy_fifo.cpp \
					  evict_policy_cost_benefit.cpp evict_policy_lambda.cpp evict_policy_fifo_zero.cpp \
					  evict_policy_selective_fifo.cpp evict_policy_k_cost_benefit.cpp evict_policy_multiqueue.cpp \
					  evict_policy_midas.cpp \
					  ftl.cpp log_fifo_cache.cpp fairywren_cache.cpp \
					  histogram.cpp \
					  istream.cpp sepbit.cpp hot_cold.cpp hot_cold_midas.cpp multi_hot_cold.cpp \
					  emwa.cpp ghost_cache.cpp \
					  MiDAS/algorithm.cpp MiDAS/hf.cpp MiDAS/model.cpp MiDAS/queue.cpp MiDAS/ssd_config.cpp MiDAS/ssdsimul.cpp

SRCS_trace_replayer:= trace_replayer.cpp trace_parser.cpp

SRCS_mrc_calculator := mrc_calculator.cpp mrc_main.cpp trace_parser.cpp

SRCS_trace_remap    := trace_remap.cpp trace_parser.cpp

# ▣ 자동 파생 객체 목록 ------------------------------------------------
OBJS_cache_sim      := $(SRCS_cache_sim:.cpp=.o)
OBJS_trace_replayer := $(SRCS_trace_replayer:.cpp=.o)
OBJS_mrc_calculator := $(SRCS_mrc_calculator:.cpp=.o)
OBJS_trace_remap    := $(SRCS_trace_remap:.cpp=.o)

# ▣ 기본 규칙 ----------------------------------------------------------
.PHONY: all clean
all: cache_sim trace_replayer mrc_calculator trace_remap

cache_sim: $(OBJS_cache_sim)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

trace_replayer: $(OBJS_trace_replayer)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

mrc_calculator: $(OBJS_mrc_calculator)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

trace_remap: $(OBJS_trace_remap)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	$(RM) $(OBJS_cache_sim) $(OBJS_trace_replayer) $(OBJS_mrc_calculator) $(OBJS_trace_remap) cache_sim trace_replayer mrc_calculator trace_remap
