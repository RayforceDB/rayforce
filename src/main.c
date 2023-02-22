#define SIZE 2

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "storm.h"

typedef long long ssize_t;

typedef struct vec
{
    float data[SIZE];
} vec;

vec add(vec a, vec b)
{
    vec result;
    for (int i = 0; i < SIZE; ++i)
    {
        result.data[i] = a.data[i] + b.data[i];
    }
    return result;
}

int main()
{
    // FILE *stream;
    // char *line = NULL;
    // size_t len = 0;
    // ssize_t nread;

    // while ((nread = getline(&line, &len, stream)) != -1)
    // {
    //     printf("Retrieved line of length %zu:\n", nread);
    //     fwrite(line, nread, 1, stdout);
    // }

    // free(line);
    // fclose(stream);
    return 0;
}
