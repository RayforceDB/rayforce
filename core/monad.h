#ifndef MONAD_H
#define MONAD_H

#include "storm.h"

// Monadic functions
value_t til(i64_t count);

u8_t *compile(value_t value);
value_t storm_add(value_t a, value_t b);
#endif
