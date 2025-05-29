CC = clang
TARGET = libraykx.so
SRCS = raykx.c serde.c types.c
CFLAGS = -fPIC -Wall -Wextra -std=c17 -O3 -fsigned-char -mavx2 -mfma -mpclmul -mbmi2\
 -fassociative-math -ftree-vectorize -funsafe-math-optimizations -funroll-loops -ffast-math -m64\
 -flax-vector-conversions -fno-math-errno -m64 -I.
LIBS = -lm -ldl -lpthread

default: debug

debug:
	$(CC) $(CFLAGS) -shared -fPIC -o $(TARGET) $(SRCS) -DDEBUG $(LIBS)

release:
	$(CC) $(CFLAGS) -shared -fPIC -o $(TARGET) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)
	