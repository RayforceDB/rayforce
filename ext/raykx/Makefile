CC = clang
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    TARGET = libraykx.dylib
    LDFLAGS += -undefined dynamic_lookup
else
    TARGET = libraykx.so
endif
SRCS = raykx.c serde.c types.c
CFLAGS = -fPIC -Wall -Wextra -std=c17 -O3 -fsigned-char -march=native\
 -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -ffast-math -m64\
 -flax-vector-conversions -fno-math-errno -m64 -I.
LIBS = -lm -ldl -lpthread

default: debug

debug:
	$(CC) $(CFLAGS) -shared -fPIC -o $(TARGET) $(SRCS) -DDEBUG $(LIBS) $(LDFLAGS)

release:
	$(CC) $(CFLAGS) -shared -fPIC -o $(TARGET) $(SRCS) $(LIBS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
	