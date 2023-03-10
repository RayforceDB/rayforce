#include "storm.h"
#include "alloc.h"
#include "vm.h"

value_t til(i64_t count)
{
    i64_t *vec;

    vec = (i64_t *)storm_malloc(count * sizeof(i64_t));
    for (i64_t i = 0; i < count; i++)
    {
        vec[i] = i;
    }
    return xi64(vec, count);
}

value_t storm_add(value_t *a, value_t *b)
{
    i64_t a_len, b_len, sum = 0;
    i64_t *a_vec;

    a_len = a->s0.len;
    a_vec = a->s0.ptr;

    sum = b->i64;

    for (i64_t i = 0; i < a_len; i++)
    {
        sum += a_vec[i];
    }

    return i64(sum);
}

u8_t *compile(value_t *value)
{
    u8_t *code;

    UNUSED(value);

    code = (u8_t *)storm_malloc(1024);

    code[0] = VM_ADD;

    return code;
}
