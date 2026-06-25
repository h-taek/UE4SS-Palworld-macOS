CXX      := clang++
CAPSTONE := $(shell brew --prefix capstone)
CXXFLAGS := -std=c++23 -arch arm64 -O2 -fPIC -Wall -Wextra -Isrc -I$(CAPSTONE)/include
LDLIBS   := -L$(CAPSTONE)/lib -lcapstone
FRAMEWORKS := -framework CoreFoundation

SRCS := $(wildcard src/*.cpp src/darwin/*.cpp src/hook/*.cpp src/engine/*.cpp)
DYLIB := build/libue4ss_mac_spike.dylib

.PHONY: all clean test sign
all: $(DYLIB) sign

$(DYLIB): $(SRCS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -dynamiclib $(SRCS) -o $@ $(LDLIBS) $(FRAMEWORKS)

sign: $(DYLIB)
	codesign -s - --force --timestamp=none $(DYLIB)

# 유닛 테스트(게임 불필요): 각 test_*.cpp는 자체 main을 가짐
TEST_SRCS  := $(wildcard tests/test_*.cpp)
IMPL_SRCS  := $(wildcard src/darwin/*.cpp src/hook/*.cpp src/engine/*.cpp)
test:
	@mkdir -p build/tests
	@for t in $(TEST_SRCS); do \
	  name=$$(basename $$t .cpp); \
	  echo "== $$name =="; \
	  $(CXX) $(CXXFLAGS) $$t $(IMPL_SRCS) \
	    -o build/tests/$$name $(LDLIBS) $(FRAMEWORKS) && \
	  codesign -s - --force --entitlements tests/jit.entitlements build/tests/$$name && \
	  build/tests/$$name || exit 1; \
	done

clean:
	rm -rf build
