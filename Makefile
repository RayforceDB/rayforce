CC      ?= clang
STD     = c17
AR      = ar
TARGET  = rayforce
# Version is authoritative in include/rayforce.h — extract it here
VERSION_MAJOR := $(shell grep 'RAY_VERSION_MAJOR' include/rayforce.h | head -1 | awk '{print $$3}')
VERSION_MINOR := $(shell grep 'RAY_VERSION_MINOR' include/rayforce.h | head -1 | awk '{print $$3}')
VERSION_PATCH := $(shell grep 'RAY_VERSION_PATCH' include/rayforce.h | head -1 | awk '{print $$3}')
VERSION       = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date -u +%Y-%m-%d)

WARNS   = -Wall -Wextra -Werror -Wstrict-prototypes -Wno-unused-parameter
DEFS    = -DRAYFORCE_GIT_COMMIT=\"$(GIT_HASH)\" -DRAYFORCE_BUILD_DATE=\"$(BUILD_DATE)\"
INCLUDES = -Iinclude -Isrc

UNAME_S := $(shell uname -s)

DEBUG_CFLAGS   = -fPIC $(WARNS) -std=$(STD) -g -O0 -march=native -DDEBUG \
  -fsanitize=address,undefined -fno-omit-frame-pointer
RELEASE_CFLAGS = -fPIC $(WARNS) -std=$(STD) -O3 -march=native \
  -funroll-loops -fomit-frame-pointer -fno-math-errno \
  -fassociative-math -ffp-contract=fast -fno-signed-zeros -fno-trapping-math
# -fassociative-math: license to reorder FP additions/multiplications.
#   Required for autovectorization of F64 reductions (sum/avg/dot).
#   Without it, scalar_sum_f64_fn at group.c:1666 is a serial latency
#   chain (~3-4 cycles/op) instead of 4-8 lanes/cycle SIMD.
# -ffp-contract=fast: emit FMA (fused multiply-add) where beneficial.
# -fno-signed-zeros: treat -0.0 == +0.0 (matches how distinct/hashset
#   normalises -0.0 → 0.0 in group.c:208).
# -fno-trapping-math: assume FP ops never trap; enables more reorder.
# NOT enabling -ffinite-math-only or -ffast-math: those assume no
#   NaN/Inf, which would break our null sentinels (NaN-encoded nulls
#   in F64 columns).

# Coverage: clang source-based instrumentation.  Sanitizers conflict
# with the profile runtime, so we drop them; -O0 keeps line numbers
# and avoids dead-code regions getting marked uncovered for the
# wrong reason.  See `make coverage` below.
COVERAGE_CFLAGS = -fPIC $(WARNS) -std=$(STD) -g -O0 -march=native -DDEBUG \
  -fno-omit-frame-pointer -fprofile-instr-generate -fcoverage-mapping
COVERAGE_LDFLAGS = -fprofile-instr-generate -fcoverage-mapping

ifeq ($(UNAME_S),Linux)
  LIBS            = -lm -lpthread
  RELEASE_LDFLAGS = -Wl,--gc-sections -Wl,--as-needed
else
  LIBS            = -lm
  RELEASE_LDFLAGS = -Wl,-dead_strip
endif

DEBUG_LDFLAGS   = -fsanitize=address,undefined

CFLAGS  = $(DEBUG_CFLAGS)
LDFLAGS = $(DEBUG_LDFLAGS)

# Sources
LIB_SRC  = $(wildcard src/*/*.c)
LIB_SRC := $(filter-out src/app/main.c, $(LIB_SRC))
LIB_OBJ  = $(LIB_SRC:.c=.o)
MAIN_SRC = src/app/main.c
MAIN_OBJ = $(MAIN_SRC:.c=.o)
TEST_SRC = $(wildcard test/*.c)
TEST_OBJ = $(TEST_SRC:.c=.o)

# Default target
default: debug

%.o: %.c
	$(CC) -c $(CFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

# Main binary — shared by debug/release/test (test/rfl/system/ipc_diff.rfl
# spawns ./$(TARGET) as a server, so test depends on it too).
$(TARGET): $(LIB_OBJ) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(LIB_OBJ) $(MAIN_OBJ) $(LIBS) $(LDFLAGS)

# Debug build
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = $(DEBUG_LDFLAGS)
debug: $(TARGET)

# Release build
release: CFLAGS = $(RELEASE_CFLAGS)
release: LDFLAGS = $(RELEASE_LDFLAGS)
release: $(TARGET)

# Static library
lib: CFLAGS = $(RELEASE_CFLAGS)
lib: $(LIB_OBJ)
	$(AR) rc lib$(TARGET).a $(LIB_OBJ)

# Tests.  Depends on $(TARGET) because test/rfl/system/ipc_diff.rfl
# spawns ./$(TARGET) as an IPC server via .sys.exec — both binaries
# must exist on disk and share the build flavour (sanitizers, coverage).
test: CFLAGS = $(DEBUG_CFLAGS)
test: LDFLAGS = $(DEBUG_LDFLAGS)
test: $(TARGET) $(LIB_OBJ) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET).test $(LIB_OBJ) $(TEST_OBJ) $(LIBS) $(LDFLAGS) -Itest
	./$(TARGET).test

# Coverage report.  Builds both binaries with clang source-based
# instrumentation, runs the test suite (writing one .profraw per
# process — the test binary AND every IPC server it spawns —
# thanks to LLVM_PROFILE_FILE='%p' giving each pid a unique file),
# merges, and emits an HTML report under coverage_html/.
#
# Requires clang + llvm-profdata + llvm-cov.  Sanitizers are dropped
# for this build (incompatible with the profile runtime).
coverage:
	@command -v clang         >/dev/null || { echo "coverage: clang not found";         exit 1; }
	@command -v llvm-profdata >/dev/null || { echo "coverage: llvm-profdata not found"; exit 1; }
	@command -v llvm-cov      >/dev/null || { echo "coverage: llvm-cov not found";      exit 1; }
	$(MAKE) clean
	rm -f cov-*.profraw default.profraw coverage.profdata
	rm -rf coverage_html
	LLVM_PROFILE_FILE='cov-%p.profraw' $(MAKE) test \
		CC=clang \
		DEBUG_CFLAGS='$(COVERAGE_CFLAGS)' \
		DEBUG_LDFLAGS='$(COVERAGE_LDFLAGS)'
	llvm-profdata merge -sparse cov-*.profraw -o coverage.profdata
	llvm-cov show ./$(TARGET).test \
		-instr-profile=coverage.profdata \
		-format=html -output-dir=coverage_html \
		-show-line-counts-or-regions \
		-ignore-filename-regex='test/.*|/usr/.*'
	@echo
	@echo "=== coverage summary ==="
	@llvm-cov report ./$(TARGET).test \
		-instr-profile=coverage.profdata \
		-ignore-filename-regex='test/.*|/usr/.*' 2>/dev/null | tail -3
	@echo
	@echo "→ coverage_html/index.html"

clean:
	-rm -f $(LIB_OBJ) $(MAIN_OBJ) $(TEST_OBJ)
	-rm -f $(TARGET) $(TARGET).test lib$(TARGET).a
	-rm -rf build build_release
	# Test-generated fixtures (see test/rfl/system/*.rfl) — should not linger after a run.
	-rm -f rf_test_*.csv
	# Coverage artefacts (see `make coverage`).
	-rm -f cov-*.profraw default.profraw coverage.profdata
	-rm -rf coverage_html

.PHONY: default debug release lib test coverage clean
