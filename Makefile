CC=clang
AR=ar
# RELEASE_CFLAGS = -fPIC -Wall -Wextra -Werror -Wpedantic -std=c17 -O3 -march=native
RELEASE_CFLAGS = -fPIC -Wall -Wextra -Wpedantic -std=c17 -O3 -march=native
# RELEASE_CFLAGS = -fPIC -Wall -Wextra -Wpedantic -std=c17 -O3 -march=native -g -pg
DEBUG_CFLAGS =  -fPIC -Wall -Wextra -std=c17 -g -O0
# CFLAGS = $(DEBUG_CFLAGS)
CFLAGS = $(RELEASE_CFLAGS)
CORE_HEADERS = core/string.h core/mmap.h core/hash.h core/symbols.h\
 core/format.h core/storm.h core/monad.h core/alloc.h core/vm.h
APP_HEADERS = app/parse.h
CORE_OBJECTS = core/string.o core/hash.o core/symbols.o core/alloc.o\
 core/format.o core/monad.o core/storm.o core/vm.o
APP_OBJECTS = app/parse.o app/main.o
TESTS_OBJECTS = app/parse.o app/tests.o
TARGET = stormdb

app: $(APP_OBJECTS) lib
	$(CC) $(CFLAGS) -o $(TARGET) $(CORE_OBJECTS) $(APP_OBJECTS) -L. -l$(TARGET) 

tests: $(TESTS_OBJECTS) lib
	$(CC) $(CFLAGS) -o $(TARGET).test $(CORE_OBJECTS) $(TESTS_OBJECTS) -L. -l$(TARGET) 
	./$(TARGET).test

%.o: %.c
	$(CC) -c $^ $(CFLAGS) -o $@

lib: $(CORE_OBJECTS)
	$(AR) rc lib$(TARGET).a $(CORE_OBJECTS)

disasm: app
	objdump -d $(TARGET) -l > $(TARGET).S

default: app

all: default

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