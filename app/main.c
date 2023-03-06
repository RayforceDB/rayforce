#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include "../core/storm.h"
#include "../core/format.h"
#include "../core/monad.h"
#include "../core/alloc.h"
#include "../core/vm.h"
#include "parse.h"
#include <time.h>

#include <stdlib.h>
#define LINE_SIZE 2048

int main()
{
    storm_alloc_init();

    int run = 1;
    str_t line = (char *)storm_malloc(LINE_SIZE);
    str_t ptr;
    value_t value;
    vm_t vm;
    u8_t *code;

    while (run)
    {

        vm = vm_create();

        printf(">");
        ptr = fgets(line, LINE_SIZE, stdin);
        UNUSED(ptr);

        int c = atoi(line);

        value_t v1 = til(c);
        value_t v2 = new_scalar_i64(c);

        clock_t t;
        t = clock();
        value = storm_add(v1, v2);
        t = clock() - t;

        printf("Time taken: %fms\n", ((double)t) / CLOCKS_PER_SEC * 1000);

        str_t f = value_fmt(value);
        printf("res: %s\n", f);

        str_t a = str_fmt("%s", "hello");
        printf("a: %s\n", a);

        value_free(v1);
        value_free(v2);

        // value = parse("REPL", line);
        // code = compile(value);
        // vm_exec(vm, code);

        // value_free(value);
        // vm_free(vm);
    }

    storm_alloc_deinit();

    return 0;
}
