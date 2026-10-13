# Copyright (c) 2026 Vinod Naraniwal, Sonam Naraniwal (AXIORA GLOBAL SYSTEMS PVT LTD) - MIT licence
# make            -> optimised build for this machine (-march=native)
# make portable   -> generic x86-64 build (runs on any 64-bit x86 CPU)
# make avx2       -> x86-64-v3 build (AVX2 CPUs, most PCs since ~2015)
# make debug      -> -O0 -g with sanitizers
# make tsan       -> thread-sanitizer build (checks the multi-threaded search)
# EVALFILE=path   -> network embedded in the binary (default nets/default.nnue;
#                    EVALFILE= builds without an embedded net)
CXX ?= g++
SRC := $(wildcard src/*.cpp)
EVALFILE ?= nets/default.nnue
EMBED := $(if $(and $(EVALFILE),$(wildcard $(EVALFILE))),-DAXIORA_EMBED=$(abspath $(EVALFILE)),)
BASE := -std=c++17 -Wall -Wextra -pthread $(EMBED)
EXE ?= axiora

native:
	$(CXX) $(BASE) -O3 -march=native -DNDEBUG -flto $(SRC) -o $(EXE)

avx2:
	$(CXX) $(BASE) -O3 -march=x86-64-v3 -DNDEBUG -flto $(SRC) -o $(EXE)-avx2

portable:
	$(CXX) $(BASE) -O3 -DNDEBUG -flto $(SRC) -o $(EXE)-portable

debug:
	$(CXX) $(BASE) -O0 -g -fsanitize=address,undefined $(SRC) -o $(EXE)-debug

tsan:
	$(CXX) $(BASE) -O1 -g -fsanitize=thread $(SRC) -o $(EXE)-tsan

clean:
	rm -f $(EXE) $(EXE)-portable $(EXE)-avx2 $(EXE)-debug $(EXE)-tsan

.PHONY: native avx2 portable debug tsan clean
