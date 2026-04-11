# ------------------------------------------------------------------
#  Makefile – cache simulator & trace replayer
# ------------------------------------------------------------------

# ▣ Toolchain
CXX      := g++
CXXFLAGS := -Wall -O2 -g -rdynamic -fno-omit-frame-pointer \
            -std=c++17 -DBOOST_STACKTRACE_USE_BACKTRACE \
            -D_LARGEFILE64_SOURCE -I. -Idogi
LIBS     := -lboost_stacktrace_backtrace -ldl -lunwind -laio -lopenblas -pthread

# ▣ Target-별 소스 목록 ------------------------------------------------
SRCS_cache_sim     := cache_sim.cpp trace_parser.cpp allocator.cpp \
                      icache.cpp lru_cache.cpp fifo_cache.cpp         \
                      log_cache.cpp midas_cache.cpp midas_hf.cpp midas_model.cpp evict_policy_greedy.cpp evict_policy_fifo.cpp \
					  evict_policy_cost_benefit.cpp evict_policy_cb_greedy.cpp evict_policy_lambda.cpp evict_policy_fifo_zero.cpp \
					  evict_policy_selective_fifo.cpp evict_policy_k_cost_benefit.cpp evict_policy_multiqueue.cpp \
					  evict_policy_midas.cpp \
					  ftl.cpp log_fifo_cache.cpp fairywren_cache.cpp \
					  histogram.cpp \
					  istream.cpp sepbit.cpp hot_cold.cpp hot_cold_midas.cpp multi_hot_cold.cpp readwrite_stream.cpp \
					  emwa.cpp ghost_cache.cpp \
					  MiDAS/algorithm.cpp MiDAS/hf.cpp MiDAS/model.cpp MiDAS/queue.cpp MiDAS/ssd_config.cpp MiDAS/ssdsimul.cpp \
					  dogi_cache.cpp

# DOGI sources (.cc) — copied as-is from /home/sejun000/DOGI; ZNS/RocksDB/buse/scheduler removed
DOGI_CC_SRCS := dogi/app/classifier.cc \
				dogi/app/freq_features.cc \
				dogi/app/global.cc \
				dogi/app/group_config.cc \
				dogi/app/group_optimizer.cc \
				dogi/app/mlp_inference.cc \
				dogi/app/model_train.cc \
				dogi/src/placement/dogi.cc \
				dogi/src/selection/dogiselect.cc \
				dogi/src/selection/costbenefit.cc \
				dogi/src/selection/greedy.cc \
				dogi/src/logstore/manager.cc \
				dogi/src/logstore/segment.cc \
				dogi/src/indexmap/array.cc \
				dogi/src/indexmap/filter.cc \
				dogi/src/indexmap/hashmap.cc \
				dogi/src/storage_adapter/local_adapter.cc

DOGI_OBJS := $(DOGI_CC_SRCS:.cc=.o)

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

cache_sim: $(OBJS_cache_sim) $(DOGI_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

trace_replayer: $(OBJS_trace_replayer)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

mrc_calculator: $(OBJS_mrc_calculator)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

trace_remap: $(OBJS_trace_remap)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(SRCS_cache_sim:.cpp=.d)
-include $(DOGI_OBJS:.o=.d)

clean:
	$(RM) $(OBJS_cache_sim) $(DOGI_OBJS) $(OBJS_trace_replayer) $(OBJS_mrc_calculator) $(OBJS_trace_remap) cache_sim trace_replayer mrc_calculator trace_remap
