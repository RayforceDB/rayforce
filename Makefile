CC = clang
AR = ar
# RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=c17 -Ofast -march=native -g -pg
RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=c17 -Ofast -march=native -fassociative-math -ftree-vectorize -m64
# DEBUG_CFLAGS =  -fPIC -Wall -Wextra -std=c17 -g -O0 -DDEBUG
DEBUG_CFLAGS = -fPIC -Wall -Wextra -std=c17 -g -O0 -DDEBUG -m64 -fno-omit-frame-pointer\
 -fsanitize=undefined -fsanitize=address
CORE_HEADERS =  core/poll.h core/sys.h core/fs.h core/mmap.h core/serde.h core/timestamp.h\
 core/guid.h core/sort.h core/ops.h core/util.h core/string.h core/hash.h core/symbols.h\
 core/format.h core/rayforce.h core/heap.h core/runtime.h core/parse.h core/vm.h core/nfo.h\
 core/cc.h core/env.h core/lambda.h core/unary.h core/binary.h core/vary.h core/sock.h\
 core/math.h core/rel.h core/items.h core/logic.h core/compose.h core/order.h core/io.h\
 core/misc.h core/queue.h core/freelist.h core/amend.h core/join.h
CORE_OBJECTS = core/poll.o core/sys.o core/fs.o core/mmap.o core/serde.o core/timestamp.o\
 core/guid.o core/sort.o core/ops.o core/util.o core/string.o core/hash.o core/symbols.o\
 core/heap.o core/format.o core/rayforce.o core/parse.o core/runtime.o core/vm.o core/nfo.o\
 core/cc.o core/env.o core/lambda.o core/unary.o core/binary.o core/vary.o core/sock.o core/math.o\
 core/rel.o core/items.o core/logic.o core/compose.o core/order.o core/io.o core/misc.o\
 core/queue.o core/freelist.o core/amend.o core/join.o
APP_OBJECTS = app/main.o
TESTS_OBJECTS = tests/main.o
TARGET = rayforce
# windows flags
# LIBS = -lm -lws2_32 -lkernel32
# nix flags
LIBS = -lm
CFLAGS = $(DEBUG_CFLAGS)

default: debug

all: default

app: $(APP_OBJECTS) lib
	$(CC) $(CFLAGS) -o $(TARGET) $(CORE_OBJECTS) $(APP_OBJECTS) -L. -l$(TARGET) $(LIBS)

tests: CFLAGS = $(RELEASE_CFLAGS)
tests: $(TESTS_OBJECTS) lib
	$(CC) $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(TESTS_OBJECTS) -L. -l$(TARGET) $(LIBS)
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