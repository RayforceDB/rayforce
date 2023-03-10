#ifndef STRING_H
#define STRING_H

#include "storm.h"

/*
 * String type containing length and pointer to string.
 * Does not require string to be null terminated
 */
typedef struct string_t
{
    u64_t len;
    str_t str;
} string_t;

string_t string_create(str_t str, u64_t len);

#endif
