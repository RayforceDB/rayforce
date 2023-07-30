CC = gcc
AR = ar
# RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=c17 -Ofast -march=native -g -pg -fno-omit-frame-pointer\
#  -fsanitize=undefined -fsanitize=address
RELEASE_CFLAGS = -fPIC -Wall -Wextra -std=c17 -Ofast -march=native -fassociative-math -ftree-vectorize -m64
DEBUG_CFLAGS =  -fPIC -Wall -Wextra -std=c17 -g -O0 -DDEBUG -m64 -fno-omit-frame-pointer\
 -fsanitize=undefined -fsanitize=address
CORE_HEADERS = core/fs.h core/mmap.h core/serde.h core/timestamp.h core/guid.h core/sort.h\
 core/ops.h core/util.h core/string.h core/hash.h core/symbols.h core/format.h\
 core/rayforce.h core/heap.h core/runtime.h core/parse.h core/vm.h core/nfo.h core/cc.h\
 core/env.h core/lambda.h core/unary.h core/binary.h core/vary.h
APP_HEADERS = app/parse.h
CORE_OBJECTS = core/fs.o core/serde.o core/timestamp.o core/guid.o core/sort.o core/ops.o\
 core/util.o core/string.o core/hash.o core/symbols.o core/heap.o core/format.o core/rayforce.o\
 core/parse.o core/runtime.o core/vm.o core/nfo.o core/cc.o core/env.o core/lambda.o\
 core/unary.o core/binary.o core/vary.o
APP_OBJECTS = app/main.o
TESTS_OBJECTS = app/tests.o
TARGET = rayforce
CFLAGS = $(DEBUG_CFLAGS)

default: debug

all: default

app: $(APP_OBJECTS) lib
	$(CC) $(CFLAGS) -o $(TARGET) $(CORE_OBJECTS) $(APP_OBJECTS) -L. -l$(TARGET) 

tests: $(TESTS_OBJECTS) lib
	$(CC) $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(TESTS_OBJECTS) -L. -l$(TARGET) 
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
	-rm -f $(TARGET)
	-rm -f lib$(TARGET).a
	-rm core/*.gch
	-rm app/*.gch
	-rm -f $(TARGET).S
	-rm -f $(TARGET).test
	-rm -rf *.out