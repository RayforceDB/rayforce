CC=clang
CFLAGS = -Wall -Wextra -Werror -Wpedantic -std=c17 -g -O3 -march=native
HEADERS = storm.h
OBJECTS = storm.o main.o
TARGET = stormdb

all: $(TARGET)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) $< -c

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@

clean:
	-rm -f *.o
	-rm -f $(TARGET)