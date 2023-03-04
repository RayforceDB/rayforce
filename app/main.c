#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include "../core/storm.h"
#include "../core/format.h"
#include "../core/monad.h"
#include "../core/alloc.h"
#include "parse.h"

#define LINE_SIZE 2048

int main()
{
    storm_alloc_init();

    int run = 1;
    char *line = (char *)storm_malloc(LINE_SIZE);
    char *ptr;
    i8_t res;
    value_t value;

    while (run)
    {

        printf(">");
        ptr = fgets(line, LINE_SIZE, stdin);
        UNUSED(ptr);
        value = parse("REPL", line);

        res = value_fmt(&line, value);
        if (res == OK)
            printf("%s\n", line);

        value_free(value);
    }

    storm_alloc_deinit();

    return 0;
}
