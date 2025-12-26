CC = clang
STD = c17
AR = ar
PROFILER = gprof

ifeq ($(OS),)
OS := $(shell uname -s | tr "[:upper:]" "[:lower:]")
endif

$(info OS="$(OS)")

ifeq ($(OS),Windows_NT)
AR = llvm-ar
DEBUG_CFLAGS = -Wall -Wextra -std=$(STD) -g -O0 -DDEBUG -D_CRT_SECURE_NO_WARNINGS
RELEASE_CFLAGS = -Wall -Wextra -std=$(STD) -O3 -DNDEBUG -D_CRT_SECURE_NO_WARNINGS \
 -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -fno-math-errno
LIBS = -lws2_32 -lmswsock -lkernel32
DEBUG_LDFLAGS = -fuse-ld=lld
RELEASE_LDFLAGS = -fuse-ld=lld
LIBNAME = rayforce.dll
TARGET = rayforce.exe
endif

ifeq ($(OS),linux)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -g -O0 -march=native -fsigned-char -DDEBUG -m64
RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -O3 -fsigned-char -march=native\
 -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -m64\
 -flax-vector-conversions -fno-math-errno -fomit-frame-pointer -fno-stack-protector\
 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
LIBS = -lm -ldl -lpthread
RELEASE_LDFLAGS = -Wl,--strip-all -Wl,--gc-sections -Wl,--as-needed\
 -Wl,--build-id=none -Wl,--no-eh-frame-hdr -Wl,--no-ld-generated-unwind-info\
 -Wl,--dynamic-list=rayforce.syms
DEBUG_LDFLAGS = -Wl,--dynamic-list=rayforce.syms
LIBNAME = rayforce.so
TARGET = rayforce
endif

ifeq ($(OS),darwin)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -Wunused-function -std=$(STD) -g -O0 -march=native -fsigned-char -DDEBUG -m64 -fsanitize=undefined -fsanitize=address
RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -O3 -fsigned-char -march=native\
 -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -m64\
 -flax-vector-conversions -fno-math-errno -fomit-frame-pointer -fno-stack-protector\
 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
LIBS = -lm -ldl -lpthread
RELEASE_LDFLAGS = -Wl,-dead_strip -Wl,-export_dynamic
DEBUG_LDFLAGS = -Wl,-export_dynamic
LIBNAME = librayforce.dylib
TARGET = rayforce
endif

CORE_OBJECTS = core/poll.o core/ipc.o core/runtime.o core/sys.o core/os.o core/proc.o core/fs.o core/mmap.o core/serde.o\
 core/temporal.o core/date.o core/time.o core/timestamp.o core/guid.o core/sort.o core/ops.o core/util.o\
 core/string.o core/hash.o core/symbols.o core/format.o core/rayforce.o core/heap.o core/parse.o\
 core/eval.o core/cc.o core/nfo.o core/chrono.o core/env.o core/lambda.o core/unary.o core/binary.o core/vary.o\
 core/sock.o core/error.o core/math.o core/cmp.o core/items.o core/logic.o core/compose.o core/order.o core/io.o\
 core/misc.o core/freelist.o core/update.o core/join.o core/query.o core/cond.o\
 core/iter.o core/dynlib.o core/aggr.o core/index.o core/group.o core/filter.o core/atomic.o\
 core/thread.o core/pool.o core/progress.o core/fdmap.o core/signal.o core/log.o
APP_COMMON = app/repl.o app/term.o
APP_OBJECTS = app/main.o $(APP_COMMON)
TESTS_OBJECTS = tests/main.o
BENCH_OBJECTS = bench/main.o
CFLAGS = $(RELEASE_CFLAGS)
GIT_HASH := $(shell git rev-parse HEAD 2>/dev/null || echo "unknown")

default: debug

all: default

obj: $(CORE_OBJECTS)

app: $(APP_OBJECTS) obj
	$(CC) $(CFLAGS) -o $(TARGET) $(CORE_OBJECTS) $(APP_OBJECTS) $(LIBS) $(LDFLAGS)

tests: -DSTOP_ON_FAIL=$(STOP_ON_FAIL) -DDEBUG
tests: $(TESTS_OBJECTS) $(APP_COMMON) obj
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(APP_COMMON) $(TESTS_OBJECTS) $(LIBS) $(LDFLAGS)
	./$(TARGET).test

bench: CC = gcc
bench: CFLAGS = $(RELEASE_CFLAGS)
bench: $(BENCH_OBJECTS) $(APP_COMMON) obj
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).bench $(CORE_OBJECTS) $(APP_COMMON) $(BENCH_OBJECTS) $(LIBS) $(LDFLAGS)
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
	-rm -f $(TARGET).exe
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
