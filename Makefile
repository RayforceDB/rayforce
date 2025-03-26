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
endif

ifeq ($(OS),linux)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -g -O0 -march=native -fsigned-char -DDEBUG -m64
LIBS = -lm -ldl -lpthread
# These should be used if you want to use plugins
# LIBS = -lm -ldl -lpthread -rdynamic 
endif

ifeq ($(OS),darwin)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -Wunused-function -std=$(STD) -g -O0 -march=native -fsigned-char -DDEBUG -m64 -fsanitize=undefined -fsanitize=address
LIBS = -lm -ldl -lpthread
endif

RELEASE_CFLAGS = -fPIC -Wall -Wextra -Wunused-function -std=$(STD) -O3 -fsigned-char -march=native -fassociative-math -ftree-vectorize\
 -fno-math-errno -funsafe-math-optimizations -funroll-loops -fno-unwind-tables -m64
CORE_HEADERS = core/poll.h core/runtime.h core/sys.h core/os.h core/proc.h core/fs.h core/mmap.h core/serde.h\
 core/temporal.h core/date.h core/time.h core/timestamp.h core/guid.h core/sort.h core/ops.h core/util.h\
 core/string.h core/hash.h core/symbols.h core/format.h core/rayforce.h core/heap.h core/parse.h\
 core/eval.h core/nfo.h core/chrono.h core/env.h core/lambda.h core/unary.h core/binary.h core/vary.h\
 core/sock.h core/error.h core/math.h core/cmp.h core/items.h core/logic.h core/compose.h core/order.h core/io.h\
 core/misc.h core/queue.h core/freelist.h core/update.h core/join.h core/query.h core/cond.h\
 core/iter.h core/dynlib.h core/aggr.h core/index.h core/group.h core/filter.h core/atomic.h\
 core/thread.h core/pool.h core/progress.h core/term.h core/fdmap.h core/signal.h
CORE_OBJECTS = core/poll.o core/runtime.o core/sys.o core/os.o core/proc.o core/fs.o core/mmap.o core/serde.o\
 core/temporal.o core/date.o core/time.o core/timestamp.o core/guid.o core/sort.o core/ops.o core/util.o\
 core/string.o core/hash.o core/symbols.o core/format.o core/rayforce.o core/heap.o core/parse.o\
 core/eval.o core/nfo.o core/chrono.o core/env.o core/lambda.o core/unary.o core/binary.o core/vary.o\
 core/sock.o core/error.o core/math.o core/cmp.o core/items.o core/logic.o core/compose.o core/order.o core/io.o\
 core/misc.o core/queue.o core/freelist.o core/update.o core/join.o core/query.o core/cond.o\
 core/iter.o core/dynlib.o core/aggr.o core/index.o core/group.o core/filter.o core/atomic.o\
 core/thread.o core/pool.o core/progress.o core/term.o core/fdmap.o core/signal.o
APP_OBJECTS = app/main.o
TESTS_OBJECTS = tests/main.o
BENCH_OBJECTS = bench/main.o
TARGET = rayforce
CFLAGS = $(RELEASE_CFLAGS)
PYTHON = python3.10

default: debug

all: default

obj: $(CORE_OBJECTS)

app: $(APP_OBJECTS) obj
	$(CC) $(CFLAGS) -o $(TARGET) $(CORE_OBJECTS) $(APP_OBJECTS) $(LIBS) $(LFLAGS)

tests: CC = gcc
# tests: CFLAGS = $(DEBUG_CFLAGS)
tests: -DSTOP_ON_FAIL=$(STOP_ON_FAIL) -DDEBUG
tests: $(TESTS_OBJECTS) lib
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(TESTS_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LFLAGS)
	./$(TARGET).test

bench: CC = gcc
bench: CFLAGS = $(RELEASE_CFLAGS)
bench: $(BENCH_OBJECTS) lib
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).bench $(BENCH_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LFLAGS)
	./$(TARGET).bench

%.o: %.c
	$(CC) -include core/def.h -c $^ $(CFLAGS) -o $@

lib: CFLAGS = $(RELEASE_CFLAGS)
lib: $(CORE_OBJECTS)
	$(AR) rc lib$(TARGET).a $(CORE_OBJECTS)

lib-debug: CFLAGS = $(DEBUG_CFLAGS) -DSYS_MALLOC
lib-debug: $(CORE_OBJECTS)
	$(AR) rc lib$(TARGET).a $(CORE_OBJECTS)

disasm: release
	objdump -d $(TARGET) -l > $(TARGET).S

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: app

release: CFLAGS = $(RELEASE_CFLAGS) 
release: app

chkleak: CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 -DDEBUG -DSYS_MALLOC
chkleak: CC = gcc
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

wasm: CFLAGS = -fPIC -Wall -std=c17 -O3 -msimd128 -fassociative-math -ftree-vectorize -fno-math-errno -funsafe-math-optimizations -ffinite-math-only -funroll-loops -DSYS_MALLOC
wasm: CC = emcc 
wasm: AR = emar
wasm: $(APP_OBJECTS) lib
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).js $(CORE_OBJECTS) \
	-s "EXPORTED_FUNCTIONS=['_main', '_version', '_null', '_drop_obj', '_clone_obj', '_eval_str', '_obj_fmt', '_strof_obj']" \
	-s "EXPORTED_RUNTIME_METHODS=['ccall', 'cwrap', 'FS']" -s ALLOW_MEMORY_GROWTH=1 \
	--preload-file examples@/examples \
	-L. -l$(TARGET) $(LIBS)

shared: CFLAGS += -fPIC
shared: $(CORE_OBJECTS)
	$(CC) -shared -o librayforce.so $(CORE_OBJECTS) $(LIBS)

python: shared
	cd python && python3 setup.py build_ext --inplace

.PHONY: python shared

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
	-rm -f $(TARGET).js
	-rm -f $(TARGET).wasm
	-rm -rf __pycache__/
	-rm -f $(TARGET)_wrap.*
	-rm -f $(TARGET).py
	-rm -f $(TARGET)
	# Python-specific cleanup
	-rm -rf python/build/
	-rm -rf python/dist/
	-rm -rf python/*.egg-info/
	-rm -rf python/rayforce/__pycache__/
	-rm -f python/rayforce/*.so
	-rm -f python/rayforce/*.pyc
	-rm -f python/rayforce/*.pyo
	-rm -f python/rayforce/*.pyd

# trigger github to make a nightly build
nightly:
	git push origin :nightly
	git tag -d nightly
	git tag nightly
	git push origin nightly