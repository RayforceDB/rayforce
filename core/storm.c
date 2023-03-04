#include "storm.h"
#include "format.h"
#include <stdio.h>
#include "alloc.h"

extern value_t new_error(i8_t code, str_t message)
{
    value_t error;

    error = storm_malloc(sizeof(struct value_t));
    error->type = TYPE_ERR;
    error->error_value.code = code;
    error->error_value.message = message;
    return error;
}

extern value_t new_scalar_i64(i64_t value)
{
    value_t scalar;

    scalar = storm_malloc(sizeof(struct value_t));
    scalar->type = -TYPE_i64;
    scalar->i64_t_value = value;
    return scalar;
}

extern value_t new_vector_i64(i64_t *ptr, i64_t len)
{
    value_t vector;

    vector = storm_malloc(sizeof(struct value_t));
    vector->type = TYPE_i64;
    vector->list_value.ptr = ptr;
    vector->list_value.len = len;
    return vector;
}

extern nil_t value_free(value_t value)
{
    switch (value->type)
    {
    case TYPE_i64:
    {
        storm_free(value->list_value.ptr);
        break;
    }
    default:
    {
        printf("** Free: Invalid type\n");
        break;
    }
    }
    storm_free(value);
}
