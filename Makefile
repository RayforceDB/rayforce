CC=gcc
AR=ar
# RELEASE_CFLAGS = -fPIC -Wall -Wextra -Werror -Wpedantic -std=c17 -O3 -march=native
RELEASE_CFLAGS = -fPIC -Wall -Wextra -Wpedantic -std=c17 -Ofast -march=native
# RELEASE_CFLAGS = -fPIC -Wall -Wextra -Wpedantic -std=c17 -Ofast -march=native -g -pg
DEBUG_CFLAGS =  -fPIC -Wall -Wextra -std=c17 -g -O0 -DDEBUG
CORE_HEADERS = core/util.h core/vector.h core/string.h core/mmap.h core/hash.h\
 core/symbols.h core/format.h core/rayforce.h core/monad.h core/alloc.h core/runtime.h\
 core/dict.h core/parse.h core/vm.h core/debuginfo.h core/cc.h
APP_HEADERS = app/parse.h
CORE_OBJECTS = core/vector.o core/string.o core/hash.o core/symbols.o\
 core/alloc.o core/format.o core/monad.o core/rayforce.o core/parse.o\
 core/runtime.o core/dict.o core/vm.o core/debuginfo.o core/cc.o
APP_OBJECTS = app/main.o
TESTS_OBJECTS = app/tests.o
TARGET = rayforce

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