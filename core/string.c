#include "string.h"

string_t string_create(str_t str, u64_t len)
{
    string_t string = {
        .len = len,
        .str = str,
    };

    return string;
}
