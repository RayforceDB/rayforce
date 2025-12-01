CC = clang
STD = c17
AR = ar
PROFILER = gprof

ifeq ($(OS),)
OS := $(shell uname -s | tr "[:upper:]" "[:lower:]")
endif

$(info OS="$(OS)")

ifeq ($(OS),Windows_NT)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -g -O0 -DDEBUG
LIBS = -lm -lws2_32 -lkernel32
LIBNAME = rayforce.dll
endif

ifeq ($(OS),linux)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -g -O0 -march=native -fsigned-char -DDEBUG -m64
LIBS = -lm -ldl -lpthread
RELEASE_LDFLAGS = -Wl,--strip-all -Wl,--gc-sections -Wl,--as-needed\
 -Wl,--build-id=none -Wl,--no-eh-frame-hdr -Wl,--no-ld-generated-unwind-info -rdynamic
DEBUG_LDFLAGS = -rdynamic
LIBNAME = rayforce.so
endif

ifeq ($(OS),darwin)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -Wunused-function -std=$(STD) -g -O0 -march=native -fsigned-char -DDEBUG -m64 -fsanitize=undefined -fsanitize=address
LIBS = -lm -ldl -lpthread
LIBNAME = librayforce.dylib
endif

# -mavx2 -mfma -mpclmul -mbmi2 -ffast-math 
RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -O3 -fsigned-char -march=native\
 -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -m64\
 -flax-vector-conversions -fno-math-errno
CORE_OBJECTS = core/poll.o core/ipc.o core/repl.o core/runtime.o core/sys.o core/os.o core/proc.o core/fs.o core/mmap.o core/serde.o\
 core/temporal.o core/date.o core/time.o core/timestamp.o core/guid.o core/sort.o core/ops.o core/util.o\
 core/string.o core/hash.o core/symbols.o core/format.o core/rayforce.o core/heap.o core/parse.o\
 core/eval.o core/nfo.o core/chrono.o core/env.o core/lambda.o core/unary.o core/binary.o core/vary.o\
 core/sock.o core/error.o core/math.o core/cmp.o core/items.o core/logic.o core/compose.o core/order.o core/io.o\
 core/misc.o core/freelist.o core/update.o core/join.o core/query.o core/cond.o\
 core/iter.o core/dynlib.o core/aggr.o core/index.o core/group.o core/filter.o core/atomic.o\
 core/thread.o core/pool.o core/progress.o core/term.o core/fdmap.o core/signal.o core/log.o
APP_OBJECTS = app/main.o
TESTS_OBJECTS = tests/main.o
BENCH_OBJECTS = bench/main.o
TARGET = rayforce
CFLAGS = $(RELEASE_CFLAGS)
GIT_HASH := $(shell git rev-parse HEAD 2>/dev/null || echo "unknown")

default: debug

all: default

obj: $(CORE_OBJECTS)

app: $(APP_OBJECTS) obj
	$(CC) $(CFLAGS) -o $(TARGET) $(CORE_OBJECTS) $(APP_OBJECTS) $(LIBS) $(LDFLAGS)

tests: -DSTOP_ON_FAIL=$(STOP_ON_FAIL) -DDEBUG
tests: $(TESTS_OBJECTS) lib
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(TESTS_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LDFLAGS)
	./$(TARGET).test

bench: CC = gcc
bench: CFLAGS = $(RELEASE_CFLAGS)
bench: $(BENCH_OBJECTS) lib
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).bench $(BENCH_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LDFLAGS)
	BENCH=$(BENCH) ./$(TARGET).bench

%.o: %.c
	$(CC) -include core/def.h -c $^ $(CFLAGS) -DGIT_HASH=\"$(GIT_HASH)\" -o $@

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
coverage: CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 --coverage -fprofile-update=atomic
coverage: $(TESTS_OBJECTS) coverage-lib
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(TESTS_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LDFLAGS)
	lcov --directory . --zerocounters
	./$(TARGET).test
	lcov --capture --directory . --output-file coverage.info --ignore-errors unused
	lcov --remove coverage.info '/usr/*' 'tests/*' --output-file coverage.info --ignore-errors unused
	lcov --list coverage.info
	genhtml coverage.info --output-directory coverage_report
	@echo "Coverage report generated in coverage_report/index.html"

coverage-lib: CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 --coverage -fprofile-update=atomic
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

shared: CFLAGS = $(RELEASE_CFLAGS)
shared: LDFLAGS = $(RELEASE_LDFLAGS)
shared: $(CORE_OBJECTS)
	$(CC) -shared -o $(LIBNAME) $(CFLAGS) $(CORE_OBJECTS) $(LIBS) $(LDFLAGS)

clean:
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
	-rm -f tests/*.gcno tests/*.gcda tests/*.gcov
	-rm -f core/*.gcno core/*.gcda core/*.gcov
	-rm -f coverage.info
	-rm -rf coverage_report/
	-rm -f .DS_Store # macOS
	-rm -f core/*.opt.yaml
	-rm -f app/*.opt.yaml
	-rm -f tests/*.opt.yaml
	-rm -f bench/*.opt.yaml

# trigger github to make a nightly build
nightly:
	git push origin :nightly
	git tag -d nightly
	git tag nightly
	git push origin nightly
