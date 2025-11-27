CC = gcc
STD = c17
AR = ar
PROFILER = gprof
PYTHON = python3.10
SWIG = swig

ifeq ($(OS),)
OS := $(shell uname -s | tr "[:upper:]" "[:lower:]")
endif

$(info OS="$(OS)")

# Detect shell environment on Windows
ifeq ($(OS),Windows_NT)
# Try to detect if bash is available
BASH_EXISTS := $(shell bash --version >/dev/null 2>&1 && echo yes || echo no)
ifeq ($(BASH_EXISTS),yes)
SHELL := bash
USE_WINDOWS_COMMANDS := 0
else
# Use cmd.exe with Windows-native commands
SHELL := cmd
USE_WINDOWS_COMMANDS := 1
endif
else
USE_WINDOWS_COMMANDS := 0
endif

ifeq ($(OS),Windows_NT)
# Detect compiler type
COMPILER_VERSION := $(shell $(CC) --version 2>/dev/null)
IS_CLANG := $(if $(findstring clang,$(COMPILER_VERSION)),1,0)
IS_CLANG_MSVC := $(if $(findstring windows-msvc,$(COMPILER_VERSION)),1,0)

DEBUG_CFLAGS = -Wall -Wextra -std=$(STD) -g -O0 -DDEBUG
RELEASE_CFLAGS = -Wall -Wextra -std=$(STD) -O3 -fsigned-char -march=native\
 -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -m64\
 -flax-vector-conversions -fno-math-errno

ifeq ($(IS_CLANG_MSVC),1)
$(info Detected clang with MSVC target)
# Clang with MSVC backend - use MSVC-style linker flags
LIBS = -lws2_32 -lkernel32
LIBNAME = rayforce.dll
RELEASE_LDFLAGS = -Xlinker /STACK:8388608
DEBUG_LDFLAGS = -Xlinker /STACK:8388608
else ifeq ($(IS_CLANG),1)
$(info Detected clang with MinGW target)
# Clang (MSYS2) - use lld and compiler-rt
LIBS = -lm -lws2_32 -lkernel32
LIBNAME = rayforce.dll
CLANG64 = C:/msys64/clang64/bin
RELEASE_LDFLAGS = --ld-path=$(CLANG64)/ld.lld.exe -Wl,--stack,8388608 -rtlib=compiler-rt
DEBUG_LDFLAGS = --ld-path=$(CLANG64)/ld.lld.exe -Wl,--stack,8388608 -rtlib=compiler-rt
else
# GCC - use standard GNU ld
LIBS = -lm -lws2_32 -lkernel32
LIBNAME = rayforce.dll
RELEASE_LDFLAGS = -Wl,--stack,8388608
DEBUG_LDFLAGS = -Wl,--stack,8388608
endif
endif

ifeq ($(OS),linux)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -g -O0 -march=native -fsigned-char -DDEBUG -m64
RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -O3 -fsigned-char -march=native\
 -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -m64\
 -flax-vector-conversions -fno-math-errno
LIBS = -lm -ldl -lpthread
RELEASE_LDFLAGS = -Wl,--strip-all -Wl,--gc-sections -Wl,--as-needed\
 -Wl,--build-id=none -Wl,--no-eh-frame-hdr -Wl,--no-ld-generated-unwind-info -rdynamic
DEBUG_LDFLAGS = -rdynamic
LIBNAME = rayforce.so
endif

ifeq ($(OS),darwin)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -Wunused-function -std=$(STD) -g -O0 -march=native -fsigned-char -DDEBUG -m64 -fsanitize=undefined -fsanitize=address
RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -O3 -fsigned-char -march=native\
 -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -m64\
 -flax-vector-conversions -fno-math-errno
LIBS = -lm -ldl -lpthread
LIBNAME = librayforce.dylib
endif

# -mavx2 -mfma -mpclmul -mbmi2 -ffast-math
CORE_OBJECTS = core/poll.o core/ipc.o core/repl.o core/runtime.o core/sys.o core/os.o core/proc.o core/fs.o core/mmap.o core/serde.o\
 core/temporal.o core/date.o core/time.o core/timestamp.o core/guid.o core/sort.o core/ops.o core/util.o\
 core/string.o core/hash.o core/symbols.o core/format.o core/rayforce.o core/heap.o core/parse.o\
 core/eval.o core/nfo.o core/chrono.o core/env.o core/lambda.o core/unary.o core/binary.o core/vary.o\
 core/sock.o core/error.o core/math.o core/cmp.o core/items.o core/logic.o core/compose.o core/order.o core/io.o\
 core/misc.o core/freelist.o core/update.o core/join.o core/query.o core/cond.o\
 core/iter.o core/dynlib.o core/aggr.o core/index.o core/group.o core/filter.o core/atomic.o\
 core/thread.o core/pool.o core/progress.o core/term.o core/fdmap.o core/signal.o core/log.o core/queue.o
APP_OBJECTS = app/main.o
TESTS_OBJECTS = tests/main.o
BENCH_OBJECTS = bench/main.o
TARGET = rayforce
ifeq ($(OS),Windows_NT)
TARGET_EXE = $(TARGET).exe
else
TARGET_EXE = $(TARGET)
endif
CFLAGS = $(RELEASE_CFLAGS)

default: debug

all: default

obj: $(CORE_OBJECTS)

app: $(APP_OBJECTS) obj
	$(CC) $(CFLAGS) -o $(TARGET_EXE) $(CORE_OBJECTS) $(APP_OBJECTS) $(LIBS) $(LDFLAGS)

tests: -DSTOP_ON_FAIL=$(STOP_ON_FAIL) -DDEBUG
tests: LDFLAGS = $(DEBUG_LDFLAGS)
tests: $(TESTS_OBJECTS) obj
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(TESTS_OBJECTS) $(LIBS) $(LDFLAGS)
	./$(TARGET).test

bench: CC = gcc
bench: CFLAGS = $(RELEASE_CFLAGS)
bench: $(BENCH_OBJECTS) lib
ifeq ($(OS),Windows_NT)
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).bench.exe $(BENCH_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LDFLAGS)
	./$(TARGET).bench.exe
else
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).bench $(BENCH_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LDFLAGS)
	BENCH=$(BENCH) ./$(TARGET).bench
endif

%.o: %.c
	$(CC) -include core/def.h -c $^ $(CFLAGS) -o $@

lib: CFLAGS = $(RELEASE_CFLAGS)
lib: $(CORE_OBJECTS)
	$(AR) rc lib$(TARGET).a $(CORE_OBJECTS)

lib-debug: CFLAGS = $(DEBUG_CFLAGS) -DSYS_MALLOC
lib-debug: $(CORE_OBJECTS)
	$(AR) rc lib$(TARGET).a $(CORE_OBJECTS)

disasm: RELEASE_CFLAGS += -fsave-optimization-record
disasm: release
	objdump -d $(TARGET) -l > $(TARGET).S

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = $(DEBUG_LDFLAGS)
debug: app

release: CFLAGS = $(RELEASE_CFLAGS)
release: LDFLAGS = $(RELEASE_LDFLAGS)
release: app
	strip $(TARGET_EXE)

chkleak: CC = gcc
chkleak: DEBUG_CFLAGS += -DDEBUG -DSYS_MALLOC
chkleak: CFLAGS = $(DEBUG_CFLAGS)
chkleak: LDFLAGS = $(DEBUG_LDFLAGS)
chkleak: TARGET_ARGS =
chkleak: app
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET) $(TARGET_ARGS)

# Example: make clean && make profile SCRIPT=examples/update.rfl
profile: CC = gcc
profile: CFLAGS = -fPIC -Wall -Wextra -std=c17 -O3 -march=native -g -pg
profile: TARGET_ARGS =
profile: app
	./$(TARGET) $(TARGET_ARGS)
	$(PROFILER) $(TARGET) gmon.out > profile.txt

# Generate test coverage report (requires lcov)
coverage: CC = gcc
coverage: CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 --coverage
coverage: $(TESTS_OBJECTS) coverage-lib
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(TESTS_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LDFLAGS)
	lcov --directory . --zerocounters
	./$(TARGET).test
	lcov --capture --directory . --output-file coverage.info --ignore-errors unused
	lcov --remove coverage.info '/usr/*' 'tests/*' --output-file coverage.info --ignore-errors unused
	lcov --list coverage.info
	genhtml coverage.info --output-directory coverage_report
	@echo "Coverage report generated in coverage_report/index.html"

coverage-lib: CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 --coverage
coverage-lib: $(CORE_OBJECTS)
	$(AR) rc lib$(TARGET).a $(CORE_OBJECTS)

wasm: CFLAGS = -fPIC -Wall -std=c17 -O3 -msimd128 -fassociative-math -ftree-vectorize -fno-math-errno -funsafe-math-optimizations -ffinite-math-only -funroll-loops -DSYS_MALLOC
wasm: CC = emcc 
wasm: AR = emar
wasm: $(APP_OBJECTS) lib
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).js $(CORE_OBJECTS) \
	-s "EXPORTED_FUNCTIONS=['_main', '_version', '_null', '_drop_obj', '_clone_obj', '_eval_str', '_obj_fmt', '_strof_obj']" \
	-s "EXPORTED_RUNTIME_METHODS=['ccall', 'cwrap', 'FS']" -s ALLOW_MEMORY_GROWTH=1 \
	--preload-file examples@/examples \
	-L. -l$(TARGET) $(LIBS)

shared: LDFLAGS = $(RELEASE_LDFLAGS)
shared: $(CORE_OBJECTS)
	$(CC) -shared -o $(LIBNAME) $(CFLAGS) $(CORE_OBJECTS) $(LIBS) $(LDFLAGS)

clean:
ifeq ($(USE_WINDOWS_COMMANDS),1)
	-if exist *.o del /F /Q *.o
	-if exist core\*.o del /F /Q core\*.o
	-if exist app\*.o del /F /Q app\*.o
	-if exist tests\*.o del /F /Q tests\*.o
	-if exist bench\*.o del /F /Q bench\*.o
	-if exist lib$(TARGET).a del /F /Q lib$(TARGET).a
	-if exist core\*.gch del /F /Q core\*.gch
	-if exist app\*.gch del /F /Q app\*.gch
	-if exist $(TARGET).S del /F /Q $(TARGET).S
	-if exist $(TARGET).test del /F /Q $(TARGET).test
	-if exist $(TARGET).bench del /F /Q $(TARGET).bench
	-if exist *.out del /F /Q *.out
	-if exist *.so del /F /Q *.so
	-if exist *.dylib del /F /Q *.dylib
	-if exist *.dll del /F /Q *.dll
	-if exist $(TARGET).js del /F /Q $(TARGET).js
	-if exist $(TARGET).wasm del /F /Q $(TARGET).wasm
	-if exist $(TARGET) del /F /Q $(TARGET)
	-if exist $(TARGET).exe del /F /Q $(TARGET).exe
	-if exist tests\*.gcno del /F /Q tests\*.gcno
	-if exist tests\*.gcda del /F /Q tests\*.gcda
	-if exist tests\*.gcov del /F /Q tests\*.gcov
	-if exist core\*.gcno del /F /Q core\*.gcno
	-if exist core\*.gcda del /F /Q core\*.gcda
	-if exist core\*.gcov del /F /Q core\*.gcov
	-if exist coverage.info del /F /Q coverage.info
	-if exist coverage_report rmdir /S /Q coverage_report
	-if exist .DS_Store del /F /Q .DS_Store
	-if exist core\*.opt.yaml del /F /Q core\*.opt.yaml
	-if exist app\*.opt.yaml del /F /Q app\*.opt.yaml
	-if exist tests\*.opt.yaml del /F /Q tests\*.opt.yaml
	-if exist bench\*.opt.yaml del /F /Q bench\*.opt.yaml
else
	-rm -f *.o
	-rm -f core/*.o
	-rm -f app/*.o
	-rm -rf tests/*.o
	-rm -rf bench/*.o
	-rm -f lib$(TARGET).a
	-rm -f core/*.gch
	-rm -f app/*.gch
	-rm -f $(TARGET).S
	-rm -f $(TARGET).test
	-rm -f $(TARGET).bench
	-rm -rf *.out
	-rm -rf *.so
	-rm -rf *.dylib
	-rm -rf *.dll
	-rm -f $(TARGET).js
	-rm -f $(TARGET).wasm
	-rm -f $(TARGET)
	-rm -f $(TARGET).exe
	-rm -f tests/*.gcno tests/*.gcda tests/*.gcov
	-rm -f core/*.gcno core/*.gcda core/*.gcov
	-rm -f coverage.info
	-rm -rf coverage_report/
	-rm -f .DS_Store # macOS
	-rm -f core/*.opt.yaml
	-rm -f app/*.opt.yaml
	-rm -f tests/*.opt.yaml
	-rm -f bench/*.opt.yaml
endif

# trigger github to make a nightly build
nightly:
	git push origin :nightly
	git tag -d nightly
	git tag nightly
	git push origin nightly
