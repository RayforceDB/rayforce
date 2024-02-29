CC = clang
AR = ar
# RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=c17 -Ofast -march=native -g -pg
RELEASE_CFLAGS = -fPIC -Wall -Wpedantic -Wextra -std=c17 -Ofast -march=native -fassociative-math -ftree-vectorize\
 -fno-math-errno -funsafe-math-optimizations -ffinite-math-only -funroll-loops -m64
# DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 -DDEBUG -DSYS_MALLOC
DEBUG_CFLAGS = -fPIC -Wall -Wpedantic -Wextra -std=c17 -g -O0 -DDEBUG -m64 -fsanitize=undefined -fsanitize=address
CORE_HEADERS = core/poll.h core/sys.h core/fs.h core/mmap.h core/serde.h core/timestamp.h\
 core/guid.h core/sort.h core/ops.h core/util.h core/string.h core/hash.h core/symbols.h\
 core/format.h core/rayforce.h core/heap.h core/runtime.h core/parse.h core/eval.h core/nfo.h\
 core/env.h core/lambda.h core/unary.h core/binary.h core/vary.h core/sock.h core/error.h\
 core/math.h core/rel.h core/items.h core/logic.h core/compose.h core/order.h core/io.h\
 core/misc.h core/queue.h core/freelist.h core/update.h core/join.h core/query.h core/cond.h\
 core/iter.h core/dynlib.h core/aggr.h core/index.h core/group.h core/filter.h core/timer.h
CORE_OBJECTS = core/poll.o core/sys.o core/fs.o core/mmap.o core/serde.o core/timestamp.o\
 core/guid.o core/sort.o core/ops.o core/util.o core/string.o core/hash.o core/symbols.o\
 core/format.o core/rayforce.o core/heap.o core/runtime.o core/parse.o core/eval.o core/nfo.o\
 core/env.o core/lambda.o core/unary.o core/binary.o core/vary.o core/sock.o core/error.o\
 core/math.o core/rel.o core/items.o core/logic.o core/compose.o core/order.o core/io.o\
 core/misc.o core/queue.o core/freelist.o core/update.o core/join.o core/query.o core/cond.o\
 core/iter.o core/dynlib.o core/aggr.o core/index.o core/group.o core/filter.o core/timer.o
APP_OBJECTS = app/main.o
TESTS_OBJECTS = tests/main.o
TARGET = rayforce
# windows flags
# LIBS = -lm -lws2_32 -lkernel32
# nix flags
LIBS = -lm
CFLAGS = $(DEBUG_CFLAGS)
LFLAGS = -rdynamic

default: debug

all: default

app: $(APP_OBJECTS) lib
	$(CC) $(CFLAGS) -o $(TARGET) $(CORE_OBJECTS) $(APP_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LFLAGS)

tests: CFLAGS = $(RELEASE_CFLAGS)
tests: $(TESTS_OBJECTS) lib
	$(CC) $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(TESTS_OBJECTS) -L. -l$(TARGET) $(LIBS) $(LFLAGS)
	./$(TARGET).test

%.o: %.c
	$(CC) -c $^ $(CFLAGS) -o $@

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
	$(CC) $(CFLAGS) -shared -fPIC -o lib$(TARGET).so $(CORE_OBJECTS) $(LIBS) $(LFLAGS)

chkleak: CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 -DDEBUG -DSYS_MALLOC
chkleak: CC = gcc
chkleak: app
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET)

# wasm: CFLAGS = -O0
wasm: CFLAGS = -fPIC -Wall -std=c17 -O3 -msimd128 -fassociative-math -ftree-vectorize -fno-math-errno -funsafe-math-optimizations -ffinite-math-only -funroll-loops -DSYS_MALLOC
wasm: CC = emcc 
wasm: AR = emar
wasm: $(APP_OBJECTS) lib
	$(CC) $(CFLAGS) -o $(TARGET).js $(CORE_OBJECTS) \
	-s "EXPORTED_FUNCTIONS=['_main', '_version', '_null', '_drop', '_clone', '_disp', '_parseval', '_obj_fmt']" \
	-s "EXPORTED_RUNTIME_METHODS=['ccall', 'cwrap']" -s ALLOW_MEMORY_GROWTH=1 -L. -l$(TARGET) $(LIBS)

clean:
	-rm -f *.o
	-rm -f core/*.o
	-rm -f app/*.o
	-rm -rf tests/*.o
	-rm -f $(TARGET).*
	-rm -f lib$(TARGET).a
	-rm -f core/*.gch
	-rm -f app/*.gch
	-rm -f $(TARGET).S
	-rm -f $(TARGET).test
	-rm -rf *.out
	-rm -rf *.so
	-rm -f $(TARGET).js
	-rm -f $(TARGET).wasm

# trigger github to make a nightly build
nightly:
	git push origin :nightly
	git tag -d nightly
	git tag nightly
	git push origin nightly