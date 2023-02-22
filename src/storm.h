#ifndef STORM_H
#define STORM_H

typedef char *Char;
typedef long long Int;
typedef double Float;

// #ifdef __cplusplus
// extern "C"
// {
// #endif

// Type constants
#define TYPE_NODE 0;
#define TYPE_CHAR 1;
#define TYPE_INT 2;
#define TYPE_FLOAT 3;

// Generic type
typedef struct Stype
{
    Char type_t;

    union
    {
        Char char_t;
        Int int_t;
        Float float_t;
        struct Stype *snode_t;
    };
} *Snode;

// Constructors
extern Int new_scalar_int(Int value);

// #ifdef __cplusplus
// }
// #endif

#endif
