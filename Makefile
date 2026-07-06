CC      ?= clang
# Some targets need clang specifically regardless of $(CC): the fuzzing
# runtime (-fsanitize=fuzzer) and -MJ compilation-database fragments are
# clang-only.  Same precedent as `make coverage`, which pins CC=clang.
CLANG   ?= clang
# The fuzzing runtime (-fsanitize=fuzzer) is implemented in C++, so the
# final link must pull in the C++ standard library.  Using the C++ driver
# for that one link step is the portable way to get its path right; the
# translation units themselves stay C.
CLANGXX ?= clang++
STD     = c17
AR      = ar
TARGET  = rayforce
# Version: the git tag is the single source of truth. Precedence:
#   explicit override (CI passes RAY_VERSION=X.Y.Z from the release PR title)
#   > latest git tag (vX.Y.Z) > 0.0.0 dev default.
# The value is injected into the build via -D below (see DEFS); nothing is
# hand-edited in source to cut a release. See RELEASE.md.
RAY_VERSION ?= $(shell git describe --tags --match 'v[0-9]*.[0-9]*.[0-9]*' --abbrev=0 2>/dev/null | sed 's/^v//')
ifeq ($(strip $(RAY_VERSION)),)
  RAY_VERSION := 0.0.0
endif
VERSION       = $(RAY_VERSION)
VERSION_MAJOR := $(word 1,$(subst ., ,$(RAY_VERSION)))
VERSION_MINOR := $(word 2,$(subst ., ,$(RAY_VERSION)))
VERSION_PATCH := $(word 3,$(subst ., ,$(RAY_VERSION)))
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date -u +%Y-%m-%d)

WARNS   = -Wall -Wextra -Werror -Wstrict-prototypes -Wno-unused-parameter
DEFS    = -DRAYFORCE_GIT_COMMIT=\"$(GIT_HASH)\" -DRAYFORCE_BUILD_DATE=\"$(BUILD_DATE)\" \
          -DRAY_VERSION_MAJOR=$(VERSION_MAJOR) -DRAY_VERSION_MINOR=$(VERSION_MINOR) \
          -DRAY_VERSION_PATCH=$(VERSION_PATCH) -DRAYFORCE_VERSION=\"$(RAY_VERSION)\"
INCLUDES = -Iinclude -Isrc
# Header-dependency tracking: -MMD emits a .d makefile fragment next to
# each .o listing the headers it included (user headers only, not system);
# -MP adds a phony target per header so deleting a header doesn't break the
# build with a "no rule to make" error.  The fragments are -included below.
DEPFLAGS = -MMD -MP

UNAME_S := $(shell uname -s)

# Target microarchitecture.  Default `native` = build for THIS machine (fastest;
# right for local builds and the per-machine release tarballs).  Override for
# REDISTRIBUTABLE packages (.deb) that must run on any CPU, e.g.
# `make release RAY_MARCH=x86-64-v3` (AVX2 baseline, ~2013+) — a -march=native
# binary handed to a different/older CPU dies with SIGILL.
RAY_MARCH ?= native

DEBUG_CFLAGS   = -fPIC $(WARNS) -std=$(STD) -g -O0 -march=$(RAY_MARCH) -DDEBUG \
  -fsanitize=address,undefined -fno-omit-frame-pointer
RELEASE_CFLAGS = -fPIC $(WARNS) -std=$(STD) -O3 -march=$(RAY_MARCH) \
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
COVERAGE_CFLAGS = -fPIC $(WARNS) -std=$(STD) -g -O0 -march=$(RAY_MARCH) -DDEBUG \
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

# Extra build flavours (fuzz / tsan / hardened).  Each gets its OWN object
# suffix (%.fuzz.o, %.tsan.o, %.hard.o) because the primary flavours compile
# objects in place — mixing, say, thread-sanitized and address-sanitized
# objects in one link is undefined at best.  The debug/release pair keeps
# its historical in-place behaviour.
#
# fuzz: coverage-guided fuzzing drivers under fuzz/ link against these
#   objects.  -O1 (not -O0): ~5-10x more executions/sec with still-usable
#   stack traces.  RAY_FUZZING compiles out shell/network escape hatches
#   (see src/lang/syscmd.c) so eval-based fuzzing stays sandboxed.
#   Linux-only: the system toolchain on macOS does not bundle the fuzzer
#   runtime.
FUZZ_CFLAGS = -fPIC $(WARNS) -std=$(STD) -g -O1 -march=$(RAY_MARCH) -DDEBUG -DRAY_FUZZING \
  -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer

# tsan: data-race detection for the lock-free pool/heap paths.  Mutually
# exclusive with address sanitizing, hence its own flavour.  DEBUG is kept
# so the invariant checks run under it too.
TSAN_CFLAGS  = -fPIC $(WARNS) -std=$(STD) -g -O1 -march=$(RAY_MARCH) -DDEBUG \
  -fsanitize=thread -fno-omit-frame-pointer
TSAN_LDFLAGS = -fsanitize=thread

# hardened: the cloud-deployment tier.  Release optimization levels, but
# keeps symbols and frame pointers (reliable crash backtraces, ~1% cost)
# and RAY_HARDENED promotes the cheap invariant checks that DEBUG-only
# builds would strip.  Same FP-reassociation flags as release — the two
# flavours must not diverge numerically.
HARDENED_CFLAGS = -fPIC $(WARNS) -std=$(STD) -O3 -march=$(RAY_MARCH) -g \
  -fno-omit-frame-pointer -DRAY_HARDENED \
  -funroll-loops -fno-math-errno -fassociative-math -ffp-contract=fast \
  -fno-signed-zeros -fno-trapping-math

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

# Per-flavour object lists (see the flavour CFLAGS block above).
FUZZ_LIB_OBJ  = $(LIB_SRC:.c=.fuzz.o)
TSAN_LIB_OBJ  = $(LIB_SRC:.c=.tsan.o)
TSAN_MAIN_OBJ = $(MAIN_SRC:.c=.tsan.o)
TSAN_TEST_OBJ = $(TEST_SRC:.c=.tsan.o)
HARD_LIB_OBJ  = $(LIB_SRC:.c=.hard.o)
HARD_MAIN_OBJ = $(MAIN_SRC:.c=.hard.o)

# Auto-generated header dependencies (one .d per .o, see DEPFLAGS).
# The fragments are -included at the very END of this file — including
# them here would let a .d's first rule (e.g. `foo.o: ...`) become the
# default goal, so bare `make` would build one object instead of `debug`.
DEPS = $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJ:.o=.d) \
       $(FUZZ_LIB_OBJ:.o=.d) \
       $(TSAN_LIB_OBJ:.o=.d) $(TSAN_MAIN_OBJ:.o=.d) $(TSAN_TEST_OBJ:.o=.d) \
       $(HARD_LIB_OBJ:.o=.d) $(HARD_MAIN_OBJ:.o=.d)

# Default target (pinned so an -included .d fragment can't steal it).
.DEFAULT_GOAL := default
default: debug

%.o: %.c
	$(CC) -c $(CFLAGS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

%.fuzz.o: %.c
	$(CLANG) -c $(FUZZ_CFLAGS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

# TSan objects build with clang: gcc's -Werror=tsan rejects standalone
# atomic_thread_fence (src/core/platform.h), which clang instruments fine.
%.tsan.o: %.c
	$(CLANG) -c $(TSAN_CFLAGS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

%.hard.o: %.c
	$(CC) -c $(HARDENED_CFLAGS) $(DEPFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

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

# Release tarball: build the optimized binary and package it as
# dist/rayforce-<version>-<os>-<arch>.tar.gz plus a SHA-256 checksum.
# Used by .github/workflows/release.yml (which passes RAY_VERSION=X.Y.Z) and
# runnable locally. VERSION comes from the tag/override resolved at the top.
dist: release
	@mkdir -p dist
	@os=$$(uname -s | tr 'A-Z' 'a-z'); arch=$$(uname -m); \
	 name=$(TARGET)-$(VERSION)-$$os-$$arch; stage=dist/$$name; \
	 mkdir -p $$stage; \
	 cp $(TARGET) LICENSE README.md include/rayforce.h $$stage/; \
	 tar -czf dist/$$name.tar.gz -C dist $$name; \
	 rm -rf $$stage; \
	 ( cd dist && { command -v sha256sum >/dev/null 2>&1 && sha256sum $$name.tar.gz || shasum -a 256 $$name.tar.gz; } > $$name.tar.gz.sha256 ); \
	 echo "built dist/$$name.tar.gz"

# Worker threads per process during tests. Without this the runtime
# auto-sizes to ncpu-1, so on a many-core box the in-process harness AND
# every server it spawns via .sys.exec each create ~ncpu-1 threads — a lot of
# wasted CPU for tiny test inputs. RAYFORCE_CORES (honored by ray_pool_create)
# caps it; children inherit the env. Override for a fuller parallel stress,
# e.g. `make test TEST_CORES=0` (serial) or `make test TEST_CORES=8`.
TEST_CORES ?= 2

# Tests.  Depends on $(TARGET) because test/rfl/system/ipc_diff.rfl
# spawns ./$(TARGET) as an IPC server via .sys.exec — both binaries
# must exist on disk and share the build flavour (sanitizers, coverage).
test: CFLAGS = $(DEBUG_CFLAGS)
test: LDFLAGS = $(DEBUG_LDFLAGS)
test: $(TARGET) $(LIB_OBJ) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET).test $(LIB_OBJ) $(TEST_OBJ) $(LIBS) $(LDFLAGS) -Itest
	RAYFORCE_CORES=$(TEST_CORES) ./$(TARGET).test

# ─── ThreadSanitizer ────────────────────────────────────────────────
# Data-race detection for the lock-free pool/heap paths, which the
# default ASan build cannot see (ASan and TSan are mutually exclusive,
# hence the separate .tsan.o object flavour).  TSAN_CORES defaults to 4:
# the harness default of 2 under-exercises the SPMC work-stealing ring,
# and TSan wants real contention to observe races.
#
# tsan-test runs the IN-PROCESS concurrency suites by default (pool /
# heap / parallel / stress); the IPC-diff tests spawn ./$(TARGET) as a
# server and would mix build flavours, so they are out of scope here.
# Narrow or widen with TSAN_FILTER, e.g. `make tsan-test TSAN_FILTER=pool`.
TSAN_CORES  ?= 4
TSAN_FILTER ?= stress
TSAN_ENV     = TSAN_OPTIONS="suppressions=$(PWD)/.tsan-suppressions history_size=7 second_deadlock_stack=1 halt_on_error=0"

tsan: $(TSAN_LIB_OBJ) $(TSAN_MAIN_OBJ)
	$(CLANG) $(TSAN_CFLAGS) -o $(TARGET).tsan $(TSAN_LIB_OBJ) $(TSAN_MAIN_OBJ) $(LIBS) $(TSAN_LDFLAGS)

tsan-test: $(TSAN_LIB_OBJ) $(TSAN_TEST_OBJ)
	$(CLANG) $(TSAN_CFLAGS) -o $(TARGET).test.tsan $(TSAN_LIB_OBJ) $(TSAN_TEST_OBJ) $(LIBS) $(TSAN_LDFLAGS) -Itest
	RAYFORCE_CORES=$(TSAN_CORES) $(TSAN_ENV) ./$(TARGET).test.tsan $(if $(TSAN_FILTER),-f $(TSAN_FILTER),)

# ─── Hardened cloud tier ────────────────────────────────────────────
# The build flavour shipped to the cloud: release optimisation and the
# same FP-reassociation flags as `release` (so the two never diverge
# numerically), but keeps debug symbols and frame pointers for reliable
# crash backtraces (~1% cost) and defines RAY_HARDENED, which promotes
# the cheap invariant checks that a plain release strips.  The fatal-
# signal handler (src/core/crash.c) is compiled into every flavour and
# installed unconditionally from main().
# -rdynamic exports the dynamic symbol table so backtrace_symbols_fd in the
# crash handler resolves function NAMES (not just module+offset) in a
# production trace.  Worth the slightly larger symbol table for a cloud
# binary whose crashes must be diagnosable from logs alone.
hardened: $(HARD_LIB_OBJ) $(HARD_MAIN_OBJ)
	$(CC) $(HARDENED_CFLAGS) -o $(TARGET) $(HARD_LIB_OBJ) $(HARD_MAIN_OBJ) $(LIBS) $(RELEASE_LDFLAGS) -rdynamic

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
		-ignore-filename-regex='test/.*|/usr/.*|.*_alloc_stub\.c|include/rayforce\.h'
	@echo
	@echo "=== coverage summary ==="
	@llvm-cov report ./$(TARGET).test \
		-instr-profile=coverage.profdata \
		-ignore-filename-regex='test/.*|/usr/.*|.*_alloc_stub\.c|include/rayforce\.h' 2>/dev/null | tail -3
	@echo
	@echo "→ coverage_html/index.html"

# Compilation database for editors and standalone analyzers.  Flags are
# uniform across every translation unit, so no build-interception tool is
# needed: a syntax-only pass with -MJ emits one JSON fragment per file and
# the fragments concatenate into compile_commands.json.
compdb:
	@rm -rf build_compdb && mkdir -p build_compdb
	@i=0; for f in $(LIB_SRC) $(MAIN_SRC); do \
	  i=$$((i+1)); \
	  $(CLANG) -c -o /dev/null -std=$(STD) -DDEBUG $(DEFS) $(INCLUDES) \
	    -MJ build_compdb/$$i.json $$f || exit 1; \
	done
	@{ echo '['; cat build_compdb/*.json | sed '$$s/,$$//'; echo ']'; } > compile_commands.json
	@rm -rf build_compdb
	@echo "compile_commands.json: $$(grep -c '"file"' compile_commands.json) entries"

# ─── Fuzzing (coverage-guided; clang + Linux only) ──────────────────
# Drivers live under fuzz/ and link against the address-sanitized
# .fuzz.o library objects.  Grown corpora live in fuzz/corpus/<t>/
# (gitignored); committed starter inputs live in fuzz/seeds/<t>/.
#
#   make fuzz-parse                 # 60s (default) run of one target
#   make fuzz-parse FUZZ_RUNTIME=0  # run until a crash / Ctrl-C
#   make fuzz-smoke                 # short CI pass over the fast targets
#
# The system toolchain on macOS does not ship the fuzzer runtime, so
# these targets are Linux-only by design; CI gates them to ubuntu.
FUZZ_RUNTIME ?= 60
FUZZ_OPTS     = -rss_limit_mb=4096 -timeout=10 -max_len=65536 -print_final_stats=1
FUZZ_TARGETS  = parse numparse de eval csv journal
# Escape hatch for hosts where clang auto-selects a gcc toolchain dir that
# lacks libstdc++ (e.g. a partially-installed newer gcc shadowing the real
# one).  Normally empty; set on such a box, e.g.
#   make fuzz-smoke FUZZ_LDEXTRA=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/11
FUZZ_LDEXTRA ?=

# Per-target dictionary (defaults to the rayfall token dict; override in
# the map below).  numparse needs no dictionary.
DICT_parse    = rayfall
DICT_eval     = rayfall
DICT_de       = frame
DICT_journal  = frame
DICT_csv      = csv

# The driver .c is compiled with fuzzer instrumentation; the link goes
# through the C++ driver so the fuzzer runtime's C++ dependency resolves.
build_fuzz/fuzz_%: fuzz/fuzz_%.c $(FUZZ_LIB_OBJ)
	@mkdir -p build_fuzz
	$(CLANG) -c $(FUZZ_CFLAGS) -fsanitize=fuzzer $(DEFS) $(INCLUDES) -Ifuzz \
	  -o build_fuzz/$*.o $<
	$(CLANGXX) -fsanitize=fuzzer,address,undefined $(FUZZ_LDEXTRA) \
	  -o $@ build_fuzz/$*.o $(FUZZ_LIB_OBJ) $(LIBS)

fuzz-%: build_fuzz/fuzz_%
	@mkdir -p fuzz/corpus/$*
	@dict=$(DICT_$*); \
	 dictopt=$${dict:+-dict=fuzz/dict/$$dict.dict}; \
	 seeds=$$( [ -d fuzz/seeds/$* ] && echo fuzz/seeds/$* ); \
	 set -x; \
	 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
	   ./build_fuzz/fuzz_$* fuzz/corpus/$* $$seeds \
	   $$dictopt $(FUZZ_OPTS) -max_total_time=$(FUZZ_RUNTIME)

# Short smoke pass for PR CI — a healthy corpus finds nothing new in a
# minute, and any crash it surfaces is a genuine regression.
fuzz-smoke:
	$(MAKE) fuzz-parse fuzz-numparse fuzz-de FUZZ_RUNTIME=60

.PHONY: fuzz-smoke

# ─── Static analysis ────────────────────────────────────────────────
# clang-tidy runs OUTSIDE the build: it never touches the real compiler
# warnings or -Werror.  Flags are uniform across every translation unit,
# so we pass them after `--` and skip the compilation database entirely
# (the `.clang-tidy` file selects the correctness-only check set).
# `make tidy FILES="src/ops/foo.c"` narrows to specific files.
TIDY_FLAGS = -std=$(STD) -DDEBUG $(DEFS) $(INCLUDES)
FILES     ?= $(LIB_SRC) $(MAIN_SRC)

tidy:
	@command -v clang-tidy >/dev/null || { echo "tidy: clang-tidy not found"; exit 1; }
	clang-tidy --quiet $(FILES) -- $(TIDY_FLAGS)

# cppcheck is a second-opinion linter — advisory only, never a gate.
cppcheck:
	@command -v cppcheck >/dev/null || { echo "cppcheck: not found"; exit 1; }
	cppcheck --enable=warning,portability --inline-suppr --error-exitcode=1 \
	  -j $(shell nproc 2>/dev/null || echo 4) \
	  --suppress=missingIncludeSystem \
	  --suppress=assignBoolToPointer \
	  --suppress=nullPointerRedundantCheck \
	  --std=c17 -q $(INCLUDES) src/
# assignBoolToPointer: cppcheck misparses the GNU computed-goto label
#   address `&&label` (a void*) as a logical-AND yielding a bool.
# nullPointerRedundantCheck: a heuristic that fires on the codebase's
#   deliberate defensive "check then use" ordering; reviewed as benign.

.PHONY: tidy cppcheck

clean:
	-rm -f $(LIB_OBJ) $(MAIN_OBJ) $(TEST_OBJ)
	-rm -f $(FUZZ_LIB_OBJ) $(TSAN_LIB_OBJ) $(TSAN_MAIN_OBJ) $(TSAN_TEST_OBJ) \
	       $(HARD_LIB_OBJ) $(HARD_MAIN_OBJ)
	-rm -f $(DEPS)
	-rm -f $(TARGET) $(TARGET).test lib$(TARGET).a
	-rm -f $(TARGET).tsan $(TARGET).test.tsan
	-rm -rf build build_release build_fuzz build_compdb dist
	# Test-generated fixtures (see test/rfl/system/*.rfl) — should not linger after a run.
	-rm -f rf_test_*.csv
	# Coverage artefacts (see `make coverage`).
	-rm -f cov-*.profraw default.profraw coverage.profdata
	-rm -rf coverage_html

.PHONY: default debug release lib dist test coverage compdb tsan tsan-test hardened fuzz-smoke tidy cppcheck clean

# Header dependencies last: .d fragments only add prerequisites to the
# object targets above, and being last they can't hijack the default goal.
# -include silently skips any that don't exist yet (first build).
-include $(DEPS)
