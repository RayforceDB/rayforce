CC      ?= clang
STD     = c17
AR      = ar
TARGET  = rayforce
# Version is authoritative in include/rayforce.h — extract it here
VERSION_MAJOR := $(shell grep 'RAY_VERSION_MAJOR' include/rayforce.h | head -1 | awk '{print $$3}')
VERSION_MINOR := $(shell grep 'RAY_VERSION_MINOR' include/rayforce.h | head -1 | awk '{print $$3}')
VERSION_PATCH := $(shell grep 'RAY_VERSION_PATCH' include/rayforce.h | head -1 | awk '{print $$3}')
VERSION       = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date -u +%Y-%m-%d)

WARNS   = -Wall -Wextra -Werror -Wstrict-prototypes -Wno-unused-parameter
DEFS    = -DRAYFORCE_GIT_COMMIT=\"$(GIT_HASH)\" -DRAYFORCE_BUILD_DATE=\"$(BUILD_DATE)\"
INCLUDES = -Iinclude -Isrc

UNAME_S := $(shell uname -s)

DEBUG_CFLAGS   = -fPIC $(WARNS) -std=$(STD) -g -O0 -march=native -DDEBUG \
  -fsanitize=address,undefined -fno-omit-frame-pointer
RELEASE_CFLAGS = -fPIC $(WARNS) -std=$(STD) -O3 -march=native \
  -funroll-loops -fomit-frame-pointer -fno-math-errno

ifeq ($(UNAME_S),Linux)
  LIBS            = -lm -lpthread
  RELEASE_LDFLAGS = -Wl,--gc-sections -Wl,--as-needed
else
  LIBS            = -lm
  RELEASE_LDFLAGS = -Wl,-dead_strip
endif

DEBUG_LDFLAGS   = -fsanitize=address,undefined

CFLAGS  = $(DEBUG_CFLAGS)
LDFLAGS = $(DEBUG_LDFLAGS)

# Sources
LIB_SRC  = $(wildcard src/*/*.c)
LIB_SRC := $(filter-out src/app/main.c, $(LIB_SRC))
LIB_OBJ  = $(LIB_SRC:.c=.o)
MAIN_SRC = src/app/main.c
MAIN_OBJ = $(MAIN_SRC:.c=.o)
TEST_SRC = $(wildcard test/*.c)
TEST_OBJ = $(TEST_SRC:.c=.o)

# Default target
default: debug

%.o: %.c
	$(CC) -c $(CFLAGS) $(DEFS) $(INCLUDES) -o $@ $<

# Debug build
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = $(DEBUG_LDFLAGS)
debug: $(LIB_OBJ) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(LIB_OBJ) $(MAIN_OBJ) $(LIBS) $(LDFLAGS)

# Release build
release: CFLAGS = $(RELEASE_CFLAGS)
release: LDFLAGS = $(RELEASE_LDFLAGS)
release: $(LIB_OBJ) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(LIB_OBJ) $(MAIN_OBJ) $(LIBS) $(LDFLAGS)

# Static library
lib: CFLAGS = $(RELEASE_CFLAGS)
lib: $(LIB_OBJ)
	$(AR) rc lib$(TARGET).a $(LIB_OBJ)

# Tests
test: CFLAGS = $(DEBUG_CFLAGS)
test: LDFLAGS = $(DEBUG_LDFLAGS)
test: $(LIB_OBJ) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET).test $(LIB_OBJ) $(TEST_OBJ) $(LIBS) $(LDFLAGS) -Itest
	./$(TARGET).test

clean:
	-rm -f $(LIB_OBJ) $(MAIN_OBJ) $(TEST_OBJ)
	-rm -f $(TARGET) $(TARGET).test lib$(TARGET).a
	-rm -rf build build_release
	# Test-generated fixtures (see test/rfl/system/*.rfl) — should not linger after a run.
	-rm -f rf_test_*.csv

.PHONY: default debug release lib test clean
