CC = clang
UNAME_S := $(shell uname -s)

# Windows detection (same pattern as main Makefile)
ifeq ($(OS),)
OS := $(shell uname -s | tr "[:upper:]" "[:lower:]")
endif
IS_WINDOWS := $(filter Windows_NT,$(OS))$(findstring mingw,$(OS))$(findstring msys,$(OS))

SRCS = raykx.c serde.c types.c
CFLAGS = -Wall -Wextra -std=c17 -fsigned-char -m64 -I. -I../../core

ifneq (,$(IS_WINDOWS))
    TARGET = raykx.dll
    # On Windows, exports via __declspec(dllexport) in raykx.h
    TEMP ?= /tmp
    export TEMP
    LDFLAGS = -shared -fuse-ld=lld
    LIBS = -lws2_32 -lmswsock ../../rayforce.lib
    DEBUG_CFLAGS = $(CFLAGS) -g -O0 -DDEBUG -D_CRT_SECURE_NO_WARNINGS
    RELEASE_CFLAGS = $(CFLAGS) -O3 -D_CRT_SECURE_NO_WARNINGS
else ifeq ($(findstring Darwin,$(UNAME_S)),Darwin)
    TARGET = libraykx.dylib
    LDFLAGS = -shared -fPIC -undefined dynamic_lookup
    LIBS = -lm -ldl -lpthread
    DEBUG_CFLAGS = $(CFLAGS) -fPIC -g -O0 -DDEBUG -march=native
    RELEASE_CFLAGS = $(CFLAGS) -fPIC -O3 -march=native -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -ffast-math -flax-vector-conversions -fno-math-errno
else
    TARGET = libraykx.so
    LDFLAGS = -shared -fPIC
    LIBS = -lm -ldl -lpthread
    DEBUG_CFLAGS = $(CFLAGS) -fPIC -g -O0 -DDEBUG -march=native
    RELEASE_CFLAGS = $(CFLAGS) -fPIC -O3 -march=native -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -ffast-math -flax-vector-conversions -fno-math-errno
endif

default: debug

debug:
	$(CC) -include ../../core/def.h $(DEBUG_CFLAGS) $(LDFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

release:
	$(CC) -include ../../core/def.h $(RELEASE_CFLAGS) $(LDFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET) libraykx.so libraykx.dylib raykx.dll
