CC=clang
AR=ar
# RELEASE_CFLAGS = -fPIC -Wall -Wextra -Werror -Wpedantic -std=c17 -O3 -march=native
RELEASE_CFLAGS = -fPIC -Wall -Wextra -Wpedantic -std=c17 -O3 -march=native
DEBUG_CFLAGS =  -Icore -Iapp -fPIC -Wall -Wextra -std=c17 -g -O0 -march=native
# CFLAGS = $(DEBUG_CFLAGS)
CFLAGS = $(RELEASE_CFLAGS)
CORE_HEADERS = core/format.h core/storm.h core/monad.h core/alloc.h core/vm.h
APP_HEADERS = app/lex.h app/parse.h
APP_SOURCES = app/lex.c app/parse.c app/main.c
CORE_OBJECTS = core/alloc.o core/format.o core/monad.o core/storm.o core/vm.o
APP_OBJECTS = app/lex.o app/parse.o app/main.o
TARGET = stormdb

app: $(APP_OBJECTS) lib
	$(CC) $(CFLAGS) -o $(TARGET) $(CORE_OBJECTS) $(APP_OBJECTS) -L. -l$(TARGET) 

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