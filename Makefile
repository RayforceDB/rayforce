CC = clang
STD = c17
AR = ar
PROFILER = gprof

ifeq ($(OS),)
OS := $(shell uname -s | tr "[:upper:]" "[:lower:]")
endif

$(info OS="$(OS)")

ifeq ($(OS),Windows_NT)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 -DDEBUG
LIBS = -lm -lws2_32 -lkernel32
endif

ifeq ($(OS),linux)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -g -O0 -march=native -fsigned-char -DDEBUG -m64 -fsanitize=undefined -fsanitize=address
LIBS = -lm -ldl -lpthread
endif

ifeq ($(OS),darwin)
DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -g -O0 -fsigned-char -DDEBUG -m64 -fsanitize=undefined -fsanitize=address
LIBS = -lm -ldl -lpthread
endif

RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=$(STD) -Ofast -fsigned-char -march=native -fassociative-math -ftree-vectorize\
 -fno-math-errno -funsafe-math-optimizations -ffinite-math-only -funroll-loops -fno-unwind-tables -m64
CORE_HEADERS = core/poll.h core/runtime.h core/sys.h core/fs.h core/mmap.h core/serde.h\
 core/timestamp.h core/guid.h core/sort.h core/ops.h core/util.h core/string.h core/hash.h core/symbols.h\
 core/format.h core/rayforce.h core/heap.h core/parse.h core/eval.h core/nfo.h core/time.h\
 core/env.h core/lambda.h core/unary.h core/binary.h core/vary.h core/sock.h core/error.h\
 core/math.h core/cmp.h core/items.h core/logic.h core/compose.h core/order.h core/io.h\
 core/misc.h core/queue.h core/freelist.h core/update.h core/join.h core/query.h core/cond.h\
 core/iter.h core/dynlib.h core/aggr.h core/index.h core/group.h core/filter.h core/atomic.h\
 core/thread.h core/pool.h core/term.h
CORE_OBJECTS = core/poll.o core/runtime.o core/sys.o core/fs.o core/mmap.o core/serde.o core/timestamp.o\
 core/guid.o core/sort.o core/ops.o core/util.o core/string.o core/hash.o core/symbols.o\
 core/format.o core/rayforce.o core/heap.o core/parse.o core/eval.o core/nfo.o core/time.o\
 core/env.o core/lambda.o core/unary.o core/binary.o core/vary.o core/sock.o core/error.o\
 core/math.o core/cmp.o core/items.o core/logic.o core/compose.o core/order.o core/io.o\
 core/misc.o core/queue.o core/freelist.o core/update.o core/join.o core/query.o core/cond.o\
 core/iter.o core/dynlib.o core/aggr.o core/index.o core/group.o core/filter.o core/atomic.o\
 core/thread.o core/pool.o core/term.o
APP_OBJECTS = app/main.o
TESTS_OBJECTS = tests/main.o
TARGET = rayforce
CFLAGS = $(DEBUG_CFLAGS)
PYTHON = python3.10

default: debug

all: default

obj: $(CORE_OBJECTS)

app: $(APP_OBJECTS) obj
	$(CC) $(CFLAGS) -o $(TARGET) $(CORE_OBJECTS) $(APP_OBJECTS) $(LIBS) $(LFLAGS)

tests: CC = gcc
# tests: CFLAGS = $(DEBUG_CFLAGS)
tests: CFLAGS = $(RELEASE_CFLAGS) -DSTOP_ON_FAIL=$(STOP_ON_FAIL) -DDEBUG
tests: $(TESTS_OBJECTS) lib
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(TESTS_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LFLAGS)
	./$(TARGET).test

%.o: %.c
	$(CC) -include core/def.h -c $^ $(CFLAGS) -o $@

lib: CFLAGS = $(DEBUG_CFLAGS) -DSYS_MALLOC
#lib: CFLAGS = $(RELEASE_CFLAGS) -DSYS_MALLOC
lib: $(CORE_OBJECTS)
	$(AR) rc lib$(TARGET).a $(CORE_OBJECTS)

disasm: release
	objdump -d $(TARGET) -l > $(TARGET).S

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: app

release: CFLAGS = $(RELEASE_CFLAGS) 
release: app

dll: CFLAGS = $(RELEASE_CFLAGS)
dll: $(CORE_OBJECTS)
	$(CC) -include core/def.h $(CFLAGS) -shared -fPIC -o lib$(TARGET).so $(CORE_OBJECTS) $(LIBS) $(LFLAGS)

chkleak: CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 -DDEBUG -DSYS_MALLOC
chkleak: CC = gcc
chkleak: TARGET_ARGS =
chkleak: app
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET) $(TARGET_ARGS)

# Example: make clean && make profile SCRIPT=examples/update.rfl
profile: CC = gcc
profile: CFLAGS = -fPIC -Wall -Wextra -std=c17 -Ofast -march=native -g -pg
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

python: CFLAGS = $(RELEASE_CFLAGS)
python: LFLAGS = -rdynamic
python: $(CORE_OBJECTS)
	swig -python $(TARGET).i
	$(CC) -include core/def.h $(CFLAGS) -shared -fPIC $(CORE_OBJECTS) $(TARGET)_wrap.c -o _$(TARGET).so -I/usr/include/$(PYTHON) -l$(PYTHON) $(LIBS) $(LFLAGS)

clean:
	-rm -f *.o
	-rm -f core/*.o
	-rm -f app/*.o
	-rm -rf tests/*.o
	-rm -f lib$(TARGET).a
	-rm -f core/*.gch
	-rm -f app/*.gch
	-rm -f $(TARGET).S
	-rm -f $(TARGET).test
	-rm -rf *.out
	-rm -rf *.so
	-rm -f $(TARGET).js
	-rm -f $(TARGET).wasm
	-rm -rf __pycache__/
	-rm -f $(TARGET)_wrap.*
	-rm -f $(TARGET).py

# trigger github to make a nightly build
nightly:
	git push origin :nightly
	git tag -d nightly
	git tag nightly
	git push origin nightly