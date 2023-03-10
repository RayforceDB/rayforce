#ifndef SYMBOLS_H
#define SYMBOLS_H

#include "storm.h"
#include "hash.h"
#include "mmap.h"
#include "string.h"

#define STRINGS_POOL_SIZE 4096

/*
Holds memory for strings pool as a node in a linked list
*/
typedef struct pool_node_t
{
    struct pool_node_t *next;
} pool_node_t;

/*
Intern symbols here. Assume symbols are never freed.
*/
typedef struct symbols_t
{
    hash_table_t *str_to_id;
    hash_table_t *id_to_str;
    pool_node_t *pool_node_0;
    pool_node_t *pool_node;
    str_t strings_pool;
} symbols_t;

i64_t symbols_intern(string_t str);
str_t symbols_get(i64_t key);

symbols_t *symbols_create();
null_t symbols_free(symbols_t *symbols);

#endif
