/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "lang/sig.h"
#include "lang/env.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* One line per builtin with a fixed argument list: the name, then each
 * argument as <name: type> in call order.  Types: int (an integer atom),
 * num (a number), vec (a vector), list (a vector or list), str, sym,
 * table, dict, fn (a function), any.  Where the order is not the usual
 * subject-first one, the line is what tells it.
 *
 * A builtin that runs other code (eval, try, load, the binding forms) is in
 * own_sigs below, not here: an error from the code it runs is not its own.
 *
 * Each entry also carries the builtin's one-line summary (#1737): what it
 * does, for .repl.signature and :h.  This file is the one source of it:
 * scripts/check-builtin-docs.py writes the Description cells of the
 * reference pages from it (make docs-gen), and CI fails when a page
 * differs.  A comment `docs: ...` after an entry is more text for the
 * builtin's row in docs/docs/reference/all-functions.md only, appended
 * after the summary; it is not compiled in. */
typedef struct { const char* name; const char* usage; const char* summary; } ray_sig_entry_t;

static const ray_sig_entry_t sigs[] = {
    /* arithmetic and comparison */
    {"+", "(+ <left: num, date, time, timestamp or timespan> <right: the same kinds>)",
     "Addition"},
    {"-", "(- <left: num, date, time, timestamp or timespan> <right: the same kinds>)",
     "Subtraction"},
    {"*", "(* <left: num or timespan> <right: num or timespan>)",
     "Multiplication"},
    {"/", "(/ <left: num or timespan> <right: num>)",
     "Float division (always returns f64)"},
    {"%", "(% <left: num> <right: num>)",
     "Modulo (remainder)"},
    {"div", "(div <left: num> <right: num>)",
     "Floor division, promoted type"},
    {"pow", "(pow <base: num> <exponent: num>)",
     "Power (x^y, returns f64; lowers in query expressions)"},
    {">", "(> <left: any> <right: a comparable value of the same kind>)",
     "Greater than"},
    {"<", "(< <left: any> <right: a comparable value of the same kind>)",
     "Less than"},
    {">=", "(>= <left: any> <right: a comparable value of the same kind>)",
     "Greater than or equal"},
    {"<=", "(<= <left: any> <right: a comparable value of the same kind>)",
     "Less than or equal"},
    {"==", "(== <left: any> <right: a value of the same kind>)",
     "Equal"},
    {"!=", "(!= <left: any> <right: a value of the same kind>)",
     "Not equal"},
    {"not", "(not <x: bool or num, atom or vector>)",
     "Logical NOT; element-wise over a BOOL, numeric or temporal vector (zero and null are not true)"},
    {"neg", "(neg <x: num, atom or vector>)",
     "Negate value"},
    {"floor", "(floor <x: num, atom or vector>)",
     "Floor (round down)"},
    {"ceil", "(ceil <x: num, atom or vector>)",
     "Ceiling (round up)"},
    {"abs", "(abs <x: num, atom or vector>)",
     "Absolute value"},
    {"sqrt", "(sqrt <x: num, atom or vector>)",
     "Square root (returns f64)"},
    {"log", "(log <x: num, atom or vector>)",
     "Natural logarithm"},
    {"exp", "(exp <x: num, atom or vector>)",
     "Exponential (e^x)"},
    {"sin", "(sin <x: num, atom or vector>)",
     "Sine, radians; returns f64"},
    {"asin", "(asin <x: num, atom or vector>)",
     "Arcsine, radians; out-of-domain values return `0Nf`"},
    {"cos", "(cos <x: num, atom or vector>)",
     "Cosine, radians; returns f64"},
    {"acos", "(acos <x: num, atom or vector>)",
     "Arccosine, radians; out-of-domain values return `0Nf`"},
    {"tan", "(tan <x: num, atom or vector>)",
     "Tangent, radians; returns f64"},
    {"atan", "(atan <x: num, atom or vector>)",
     "Arctangent, radians; returns f64"},
    {"reciprocal", "(reciprocal <x: num, atom or vector>)",
     "Reciprocal `1/x`; zero returns `0Nf`"},
    {"signum", "(signum <x: num, atom or vector>)",
     "Sign as i64: `-1`, `0`, or `1`; null stays null"},
    {"round", "(round <x: num, atom or vector>)",
     "Round to nearest integer"},
    /* aggregates over a vector */
    {"sum", "(sum <x: num, atom, vector or list>)",
     "Sum of all elements"},
    {"prod", "(prod <x: num, atom, vector or list>)",
     "Product of all non-null numeric elements; empty/all-null returns `1`"},
    {"avg", "(avg <x: num, atom, vector or list>)",
     "Arithmetic mean"},
    {"med", "(med <x: num, atom, vector or list>)",
     "Median value (returns f64); a vector of another type than numbers and temporals is a type error, empty or not"},
    {"min", "(min <x: vector, atom or list>) or (min <a: num> <b: num>)",
     "Reduce one vector, or choose the element-wise minimum of two values, in their promoted type (null lowest); f32 input answers f32"},
    {"max", "(max <x: vector, atom or list>) or (max <a: num> <b: num>)",
     "Reduce one vector, or choose the element-wise maximum of two values, in their promoted type (null lowest); f32 input answers f32"},
    {"var", "(var <x: num, atom, vector or list>)",
     "Sample variance; a vector of another type than numbers and temporals is a type error, empty or not"},
    {"var_pop", "(var_pop <x: num, atom, vector or list>)",
     "Population variance"},
    /* docs:  ([accuracy](../language/math.md#variance-and-standard-deviation-accuracy)) */
    {"dev", "(dev <x: num, atom, vector or list>)",
     "Population standard deviation; a vector of another type than numbers and temporals is a type error, empty or not"},
    {"dev_pop", "(dev_pop <x: num, atom, vector or list>)",
     "Population standard deviation (alias)"},
    {"stddev", "(stddev <x: num, atom, vector or list>)",
     "Sample standard deviation (Bessel-corrected; not an alias of `dev`); a vector of another type than numbers and temporals is a type error, empty or not"},
    {"stddev_pop", "(stddev_pop <x: num, atom, vector or list>)",
     "Population standard deviation"},
    {"mode", "(mode <x: atom, vector or list>)",
     "Most frequent non-null value; ties keep the first encountered value"},
    {"all", "(all <x: numeric or bool vector>)",
     "True if every non-null numeric element is truthy; empty/all-null returns `true`"},
    {"any", "(any <x: numeric or bool vector>)",
     "True if any non-null numeric element is truthy; empty/all-null returns `false`"},
    {"count", "(count <x: any collection>)",
     "Count of elements"},
    {"first", "(first <x: vector or list>)",
     "First element of a vector"},
    /* docs: . A direct call takes the element by position, nulls included, for every type: `(first [0Nl 1])` is `0Nl`. Inside `select … by:`, or as the aggregate of a keyless select output, nulls of a typed vector are skipped, `""` in a STR column and `'` in a SYM one, like `0Nl` in an I64 one (grouped `(first s)` over `["" "x"]` is `"x"`, a direct `(first ["" "x"])` is `""`; see [Nulls in a query](../language/math.md#first-last-nulls)), whether its argument is a column or computed (`(first (+ d 1))`, `(first (if c d 0Nd))`); a computed argument that reads no column, such as `(first (take X 4))`, stays positional; inside a helper's body called from a query the same holds for a value of the query's rows ([#1673](https://github.com/RayforceDB/rayforce/issues/1673)) */
    {"last", "(last <x: vector or list>)",
     "Last element of a vector"},
    /* docs: . A direct call takes the element by position, nulls included, for every type: `(at v (- (count v) 1))`. Inside `select … by:`, or as the aggregate of a keyless select output, nulls of a typed vector are skipped, `""` in a STR column and `'` in a SYM one, like `0Nl` in an I64 one (grouped `(last s)` over `["x" ""]` is `"x"`, a direct `(last ["x" ""])` is `""`; see [Nulls in a query](../language/math.md#first-last-nulls)), whether its argument is a column or computed (`(last (+ d 1))`, `(last (if c d 0Nd))`); a computed argument that reads no column, such as `(last (take X 4))`, stays positional; inside a helper's body called from a query the same holds for a value of the query's rows ([#1673](https://github.com/RayforceDB/rayforce/issues/1673)) */
    {"cov", "(cov <x: numeric vector> <y: numeric vector>)",
     "Population covariance over paired non-null numeric inputs"},
    {"scov", "(scov <x: numeric vector> <y: numeric vector>)",
     "Sample covariance over paired non-null numeric inputs"},
    {"pearson_corr", "(pearson_corr <x: numeric vector> <y: numeric vector>)",
     "Pearson correlation over paired non-null numeric inputs"},
    {"wavg", "(wavg <weights: numeric vector> <values: numeric vector>)",
     "Weighted average, `wsum / sum(weights)`; null for zero total weight"},
    {"wsum", "(wsum <weights: numeric vector> <values: numeric vector>)",
     "Weighted sum, `sum(weights * values)`, skipping null pairs"},
    {"percentile", "(percentile <x: numeric vector> <p: num in 0..100>)",
     "Exact linear-interpolated percentile; probability is `0..100`"},
    {"quantile", "(quantile <x: numeric vector> <p: num in 0..1>)",
     "Exact linear-interpolated quantile; probability is `0.0..1.0`; a vector of another type than numbers and temporals is a type error, empty or not"},
    {".emb.cos-dist", "(.emb.cos-dist <a: numeric vector> <b: numeric vector>)",
     "Cosine distance between two vectors"},
    /* docs: ; a vector and a vector give an atom, a list of vectors and a vector give a vector */
    {".emb.tokens", "(.emb.tokens <model: model handle> <text: str or vector of str>)",
     "The token ids a model cuts a text into"},
    {".emb.l2-dist", "(.emb.l2-dist <a: numeric vector> <b: numeric vector>)",
     "Euclidean distance between two vectors"},
    {".emb.inner-prod", "(.emb.inner-prod <a: numeric vector> <b: numeric vector>)",
     "Inner product of two vectors"},
    {"mmu", "(mmu <a: numeric vector or list of rows> <b: numeric vector or list of rows>)",
     "Matrix product: matrix × matrix, matrix × vector, vector × matrix; two vectors give their dot product (an F64 atom)"},
    {"lsq", "(lsq <b: numeric vector or list of rows> <a: square matrix as a list of rows>)",
     "`(lsq b a)` is `b mmu inv a` for a square `a`; for an `a` with more columns than rows, the least-squares solution"},
    /* docs: ; a rank-deficient `a` raises `domain` */
    /* windows: the window comes first */
    {"mavg", "(mavg <window: int> <x: numeric vector>)",
     "Moving average over trailing N rows and non-null values"},
    {"mcount", "(mcount <window: int> <x: numeric vector>)",
     "Moving non-null count over trailing N rows"},
    {"mdev", "(mdev <window: int> <x: numeric vector>)",
     "Moving population standard deviation over trailing N rows and non-null values"},
    {"mmax", "(mmax <window: int> <x: numeric vector>)",
     "Moving maximum over trailing N rows; f32 input stays f32"},
    {"mmin", "(mmin <window: int> <x: numeric vector>)",
     "Moving minimum over trailing N rows; f32 input stays f32"},
    {"msum", "(msum <window: int> <x: numeric vector>)",
     "Moving sum over trailing N rows; nulls are skipped"},
    {"mvar", "(mvar <window: int> <x: numeric vector>)",
     "Moving population variance over trailing N rows and non-null values"},
    {"xrank", "(xrank <buckets: int> <x: numeric vector>)",
     "Assign N rank buckets (quantile ranking)"},
    {"lag", "(lag <x: vector>)",
     "Shift values one row back; first row is null/sentinel"},
    {"lead", "(lead <x: vector>)",
     "Shift values one row forward; last row is null/sentinel"},
    {"deltas", "(deltas <x: numeric vector>)",
     "Adjacent differences, of the type `-` gives: TIMESTAMP → TIMESPAN, TIME → TIMESPAN, DATE → I32 (days), TIMESPAN → TIMESPAN, other integers → I64, floats → F64; first row is null"},
    {"ratios", "(ratios <x: numeric vector>)",
     "Adjacent ratios as f64; first row is null"},
    {"fills", "(fills <x: vector>)",
     "Forward-fill nullable vectors, or every column of a table"},
    {"sums", "(sums <x: numeric vector>)",
     "Running sum; nulls are skipped"},
    {"avgs", "(avgs <x: numeric vector>)",
     "Running average over non-null values"},
    {"mins", "(mins <x: vector>)",
     "Running minimum; f32 input stays f32"},
    {"maxs", "(maxs <x: vector>)",
     "Running maximum; f32 input stays f32"},
    {"prds", "(prds <x: numeric vector>)",
     "Running product; nulls are skipped"},
    {"differ", "(differ <x: vector>)",
     "Boolean change flag versus previous row; first row is true"},
    /* sorting, ranking and sets */
    {"asc", "(asc <x: vector>)",
     "Sort ascending"},
    {"desc", "(desc <x: vector>)",
     "Sort descending"},
    {"iasc", "(iasc <x: vector>)",
     "Indices that would sort ascending (grade up)"},
    {"idesc", "(idesc <x: vector>)",
     "Indices that would sort descending (grade down)"},
    {"rank", "(rank <x: vector>)",
     "Rank of each element (0-based)"},
    {"distinct", "(distinct <x: vector or list>)",
     "Remove duplicates, keeping each value at its first occurrence"},
    /* docs: , in the order the values first appear, for every type, list cells included; a null is kept once; `-0.0` equals `0.0` in float vector cells, as between atoms */
    {"diverse", "(diverse <x: vector or list>)",
     "Check if all elements are unique (no duplicates)"},
    {"group", "(group <x: vector or list>)",
     "Group indices by value (returns dict)"},
    {"reverse", "(reverse <x: vector or list>)",
     "Reverse element order; a table reverses its rows (columns and types kept), a dictionary its entries (keys and values together)"},
    {"raze", "(raze <x: list of vectors>)",
     "Flatten a list of vectors into one vector"},
    /* docs: ; a list of string atoms that repeats shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fails with `limit` */
    {"where", "(where <mask: B8 vector>)",
     "Indices where boolean vector is true"},
    {"til", "(til <n: i64 atom>)",
     "Generate range [0..n)"},
    {"guid", "(guid <n: int>)",
     "Generate a vector of N v4 GUIDs, seeded from the OS (`(guid 0)` → `[]`)"},
    {"bin", "(bin <sorted: I64 vector> <value: i64 atom or I64 vector>)",
     "Binary search — left boundary (sorted input)"},
    {"binr", "(binr <sorted: I64 vector> <value: i64 atom or I64 vector>)",
     "Binary search — right boundary (sorted input)"},
    {"top", "(top <x: vector> <n: int>)",
     "Largest N elements (descending)"},
    {"bot", "(bot <x: vector> <n: int>)",
     "Smallest N elements (ascending)"},
    {"take", "(take <collection: vector, list, str, dict or table> <count: int, or a [start amount] range>)",
     "Take first N elements (negative N takes from end); past the length it cycles"},
    /* docs:  (`select … take:` stops at the table's length). A parted table handle (`.db.parted.get`) is taken as its flat table: the same rows `(at table (til n))` returns, cycling past the row count */
    {"drop", "(drop <collection: vector, list, str, dict or table> <count: int>)",
     "Drop first N elements (negative N drops from end)"},
    {"rotate", "(rotate <collection: vector, list, str, dict or table> <count: int>)",
     "Rotate left by N positions (negative rotates right)"},
    {"filter", "(filter <x: vector, list, str or table> <mask: B8 vector, one boolean per element of x>)",
     "Keep elements where boolean mask is true"},
    {"cut", "(cut <x: vector or list> <indices: sorted integer vector>)",
     "Split a collection at sorted 0-based indices"},
    {"within", "(within <x: vector> <range: a [lo hi] vector>)",
     "Check which elements fall within an inclusive `[lo hi]` range"},
    {"find", "(find <x: vector or list> <value>)",
     "Find index of first occurrence"},
    {"in", "(in <value: atom or vector> <set: vector or list>)",
     "Membership test; a string in a list is one whole value"},
    {"not-in", "(not-in <value: atom or vector> <set: vector or list>)",
     "Negated membership test"},
    {"except", "(except <a: vector or list> <b: vector or list>)",
     "Elements of A not in B. Not a set: the duplicates of A are kept"},
    {"sect", "(sect <a: vector or list> <b: vector or list>)",
     "Elements of A that are also in B. Not a set: the duplicates of A are kept"},
    {"union", "(union <a: vector or list> <b: vector or list>)",
     "Set union: distinct elements of the left operand, then those of the right it lacks, in order of first appearance"},
    {"cross", "(cross <a: vector or list> <b: vector or list>)",
     "Cartesian product as pairs"},
    {"concat", "(concat <left: atom, vector, list, dict or table> <right: any two atoms, vectors or lists, but an atom and a vector only of the atom's type (a str and a STR vector join cell by cell); a dict with a dict; a table with a table that has each of its columns, of the same types>)",
     "Join two atoms, vectors or lists, mixed types giving a list"},
    /* docs: ; an atom joins a vector only of its own type, and a string joins each cell of a `STR` vector (`(concat "ab" ["c"])` → `["abc"]`, so `(concat "ab" [1 2])` is a `type` error); merge two dicts; append the rows of a table to a table that has each of its columns, of the same types (a parted table is refused: use `union-all`). The STR null `""` is empty text, in a direct call and on every query route: `(concat "" "a")` is `"a"`, and `(concat s "")` in a select keeps each cell of `s` */
    {"fill", "(fill <replacement: atom> <x: vector>)",
     "Replace null values; the replacement is the first argument; f32 input stays f32"},
    {"xbar", "(xbar <x: numeric or temporal vector> <bucket: non-zero num>)",
     "Round down to nearest multiple (bucketing)"},
    {"rand", "(rand <count: i64 or i32 atom> <max: i64 or i32 atom, above 0>)",
     "N random integers from 0 up to, not including, `max`; **deterministic**: the same sequence in every process"},
    {"at", "(at <collection: vector, list, str, dict or table> <index: int, an integer vector, or a dict key>)",
     "Index a collection; vector keys batch dictionary lookups. A typed vector indexed by an integer vector keeps its type: a null or out-of-range index gives the type's null (`false`/`0x00` for BOOL/U8)"},
    {"get", "(get <source: dict or table> <key: sym>)",
     "Lookup key in dict or column in table"},
    {"identical", "(identical <a: any> <b: any>)",
     "Whole-value equality: same type, shape and contents; returns one boolean"},
    /* conversion and inspection */
    {"as", "(as <type: sym, e.g. 'i64> <value: any>)",
     "Cast value to another type"},
    /* docs: ; `'LIST` boxes a typed vector into a generic `LIST` of atoms (in a query, the whole column on every route: `select`, `update`, each group of `by:`); a list of string atoms that repeats shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fails with `limit` */
    {"type", "(type <x: any>)",
     "Get the type name of a value; a function value answers `UNARY`, `BINARY`, `VARY` or `LAMBDA`"},
    {"nil?", "(nil? <x: any>)",
     "Test for null; element-wise over vectors and lists (a `B8` per element), the same in and out of queries"},
    {"key", "(key <x: dict or table>)",
     "Get column names (table) or keys (dict)"},
    {"value", "(value <x: dict or table>)",
     "Get column data (table) or values (dict)"},
    {"cols", "(cols <t: table>)",
     "Get table column names"},
    {"meta", "(meta <t: table>)",
     "Table metadata as a dict: its type and its column count"},
    {"fkeys", "(fkeys <t: table>)",
     "Inspect linked-column metadata as `column -> target table`"},
    {"ungroup", "(ungroup <t: grouped table>)",
     "Flatten a grouped table's nested list columns into one row per element"},
    {"row", "(row <t: table> <index: int>)",
     "Extract single row from table as dict"},
    {"dict", "(dict <keys: vector> <values: vector or list>)",
     "Create dictionary from evaluated keys and values vectors"},
    {"remove", "(remove <d: dict> <key>)",
     "Remove key from dictionary"},
    {"unify", "(unify <a: dict or table> <b: dict or table>)",
     "A two-element list of both values, as given (no merge, no type coercion)"},
    {"union-all", "(union-all <a: table> <b: table>)",
     "Concatenate two tables (all rows, no dedup). A parted operand (`.db.parted.get`) is read into memory first: the result is a plain table, and the partition key becomes a column"},
    {"table", "(table <names: sym vector> <columns: list of vectors>)",
     "Create table from column names and list of vectors"},
    /* docs: . A LIST of numeric atoms of one type keeps that type; mixed numeric atoms use the wider type (BOOL < U8 < I16 < I32 < I64 < F64); F32 atoms alone give F32, and F32 mixed with any other numeric type gives F64. Typed nulls stay null except in BOOL and U8 columns, which store false and zero respectively; a list of string atoms that repeats shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fails with `limit` */
    {"xasc", "(xasc <t: table> <columns: sym or sym vector>)",
     "Sort table ascending by column(s)"},
    {"xdesc", "(xdesc <t: table> <columns: sym or sym vector>)",
     "Sort table descending by column(s)"},
    {"xgroup", "(xgroup <t: table> <columns: sym or sym vector>)",
     "Group rows by column value(s), returning dict key -> table slice"},
    {"xkey", "(xkey <t: table> <columns: sym or sym vector>)",
     "Build a dict keyed by unique column value(s), with row dict values for remaining columns"},
    {"xcol", "(xcol <t: table> <names: sym vector, one per column>)",
     "Rename all table columns; names must match column count"},
    {"xcols", "(xcols <t: table> <columns: sym vector>)",
     "Project/reorder named columns"},
    {"split", "(split <text: str or sym> <delimiter: str>)",
     "Split string by delimiter into a list of strings; a string or symbol vector, or a list of string or symbol atoms, splits each element into its own list"},
    {"like", "(like <text: str, sym or a vector of them> <pattern: str with * and ?, or a str vector row by row>)",
     "Glob-style pattern match: `*` any run, `?` one character, `[abc]`, `[a-z]` or `[!abc]` a character class"},
    /* docs: . The pattern is a string atom, or a string column matched row by row (`(like t pat)`); a symbol or a number is a `type` error */
    {"str-find", "(str-find <text: str, sym or a vector of them> <pattern: str or sym>)",
     "First byte index of a literal substring (no wildcards; see `like`); `0Nl` when absent"},
    {"str-join", "(str-join <items: str or sym atom, vector or list> <delimiter: str or sym>)",
     "Join string/symbol atoms, vectors, or lists with a delimiter"},
    {"strlen", "(strlen <x: str or sym>)",
     "Length of each string. `\"\"` is the STR/SYM null: `strlen` maps it to a null length, `sum` and `avg` skip it, and the `avg` of an all-empty group is null"},
    {"sym-name", "(sym-name <ids: i64 atom or I64 vector>)",
     "Resolve integer sym IDs to symbols (passthrough for sym atoms)"},
    {"trunc", "(trunc <unit: sym, one of year quarter month week day hour minute second> <x: date, time or timestamp>)",
     "Floor a DATE, TIME or TIMESTAMP (atom or vector) to the start of the calendar or clock unit that holds it"},
    /* docs: : `'year` `'quarter` `'month` `'week` `'day` `'hour` `'minute` `'second`. The result has the type of the second argument */
    {"date", "(date <x: timestamp, date or 0>)",
     "Current date, or extract date from timestamp; answers a DATE"},
    {"time", "(time <x: timestamp, time or 0>)",
     "Current wall-clock time (millisecond resolution), or extract time from timestamp; answers a TIME"},
    {"timestamp", "(timestamp <ignored: any, usually 0>)",
     "Current wall-clock timestamp at the system clock's full resolution (nanoseconds on Linux)"},
    {"yyyy", "(yyyy <x: date, time or timestamp>)",
     "Year of a date or timestamp"},
    {"mm", "(mm <x: date, time or timestamp>)",
     "Month (1 to 12) of a date or timestamp"},
    {"dd", "(dd <x: date, time or timestamp>)",
     "Day of the month (1 to 31) of a date or timestamp"},
    {"dow", "(dow <x: date, time or timestamp>)",
     "Day of the week (Monday 1 to Sunday 7) of a date or timestamp"},
    {"doy", "(doy <x: date, time or timestamp>)",
     "Day of the year (1 to 366) of a date or timestamp"},
    {"hh", "(hh <x: date, time or timestamp>)",
     "Hour (0 to 23) of a time or timestamp"},
    {"minute", "(minute <x: date, time or timestamp>)",
     "Minute (0 to 59) of a time or timestamp"},
    {"ss", "(ss <x: date, time or timestamp>)",
     "Second (0 to 59) of a time or timestamp"},
    {"parse", "(parse <source: str>)",
     "Parse a string into a Rayfall expression tree"},
    {"read", "(read <path: str>)",
     "Read file contents as string, to EOF"},
    {"read-bytes", "(read-bytes <path: str>)",
     "Read file contents as a `U8` byte vector, to EOF"},
    {"write", "(write <path: str> <content: str>)",
     "Write a string to a file"},
    {"write-bytes", "(write-bytes <path: str> <bytes: U8 vector>)",
     "Write a `U8` byte vector to a file"},
    {".os.setenv", "(.os.setenv <name: str> <value: str>)",
     "Set environment variable"},
    {"exit", "(exit <code: int>)",
     "Exit the process with status code"},
    {".sys.cmd", "(.sys.cmd <command: str>)",
     "Run a system command by name (`env`, `timeit`, `listen`), or else the string as a shell command, returning its exit code (128 + the signal number for a command a signal killed)"},
    {".sys.trust", "(.sys.trust <lambda: a lambda, the sym naming one, or a list of them>)",
     "Mark lambdas a restricted (`-U`) caller may run with the restriction lifted, isolated from the caller; returns its argument"},
    /* docs: . See [`.sys.trust`](../namespaces/sys.md#sys-trust) */
    {".log.replay", "(.log.replay <path: str>)",
     "Replay and return processed entries, **including** evaluation errors; warns and continues, no rollback"},
    {".log.replay_report", "(.log.replay_report <path: str>)",
     "Replay **once** and return I64 `entries` / `eval_errors` on a clean tail"},
    /* docs: ; terminal errors still raise and may leave an applied prefix. Not a previous-call status getter, fail-fast mode, or invariant/rollback guarantee */
    {".log.validate", "(.log.validate <path: str>)",
     "Scan a journal file; return `(chunks valid_bytes)`"},
    {".repl.complete", "(.repl.complete <prefix: str>)",
     "Sorted names (builtins, keywords, session bindings) that start with a prefix"},
    /* docs: . See [`.repl.complete`](../namespaces/repl.md#repl-complete) */
    {".repl.signature", "(.repl.signature <name: sym>)",
     "How a named global is called: kind, calling convention, special form, lambda parameters, documented usage; calls nothing"},
    /* docs: . See [`.repl.signature`](../namespaces/repl.md#repl-signature) */
    {".repl.definition", "(.repl.definition <name: sym>)",
     "Where the function a named global holds was written: file, `(fn …)` span, text, and a digest of the loaded source"},
    /* docs: . See [`.repl.definition`](../namespaces/repl.md#repl-definition) */
    {".repl.inspect", "(.repl.inspect <path: sym or sym vector> [<opts: dict>])",
     "Kind, type and length of a named global or member path, with its dict member or table column names and types, read from headers only"},
    /* docs: . See [`.repl.inspect`](../namespaces/repl.md#repl-inspect) */
    {"ser", "(ser <x: any>)",
     "Serialize any value to binary format (byte vector)"},
    /* docs: ; a value nested deeper than 512 levels fails with `limit`, the depth `de` reads; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` */
    {"de", "(de <bytes: U8 vector>)",
     "Deserialize from binary format back to value; a list holding an error value (built in C, never in Rayfall, where a caught error is a dict) reads back as its first error"},
    /* docs: , see [A list holding an error value](../storage/ipc.md#a-list-holding-an-error-value) */
    {"raise", "(raise <message: str>)",
     "Throw an error with message"},
    {"dl-eval", "(dl-eval <program: dl-program>)",
     "Evaluate program to fixpoint (semi-naive iteration)"},
    {"dl-free", "(dl-free <program: dl-program>)",
     "Free a Datalog program; `true`, or `false` for one already freed"},
    {"dl-stratify", "(dl-stratify <program: dl-program>)",
     "Compute strata for the program (required before eval)"},
    {"dl-query", "(dl-query <program: dl-program> <relation: sym>)",
     "Query a derived or base relation by name"},
    {"dl-provenance", "(dl-provenance <program: dl-program> <relation: sym>)",
     "Reserved provenance hook; currently returns `domain: not available`"},
    {".graph.free", "(.graph.free <graph: graph handle>)",
     "Release a graph handle (idempotent)"},
    {".graph.info", "(.graph.info <graph: graph handle>)",
     "Dict of n_nodes / n_edges / sorted / has_weights"},
    {".idx.uses", "(.idx.uses <t: table>)",
     "Per-column count of index hits since the previous call; the first call arms the counter"},
    /* docs: . See [`.idx.uses`](../namespaces/idx.md#idx-uses) */
    {".attr.set", "(.attr.set <attribute: sym, one of sorted unique grouped parted> <x: vector>)",
     "Verify and stamp `sorted`, `unique`, `grouped` or `parted` on a vector; errors on a violation"},
    {".col.link", "(.col.link <target: sym naming a table> <v: integer vector>)",
     "Attach a foreign-key link from an int vector to a named table"},
    {".parquet.meta", "(.parquet.meta <path: nonempty str>)",
     "Column-chunk metadata table of a Parquet file"},
    /* docs: ; see [`.parquet.*`](../namespaces/parquet.md) */
    {".attr.get", "(.attr.get <x: vector>)",
     "Symbol vector of the attributes a vector holds (empty when none)"},
    {".attr.drop", "(.attr.drop <x: vector>)",
     "Remove all attributes from a vector"},
    {".col.link?", "(.col.link? <x: vector>)",
     "Test whether a value carries link metadata"},
    {".col.target", "(.col.target <x: vector>)",
     "Read back the link's target table name (or `null`)"},
    {".col.unlink", "(.col.unlink <x: vector>)",
     "Detach the link metadata, returning a plain int vector"},
    {".idx.bloom", "(.idx.bloom <x: vector>)",
     "Attach a Bloom filter"},
    {".idx.drop", "(.idx.drop <x: vector>)",
     "Detach whatever index the vector carries"},
    {".idx.has?", "(.idx.has? <x: vector>)",
     "True if the vector carries any index"},
    {".idx.hash", "(.idx.hash <x: vector>)",
     "Attach a hash-set index"},
    {".idx.sort", "(.idx.sort <x: vector>)",
     "Attach a sorted-permutation index"},
    {".idx.zone", "(.idx.zone <x: vector>)",
     "Attach a zone (min/max) index"},
    {".fs.list", "(.fs.list <path: str>)",
     "Sorted sym vector of directory entries"},
    {".fs.size", "(.fs.size <path: str>)",
     "File size in bytes"},
    {".os.getenv", "(.os.getenv <name: str>)",
     "Get environment variable value"},
    {"flip", "(flip <x: numeric vector or non-empty list of numeric rows>)",
     "Transpose of a matrix; rows of one type keep it, mixed rows widen to F64"},
    {"gram", "(gram <x: numeric vector or non-empty list of numeric rows>)",
     "`x mmu flip x` without forming the transpose; symmetric, computed on the upper triangle"},
    {"inv", "(inv <x: numeric vector or non-empty list of numeric rows>)",
     "Inverse of a square matrix"},
    /* docs: ; a singular matrix raises `domain` */
    {"norm", "(norm <x: numeric vector (f32, f64, i32 or i64), or a list of them>)",
     "Euclidean (L2) norm of a numeric vector, or of each vector in a list"},
    {"lower", "(lower <x: str or sym, atom or vector>)",
     "Lowercase string or symbol atoms/vectors"},
    {"upper", "(upper <x: str or sym, atom or vector>)",
     "Uppercase string or symbol atoms/vectors"},
    {"trim", "(trim <x: str or sym, atom or vector>)",
     "Strip leading/trailing whitespace"},
    {".emb.hnsw.free", "(.emb.hnsw.free <index: hnsw index handle>)",
     "Release an index early"},
    /* docs:  (indexes are reference-counted, so this is optional) */
    {".emb.hnsw.info", "(.emb.hnsw.info <index: hnsw index handle>)",
     "Dict describing an index"},
    /* docs: : `nrows`, `dim`, `metric`, `nlayers`, `M`, `efc` */
    {".emb.hnsw.get", "(.emb.hnsw.get <path: str>)",
     "Load an index from a directory"},
    {".emb.model.get", "(.emb.model.get <dir: str>)",
     "Open a text-embedding model from a directory"},
    {".emb.model.info", "(.emb.model.info <model: model handle>)",
     "Dict describing a model"},
    /* clock */
    {".time.sleep", "(.time.sleep <ms: int or timespan>)",
     "Block the calling thread for `ms` milliseconds (an integer) or a timespan; returns null"},
    /* docs: ; a REPL Ctrl-C, an IPC cancel, a server-side `kill -INT`, or the client of a sync request hanging up ends it with `cancel`; an async one runs its full time; a journal replay skips it; it stalls a server's other connections while it waits */
    /* fixed-argument builtins of the system namespaces */
    {".ipc.close", "(.ipc.close <handle: i32 or i64>)",
     "Close an IPC connection handle"},
    {".mc.unsub", "(.mc.unsub <topic: sym or str>)",
     "Remove the calling connection's subscription to a topic; null"},
    /* docs: ; an unknown topic is not an error */
    {".mc.pub", "(.mc.pub <topic: sym or str> <payload: any serializable value>)",
     "Publish a payload to every subscriber of a topic as an async `(upd topic seq payload)`; returns the sequence number, or `0` when the topic has no subscribers"},
    /* docs:  (nothing is sent and the sequence does not advance) */
    {".repl.connect", "(.repl.connect <address: str, \"host:port[:user:password]\">)",
     "Open an IPC session and route REPL input to it"},
    {".json.format", "(.json.format <x: any value but a function>)",
     "A value as JSON text (a table as an array of objects); in a query element-wise: one string per row of a column, on every route"},
    {".sys.listen", "(.sys.listen <port: int or str of digits, or address: str, \"HOST:PORT\">)",
     "Bind an IPC listener on a TCP port"},
    {".time.timer.del", "(.time.timer.del <id: i64>)",
     "Cancel a scheduled timer by id; returns null"},
    {".log.write", "(.log.write <expr: any serializable value>)",
     "Append a serialised expression to the open journal"},
};

/* Builtins that run other code (callbacks, expressions, queries) or take a
 * variable argument list.  An error that passes through them from the code
 * they run is not theirs, so their usage line is added only to a message
 * that starts with their own name (see own_message). */
static const ray_sig_entry_t own_sigs[] = {
    {"map", "(map <fn: fn> <x: vector or list>) or (map <fn: fn> <val: any> <x: vector or list>)",
     "`(map f v)` applies `f` to each element of `v`; `(map f val v)` calls `f(val, elem)` for each element, passing `val` whole each time"},
    /* docs:  (a collection `val` gives one row per element; same as `map-right`). Same-type atom results form a typed vector, otherwise a list. String results that repeat shared strings beyond 1 GiB of extra data ([values that repeat their parts](../data-types/collections.md#values-that-repeat-their-parts)) fail with `limit`. Exactly 2 or 3 arguments, else `domain` */
    {"pmap", "(pmap <fn: fn> <x: vector or list>) or (pmap <fn: fn> <val: any> <x: vector or list>)",
     "`map` with the calls run in parallel on the pool's workers when the function is proven pure and the calls are worth it; the same result, result types, error and 2 or 3 arguments as `map`"},
    /* docs: , else `domain` */
    {"map-left", "(map-left <fn: fn> <left: any> <right: any>)",
     "Map each element of the left over the whole right"},
    {"map-right", "(map-right <fn: fn> <left: any> <right: any>)",
     "Map the whole left over each element of the right"},
    {"apply", "(apply <fn: fn> <left: vector or list> <right: vector or list>)",
     "`(apply f a b)` calls `f` on the elements of two collections taken in step, `f(a_0, b_0)`, `f(a_1, b_1)`, ...; the result is as long as the shorter one"},
    /* docs: . Results are collected as `map`'s are. Two atoms call `f` once. Exactly 3 arguments, else `domain` */
    {"fold", "(fold <fn: fn> <init: any> <x: vector or list>) or (fold <fn: fn> <x: vector or list>)",
     "Reduce with a function, from an initial value or else the first element"},
    {"fold-left", "(fold-left <fn: fn> <init: any> <x: vector or list>) or (fold-left <fn: fn> <x: vector or list>)",
     "Left-associative fold"},
    {"fold-right", "(fold-right <fn: fn> <init: any> <x: vector or list>) or (fold-right <fn: fn> <x: vector or list>)",
     "Right-associative fold"},
    {"fold-while", "(fold-while <pred: fn> <fn: fn> <init: any> <x: vector or list>)",
     "Fold that stops when the accumulator fails pred"},
    {"scan", "(scan <fn: fn> <x: vector or list>)",
     "Running fold (all intermediate results)"},
    {"scan-left", "(scan-left <fn: fn> <x: vector or list>)",
     "Left-to-right running fold"},
    {"scan-right", "(scan-right <fn: fn> <x: vector or list>)",
     "Right-to-left running fold (returns a list)"},
    {"prior", "(prior <fn: fn> <x: vector or list>)",
     "Apply a binary fn to each element and its predecessor (first paired with itself)"},
    /* docs: . A comparison, or `+` `-` `*` over an `I16`/`I32`/`I64`/`F64` vector, returns a typed vector; any other fn returns a list */
    {"if", "(if <cond: bool or bool vector> <then> [<cond> <then> ...] [<else>])",
     "Conditional: (if c1 r1 c2 r2 ... else?) — first truthy condition wins"},
    /* docs: . A boolean-vector condition chooses per row between the two arms, as in a query: both arms are always evaluated (their side effects happen, and an arm that raises raises), arms are vectors of its length or atoms, and a length-1 vector gives a 1-element vector. In a query the arms give one column: numbers promote, a temporal beside a null keeps its type, two different temporal types (DATE, TIME, TIMESTAMP, TIMESPAN) or a temporal beside a number raise `type` ([control flow](../language/control-flow.md#conditionals-if)) */
    {"while", "(while <cond: bool> <body>...)",
     "Iterate while cond is truthy; returns null"},
    /* docs: . A vector condition is one truth value (non-empty is true); in a query, a vector as long as the rows (or the group) is true or false when every cell agrees and otherwise raises `nyi`, whatever computes it ([control flow](../language/control-flow.md#iteration-while)) */
    {"times", "(times <n: int> <body>...) or (times [<name: sym> <n: int>] <body>...)",
     "Run body exactly n times (count evaluated once); `(times [i n] ...)` binds `i` to 0..n-1, one per pass (at the top level a global, refused for a restricted caller); returns null"},
    {"fn", "(fn <params: sym vector> <body>...)",
     "Create lambda function"},
    {"quote", "(quote <x: any>)",
     "Return argument unevaluated; a bare name yields a literal symbol (`(quote x)` ≡ `'x`)"},
    {"return", "(return [<x: any>])",
     "Early return from the enclosing lambda; with no argument it returns null"},
    {"alter", "(alter <name: quoted sym> <op: fn or sym> <arg: any>...)",
     "Amend a named local or global binding and return its name"},
    /* docs: : `(alter 'name set idx val)` writes `val` at an index or index vector (for a dict, `idx` is a key or a vector of keys: an existing key is replaced, a missing one is added), `(alter 'name concat vals)` appends, `(alter 'name remove idx)` drops list items. On a typed vector `val` must be of the vector's kind: a number into a numeric vector (converted to the vector's type), otherwise a value of the vector's own type, and a null of any type, or a value vector of nulls only, writes the vector's null; anything else, or a value vector whose length is not the index count, is a `type` error and the vector is unchanged. A SYM vector read from a table saved with a symbol file takes any symbol, held by that file or not; the write stays in memory and the file is not changed. The innermost visible local or parameter takes precedence over a global, through a literal `alter` head or a global alias of it; shared values retain copy-on-write isolation */
    {"modify", "(modify <t: table> <column: sym> <fn: fn>)",
     "Functional table update (returns new table)"},
    {"pivot", "(pivot <t: table> <index: sym> <pivot: sym> <value: sym> <agg: fn>)",
     "Pivot table — reshape long to wide format"},
    {"insert", "(insert <target: quoted name of a table, list or vector> <value: row or values>)",
     "Append rows, or grow a parted table's in-memory live tail with an explicit partition key"},
    /* docs: ; a quoted target names the innermost visible local or parameter before a global, through a literal `insert` head or a global alias of it */
    {"upsert", "(upsert <t: table> <key: sym or sym vector> <row: dict>)",
     "Insert or update flat-table rows by key match (target, key, row); a null key matches a null-keyed row for every key type, and a key value the key column cannot hold is a `domain` error"},
    {"select", "(select <query: dict with from:, and optionally by:, where:, take:, and named columns>)",
     "Query table with optional filter, projection, grouping, and aggregation"},
    {"update", "(update <query: dict with from:, and optionally by:, where:, and named columns>)",
     "Add or modify columns: `{from: 't …}` updates the named table in place, `{from: t …}` returns a new table"},
    {"window", "(window <query: dict with from:, part:, order:, frame: and funcs:>)",
     "Partitioned window query"},
    /* docs: . `frame:` is `'whole`, `'running`, or a positive trailing row count. `first` and `last` give the frame's first or last row, a null included, in the value column's type; `min` and `max` skip nulls and keep the value column's type too, ordering SYM and STR by text and GUID by bytes, as `min` and `max` do outside a window */
    {"inner-join", "(inner-join <keys: sym vector> <left: table> <right: table>)",
     "Inner join — only matching rows from both sides"},
    {"left-join", "(left-join <keys: sym vector> <left: table> <right: table>)",
     "Left join — all left rows, unmatched filled with null. Keys are a symbol list"},
    {"full-join", "(full-join <keys: sym vector> <left: table> <right: table>)",
     "Full outer join — all rows from both sides, unmatched columns filled with null"},
    {"anti-join", "(anti-join <keys: sym vector> <left: table> <right: table>)",
     "Anti-semi-join — left rows with no right match"},
    {"semi-mask", "(semi-mask <keys: sym vector> <left: table> <right: table>)",
     "Boolean vector over left rows — true where the row has a right match"},
    {"anti-mask", "(anti-mask <keys: sym vector> <left: table> <right: table>)",
     "Boolean vector over left rows — true where the row has no right match"},
    {"asof-join", "(asof-join <keys: sym vector, the last one the time key> <left: table> <right: table>)",
     "As-of join — match most recent preceding value. Keys come first, last key is the time key"},
    {"window-join", "(window-join <keys: sym vector, the last one the time key> <intervals> <left: table> <right: table> <aggs: dict>)",
     "Window join — `[eq-keys... time-key]`, intervals, left, right, agg dict"},
    {"window-join1", "(window-join1 <keys: sym vector, the last one the time key> <intervals> <left: table> <right: table> <aggs: dict>)",
     "Window join variant (strict window, no prevailing quote)"},
    {".db.parted.get", "(.db.parted.get <root: str> <table: sym>)",
     "Load partitioned table from root directory"},
    {".db.parted.tables", "(.db.parted.tables <root: str>)",
     "List table names under a parted root (from the most recent partition)"},
    {".db.parted.fill", "(.db.parted.fill <root: str>)",
     "Backfill missing tables across a parted db's partitions"},
    {".db.splayed.get", "(.db.splayed.get <dir: str> [<options>])",
     "Load splayed table from a directory"},
    {".db.splayed.set", "(.db.splayed.set <dir: str> <table: table> [<options>...])",
     "Save table as splayed columns to a directory"},
    /* docs: ; optional `{compress: bool, symfile: 'shared/'column, index: bool, durable: bool}`; `{durable: false}` skips the fsyncs; a parted table is written a segment at a time */
    {".db.splayed.append", "(.db.splayed.append <dir: str> <table: table> [<sym: str>] [<options>])",
     "Append a table's rows to a splayed table on disk without loading it"},
    /* docs: ; the table has the target's column names and types; optional symbol-file path and `{index: bool, durable: bool}`; no table there: written as by `.db.splayed.set` */
    {".csv.splayed", "(.csv.splayed [<schema>] [<options: dict>] <source: str path> <target: str dir>)",
     "Stream a CSV directly into a splayed on-disk table; return it loaded"},
    {".csv.parted", "(.csv.parted [<schema>...] [<options: dict>] <source: str path> <root: str dir> <table: sym>)",
     "Stream a CSV directly into a partitioned table under a db root"},
    {".parquet.read", "(.parquet.read [<columns>] <path: nonempty str>)",
     "Read a Parquet file into a table; optional projection or options before the path"},
    {".parquet.scan", "(.parquet.scan [<options>] <path: nonempty str>)",
     "Reusable lazy `select` source over a Parquet file; optional options before the path"},
    {".parquet.splayed", "(.parquet.splayed [<types>] <path: nonempty str> <dir: str>)",
     "Import a Parquet file as a splayed table; returns the row count; optional `{types: [...] strict: bool compress: bool symfile: 'shared/'column reproducible: bool}`"},
    {".parquet.parted", "(.parquet.parted [<types>] <paths: nonempty str, or a list of them> <root: str> <table: sym>)",
     "Import Parquet files into a partitioned table under a root; returns the row count; optional `{types: [...] rows: n strict: bool compress: bool reproducible: bool}`"},
    {".parquet.each", "(.parquet.each [<columns>] <path: nonempty str> <callback: fn>)",
     "Deliver a Parquet file to a callback in chunks; returns the number of rows delivered"},
    {"let", "(let <name: sym> <value: any>)",
     "Bind value to local variable (lexical scope); at the top level it binds a global, refused for a restricted caller"},
    {"set", "(set <name: sym> <value: any>)",
     "Bind value to global variable"},
    {"try", "(try <expr> <handler: fn of the error, or a fallback value>)",
     "Error handling: (try expr handler-fn-or-fallback-value); the handler gets the raised value, or `{code msg}` for an engine error (`{code msg usage}` when it is about a builtin's call)"},
    {"eval", "(eval <expr: a parsed expression>)",
     "Evaluate a parsed Rayfall expression"},
    {"load", "(load <path: str>)",
     "Load and evaluate a Rayfall script file. A relative path is tried against the working directory, then below `RAYFORCE_HOME` when set; absolute paths are used as given; a failure names every path tried"},
    {"rc", "(rc <x: any>)",
     "Get reference count of an object"},
    {".mem.objsize", "(.mem.objsize <x: any>)",
     "Logical bytes retained by an object graph; shared children count once"},
    {"resolve", "(resolve <name: sym> ...)",
     "Resolve a symbol in the current scope"},
    {"rule", "(rule <head: (name ?var ...)> <body: clause>...)",
     "Define a Datalog rule (head + body clauses)"},
    {"env", "(env <ignored: any, usually 0>)",
     "List all global environment bindings"},
    {"read-csv", "(read-csv [<names: sym vector>] [<types: sym vector>] <path: str>)",
     "Load CSV file into table (mmap, parallel parse)"},
    {".csv.read", "(.csv.read [<names: sym vector>] [<types: sym vector>] <path: str>)",
     "Load CSV file into table (mmap, parallel parse)"},
    {"write-csv", "(write-csv <t: table> <path: str> [<options>])",
     "Write table to CSV file"},
    {".csv.write", "(.csv.write <t: table> <path: str> [<options>])",
     "Write table to CSV file"},
    {"read-value", "(read-value <text: str> [<limits: dict>])",
     "Reads one value from data text without evaluating anything; an optional dict overrides the limits"},
    /* docs: . See [Reading Data Text](../language/read-value.md) */
    {"read-limits", "(read-limits)",
     "The default limits of `read-value`, as a dict"},
    {"format", "(format <template: str, each % marks a value> <value: any>...)",
     "Format value to string (% as placeholder)"},
    /* docs: ; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit`; in a query element-wise: one string per row of a column, on every route */
    {"delete", "(delete <query: dict with from: and where:>)",
     "Remove the rows a predicate matches: `{from: 't where: p}` removes them in place, `{from: t where: p}` returns a new table. Both clauses are required"},
    {"del", "(del <name: quoted sym or dotted path>)",
     "Unbind one variable or dotted-path leaf and return `0`; deleting the last leaf of a dotted path removes the intermediate dicts too. Extra arguments raise `arity`"},
    {".emb.hnsw.set", "(.emb.hnsw.set <path: str> <index: hnsw index handle>)",
     "Persist an index to a directory"},
    {".ipc.post", "(.ipc.post <handle: i32 or i64> <message: any serializable value>)",
     "Send a message asynchronously (fire-and-forget); return the null object"},
    {".ipc.open", "(.ipc.open <address: str, \"host:port[:user:password]\"> [<timeout-ms: int, or options: dict of timeout, compress, keepalive, transport>])",
     "Open a connection to host:port (optional connect timeout in ms, or an options dict with `timeout`, `compress`, `keepalive`, `transport`), returns handle"},
    {".ipc.handle", "(.ipc.handle [<handle: i32 or i64>])",
     "Current connection handle inside any `.ipc.on.*` hook, `-1` outside"},
    /* docs: ; given a live connection handle, that connection's view (`inbound`, `transport`, `limit_bytes`, `limit_frames`, `queued_bytes`, `queued_frames`, `hwm_bytes`) */
    {".ipc.handles", "(.ipc.handles)",
     "Every open IPC connection of this process as a table, one row per connection in handle order: the columns of `(.ipc.handle h)` and `opened`, when it was accepted or opened. Allowed under `-U`"},
    {".ipc.auto_journal_eval", "(.ipc.auto_journal_eval)",
     "`true` while evaluating inside an auto-journaled IPC request (the request itself is the journal entry and `.log.write` refuses), `false` otherwise"},
    {".ipc.txlimit", "(.ipc.txlimit [<bytes: int> [<frames: int>]]) or (.ipc.txlimit <handle: int> <bytes: int> <frames: int>)",
     "Transmit backlog policy: `()` reads the process default, `(bytes [frames])` sets it, `(h bytes frames)` overrides one connection"},
    /* docs: ; a frame that would exceed a connection's limit is refused and a multicast subscriber is dropped and closed */
    {".ipc.transport", "(.ipc.transport [<policy: sym, 'auto, 'tcp, 'shm or 'rdma>])",
     "Read or set the transport policy, `'auto`, `'tcp`, `'shm` or `'rdma`: what this process offers as a server and asks for as a client"},
    {".mc.sub", "(.mc.sub <topic: sym or str> [<filter: null>])",
     "Subscribe the calling connection to a multicast topic (symbol or string); the filter argument must be null. Returns the sequence number the next publication will carry"},
    /* docs: . A non-null filter raises `nyi`. See [Multicast topics](../storage/ipc.md#multicast) */
    {".mc.stats", "(.mc.stats)",
     "Multicast counters as a dict: `topics`, `subscriptions`, `published`, `delivered`, `dropped`, `framed`, `tx_limit_bytes`, `tx_limit_frames`, `tx_hwm_bytes`"},
    {".repl.disconnect", "(.repl.disconnect)",
     "Detach and close the active remote session"},
    {".json.parse", "(.json.parse <text: str or str vector> [<limits: dict>])",
     "JSON text to a value (object to dict with string keys, array to a vector or list); element-wise over a string vector; strict, the limits of `read-value`"},
    /* docs: . See [`.json.*`](../namespaces/json.md) */
    {".json.get", "(.json.get <text: str or str vector> <path: str or sym, dotted, or a str or sym vector of paths> [<type: sym, 'STR, 'I64 or 'F64>] [<limits: dict>])",
     "One field of each JSON document by a dotted path (or one path per row), without building the rest"},
    /* docs: ; an optional type (`'STR`, `'I64`, `'F64`) makes it always that type, null where the field is absent or another kind; optional limits */
    {".sys.exec", "(.sys.exec <command: str> [<mode: sym, 'out>])",
     "Execute a shell command; exit code, or `{code, out}` with `'out`"},
    {".time.timer.set", "(.time.timer.set <ms: i64> <num: i64, 0 for ever> <fn: lambda of one argument>)",
     "Schedule callback every `ms`, `num` times (0 = forever); returns id"},
    {".log.open", "(.log.open <mode: sym, 'async or 'sync> <base: str>)",
     "Open a journal in async or sync mode"},
    {".log.close", "(.log.close)",
     "Flush and close the active journal"},
    {".log.roll", "(.log.roll)",
     "Close the active segment and start a new one"},
    {".log.snapshot", "(.log.snapshot)",
     "Write a snapshot of current state and roll to a fresh segment"},
    {".log.sync", "(.log.sync)",
     "`fsync` the journal"},
    {".log.posts", "(.log.posts [<on: i64, 0 or 1>])",
     "Read (no argument) or set (`0`/`1`) whether an open journal also records async requests; off by default, `--log-posts N` at startup; returns the state as `i64`"},
    {".log.purge", "(.log.purge)",
     "Close the active journal and delete all its files"},
    {".log.status", "(.log.status)",
     "Journal state as a dict of seven fields"},
    /* docs: : `open` (handle present), `opened_ok` (a journal was opened successfully at least once), `path`, `mode` (`sync`/`async`; both null if never opened successfully), `bytes` (nullable: null if the size probe failed), `auto` (inside an auto-journaled request), `posts` (async requests are journaled too); raises only on allocation/intern failure */
    {".sys.prof", "(.sys.prof)",
     "Last profiled query's per-step statistics as a table (opt-in via `:t`)"},
    {".sys.profile", "(.sys.profile)",
     "Per-call-site profile of Rayfall code as a table (opt-in via `.sys.profile.enable`)"},
    {".sys.querylog", "(.sys.querylog [<from: i64, a seq>])",
     "Ambient per-query statistics ring as a table (opt-in via `-Q` / `.sys.querylog.enable`); a `seq` column numbers every logged query from 1, and `(.sys.querylog n)` answers only the rows with `seq >= n`"},
    {".sys.querylog.stats", "(.sys.querylog.stats)",
     "The query-statistics ring's counters: `next` seq, rows `dropped` by the wrap, `capacity`"},
    {".graph.build", "(.graph.build <edges: table> <src: sym> <dst: sym> [<weight: sym>])",
     "Build a graph handle from an edge table"},
    {".graph.pagerank", "(.graph.pagerank <graph: graph handle> [<iterations: int>] [<damping: f64>])",
     "Iterative PageRank scores"},
    {".graph.connected", "(.graph.connected <graph: graph handle>)",
     "Connected components via label propagation"},
    {".graph.degree", "(.graph.degree <graph: graph handle>)",
     "In/out/total degree per node"},
    {".graph.topsort", "(.graph.topsort <graph: graph handle>)",
     "Topological sort (Kahn)"},
    {".graph.cluster", "(.graph.cluster <graph: graph handle>)",
     "Local clustering coefficient per node"},
    {".graph.mst", "(.graph.mst <graph: graph handle>)",
     "Minimum spanning tree (Kruskal)"},
    {".graph.louvain", "(.graph.louvain <graph: graph handle> [<max-iter: int>])",
     "Louvain modularity-based community detection"},
    {".graph.dijkstra", "(.graph.dijkstra <graph: graph handle> <src: int> [<dst: int>] [<max-depth: int>])",
     "Weighted shortest paths from a source (optionally to a target)"},
    {".graph.dfs", "(.graph.dfs <graph: graph handle> <src: int> [<max-depth: int>])",
     "Depth-first traversal from a seed node"},
    {".graph.betweenness", "(.graph.betweenness <graph: graph handle> [<sample: int>])",
     "Brandes betweenness centrality (exact or sampled)"},
    {".graph.closeness", "(.graph.closeness <graph: graph handle> [<sample: int>])",
     "Closeness centrality (exact or sampled)"},
    {".graph.random-walk", "(.graph.random-walk <graph: graph handle> <src: int> [<walk-len: int>])",
     "Uniform random walk from a seed node"},
    {".graph.k-shortest", "(.graph.k-shortest <graph: graph handle> <src: int> <dst: int> <k: int>)",
     "Yen's k shortest weighted paths between two nodes"},
    {".graph.shortest-path", "(.graph.shortest-path <graph: graph handle> <src: int> <dst: int> [<max-depth: int>])",
     "BFS shortest path between two nodes"},
    {".graph.expand", "(.graph.expand <graph: graph handle> <src: int> [<direction: sym>])",
     "1-hop neighbour expansion from a seed node"},
    {".graph.var-expand", "(.graph.var-expand <graph: graph handle> <src: int> <min-depth: int> <max-depth: int> [<direction: sym>] [<track-path: bool>])",
     "BFS variable-depth expansion with optional path tracking"},
    {".idx.info", "(.idx.info <col: vector>) or (.idx.info <t: table> <column: sym>)",
     "Dict describing the attached index, or a table column's format"},
    {".emb.hnsw.build", "(.emb.hnsw.build <col: list of numeric vectors> [<metric>] [<M: int>] [<ef_c: int>])",
     "Build an HNSW index over an embedding column"},
    {".emb.encode", "(.emb.encode <model: model handle> <text: str or vector of str> [<max-tokens: int>])",
     "The vector of a text, or of each of many"},
    {".emb.knn", "(.emb.knn <col: list of numeric vectors> <query: numeric vector> <k: int> [<metric>])",
     "Exact k nearest neighbours by brute force"},
    {".emb.ann", "(.emb.ann <index: hnsw index handle> <query: numeric vector> <k: int> [<ef_s: int>])",
     "Approximate k nearest neighbours through an HNSW index"},
    {"replace", "(replace <text: str or sym> <from: str or sym> <to: str or sym>)",
     "Replace all occurrences of a string/symbol atom. An empty `to` deletes each match; an empty `from` matches nothing and returns the input"},
    {"substr", "(substr <text: str or sym> <start: int, or I64/I32 vector> <length: int, or I64/I32 vector>)",
     "Substring by 1-based start and length; 0 clamps to start"},
    {".mem.ts", "(.mem.ts <expr>)",
     "Evaluate once and return result, time, allocation, peak-memory, result-size, and worker statistics as a dict"},
    {".sys.args", "(.sys.args)",
     "Application arguments as a typed dict; `user` subdict holds post-`--` args"},
    {".sys.build", "(.sys.build)",
     "Build metadata dict with `version` + `build-date`"},
    {".sys.info", "(.sys.info)",
     "Host and process info (cores, page size, total memory, workers, pid, hostname)"},
    {".sys.mem", "(.sys.mem)",
     "Memory allocator statistics (alloc / peak / slab hits)"},
    {".sys.env", "(.sys.env [<name: str>])",
     "Number of global bindings (the REPL's `:env` lists them)"},
    {".sys.gc", "(.sys.gc [<mode: i64, 0 or 1>])",
     "Trigger GC / heap flush, returns `0`; mode `1` also releases free pages now, best effort"},
    {".sys.join.par.min", "(.sys.join.par.min [<rows: i64>])",
     "Read or set the probe-side rows from which a hash join's probe goes parallel; returns the value in effect"},
    {".sys.timeit", "(.sys.timeit [<on: i64, 0 or 1>])",
     "Toggle / set the per-expression profiler"},
    {".sys.profile.enable", "(.sys.profile.enable [<on: i64, 0 or 1>])",
     "Toggle the call profiler; returns new state"},
    {".sys.profile.reset", "(.sys.profile.reset)",
     "Drop the call profiler's rows"},
    {".sys.querylog.enable", "(.sys.querylog.enable [<on: i64, 0 or 1>])",
     "Toggle query-statistics logging; returns new state"},
    {".time.now", "(.time.now)",
     "Monotonic time in milliseconds"},
    {"datoms", "(datoms)",
     "Create an empty EAV table"},
    {"assert-fact", "(assert-fact <db: datoms table> <entity> <attr: sym> <value>)",
     "Append a triple (e, a, v) to the EAV table"},
    {"retract-fact", "(retract-fact <db: datoms table> <entity> <attr: sym> <value>)",
     "Remove a triple from the EAV table"},
    {"scan-eav", "(scan-eav <db: datoms table> <attr: sym> [<options>])",
     "Query EAV by attribute, returns (e, v) table"},
    {"pull", "(pull <db: datoms table> <entity> [<attrs: sym vector>])",
     "Entity-centric retrieval — returns all attributes as dict"},
    {"dl-program", "(dl-program)",
     "Create an empty Datalog program"},
    {"dl-add-edb", "(dl-add-edb <program: dl-program> <name: sym> <table: table> <arity: int>)",
     "Register a base relation (table + arity)"},
    {"query", "(query <db: datoms table> (find <vars>...) (where <clauses>...) [(rules ...)])",
     "Compile and execute a Datalog query against EAV store"},
    {"repr", "(repr <x: any> [<max-bytes: int>])",
     "`(repr v)` writes `v` as Rayfall text that reads back to the same value"},
    /* docs: ; `(repr v n)` fails with `limit` rather than return more than `n` bytes; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit`. See [`repr`](../language/string.md#repr); in a query element-wise: one string per row of a column, on every route */
    {"timeit", "(timeit <expr>)",
     "Evaluate an expression and return the elapsed time in milliseconds"},
    {".ipc.send", "(.ipc.send <handle: i32 or i64> <message: any serializable value> [<timeout-ms: int, > 0 or 0N>])",
     "Send a value over an IPC handle (sync request); optional third argument is a deadline in ms"},
    {"do", "(do <expr>...)",
     "Sequential execution, returns last value"},
    /* docs: . Inside a query the earlier forms run once per query, over the selected rows, and the last form is planned as if alone, seeing their `let`s ([`do` inside a query](../language/control-flow.md#do-inside-a-query)) */
    {"list", "(list <x: any>...)",
     "A list of its arguments, of any types"},
    {"enlist", "(enlist <x: any>...)",
     "Wrap value(s) in a vector (list of atoms)"},
    {"print", "(print <x: any>...) or (print <template: str, each % marks a value> <value: any>...)",
     "Print value without newline"},
    /* docs: ; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` */
    {"println", "(println <x: any>...) or (println <template: str, each % marks a value> <value: any>...)",
     "Print values with newline"},
    /* docs: ; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` */
    {"show", "(show <x: any>...)",
     "Pretty-print a value (tables formatted)"},
    /* docs: ; a value that [repeats its parts](../data-types/collections.md#values-that-repeat-their-parts) beyond 1 GiB of extra output fails with `limit` */
    {"and", "(and <a: bool or num> <b: bool or num> ...)",
     "Logical AND; element-wise on vectors and lists, each element by its truthiness (zero, null and NaN are false), in a query too (lengths must match)"},
    {"or", "(or <a: bool or num> <b: bool or num> ...)",
     "Logical OR; element-wise on vectors and lists, each element by its truthiness, in a query too (lengths must match)"},
};

/* A builtin's registered name keeps at most 13 characters, so a longer name
 * reaches here cut off: an entry matches when its name is the one given, or
 * when the one given is the first 13 characters of a longer entry.  A cut-off
 * name that fits two entries (.sys.profile.enable, .sys.profile.reset) has no
 * match: it cannot be told which was called. */
#define FN_NAME_MAX 13

static bool name_matches(const char* entry, const char* name) {
    if (entry[0] != name[0]) return false;
    if (strcmp(entry, name) == 0) return true;
    return strlen(name) >= FN_NAME_MAX && strncmp(entry, name, FN_NAME_MAX) == 0;
}

/* The entry of `name`, in either table; NULL when there is none or the name is
 * ambiguous.  `*own` tells which table it came from. */
static bool own_message(const char* name, const char* msg);

static const ray_sig_entry_t* find_entry_scan(const char* name, bool* own, const char* msg) {
    const ray_sig_entry_t* hit = NULL;
    int found = 0;
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
        if (name_matches(sigs[i].name, name)) {
            if (strlen(name) < FN_NAME_MAX && strcmp(sigs[i].name, name) == 0) { *own = false; return &sigs[i]; }
            hit = &sigs[i]; *own = false; found++;
        }
    for (size_t i = 0; i < sizeof own_sigs / sizeof *own_sigs; i++)
        if (name_matches(own_sigs[i].name, name)) {
            if (strlen(name) < FN_NAME_MAX && strcmp(own_sigs[i].name, name) == 0) { *own = true; return &own_sigs[i]; }
            hit = &own_sigs[i]; *own = true; found++;
        }
    if (found == 1) return hit;
    /* A cut-off name that fits several entries (.sys.querylog and
     * .sys.querylog.enable, .sys.profile.enable and .sys.profile.reset): the
     * message begins with the full name of the builtin that wrote it. */
    if (found > 1 && msg && *msg) {
        const ray_sig_entry_t* pick = NULL;
        int picks = 0;
        for (size_t i = 0; i < sizeof own_sigs / sizeof *own_sigs; i++)
            if (name_matches(own_sigs[i].name, name) && own_message(own_sigs[i].name, msg)) {
                pick = &own_sigs[i]; picks++;
            }
        if (picks == 1) { *own = true; return pick; }
    }
    return NULL;
}

/* The scan above reads both tables and compares every name: about 470 strcmp per
 * noted error.  The tables are static, so each entry is put once in a hash table of
 * its key, the name cut at 13 characters (the longest a registered name keeps), and a
 * lookup visits only the entries of its own key.  The table is built on the first use,
 * by whichever thread gets there first; the others scan until it is ready.  An ambiguous
 * name (several entries on one key) goes to the scan, which tells them apart by the
 * message. */
#define SIG_KEY_LEN (FN_NAME_MAX + 1)
#define SIG_IDX_SIZE 2048u
#define SIG_NITEMS (sizeof sigs / sizeof *sigs + sizeof own_sigs / sizeof *own_sigs)
_Static_assert(SIG_NITEMS * 2 <= SIG_IDX_SIZE, "sig index too small for the tables");
typedef struct { char key[SIG_KEY_LEN]; const ray_sig_entry_t* e; bool own; } sig_item_t;
static sig_item_t sig_items[SIG_NITEMS];
static uint16_t sig_slots[SIG_IDX_SIZE];     /* item index + 1; 0 is empty */
static _Atomic int sig_idx_state;            /* 0 not built, 1 building, 2 ready */

static void sig_key(const char* name, char out[SIG_KEY_LEN]) {
    size_t n = strlen(name);
    if (n > FN_NAME_MAX) n = FN_NAME_MAX;
    memset(out, 0, SIG_KEY_LEN);
    memcpy(out, name, n);
}

static size_t sig_hash(const char key[SIG_KEY_LEN]) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < SIG_KEY_LEN && key[i]; i++) h = (h ^ (uint8_t)key[i]) * 16777619u;
    return (size_t)h & (SIG_IDX_SIZE - 1);
}

static void sig_index_build(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&sig_idx_state, &expected, 1)) return;
    size_t n = 0;
    for (int pass = 0; pass < 2; pass++) {
        const ray_sig_entry_t* t = pass ? own_sigs : sigs;
        size_t cnt = pass ? sizeof own_sigs / sizeof *own_sigs : sizeof sigs / sizeof *sigs;
        for (size_t i = 0; i < cnt; i++, n++) {
            sig_item_t* it = &sig_items[n];
            sig_key(t[i].name, it->key);
            it->e = &t[i];
            it->own = pass == 1;
            size_t h = sig_hash(it->key);
            while (sig_slots[h]) h = (h + 1) & (SIG_IDX_SIZE - 1);
            sig_slots[h] = (uint16_t)(n + 1);
        }
    }
    atomic_store_explicit(&sig_idx_state, 2, memory_order_release);
}

static const ray_sig_entry_t* find_entry(const char* name, bool* own, const char* msg) {
    int st = atomic_load_explicit(&sig_idx_state, memory_order_acquire);
    if (st != 2) {
        if (st == 0) sig_index_build();
        if (atomic_load_explicit(&sig_idx_state, memory_order_acquire) != 2)
            return find_entry_scan(name, own, msg);
    }
    /* The same builtin failing again and again (a try in a loop) is answered from
     * the last lookup of the thread, whose answer did not depend on the message. */
    static _Thread_local struct {
        char name[SIG_KEY_LEN + 2];
        const ray_sig_entry_t* e;
        bool own, valid;
    } last;
    size_t nlen = strlen(name);
    if (last.valid && nlen <= SIG_KEY_LEN && !memcmp(last.name, name, nlen + 1)) {
        *own = last.own;
        return last.e;
    }
    char key[SIG_KEY_LEN];
    sig_key(name, key);
    const sig_item_t* hit = NULL;
    int found = 0;
    const ray_sig_entry_t* res = NULL;
    bool res_own = false, decided = false;
    for (size_t h = sig_hash(key); sig_slots[h]; h = (h + 1) & (SIG_IDX_SIZE - 1)) {
        const sig_item_t* it = &sig_items[sig_slots[h] - 1];
        if (memcmp(it->key, key, SIG_KEY_LEN) || !name_matches(it->e->name, name)) continue;
        if (nlen < FN_NAME_MAX && strcmp(it->e->name, name) == 0) { res = it->e; res_own = it->own; decided = true; break; }
        hit = it; found++;
    }
    if (!decided) {
        if (found == 1) { res = hit->e; res_own = hit->own; decided = true; }
        else if (found > 1) return find_entry_scan(name, own, msg);   /* told apart by the message: not cached */
        else decided = true;                                           /* no entry: res stays NULL */
    }
    if (nlen <= SIG_KEY_LEN) {
        memcpy(last.name, name, nlen + 1);
        last.e = res; last.own = res_own; last.valid = true;
    }
    *own = res_own;
    return res;
}

/* The entry of a full builtin name, as .repl.signature and :h pass it:
 * matched whole first, because the error path's lookup keys a name of 13
 * bytes or more on its cut-off prefix and finds nothing where several share
 * it (.sys.querylog, .sys.querylog.enable, .sys.querylog.stats). */
static const ray_sig_entry_t* entry_by_full_name(const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
        if (strcmp(sigs[i].name, name) == 0) return &sigs[i];
    for (size_t i = 0; i < sizeof own_sigs / sizeof *own_sigs; i++)
        if (strcmp(own_sigs[i].name, name) == 0) return &own_sigs[i];
    bool own;
    return find_entry(name, &own, NULL);
}

const char* ray_sig_usage(const char* name) {
    const ray_sig_entry_t* e = entry_by_full_name(name);
    return e ? e->usage : NULL;
}

const char* ray_sig_summary(const char* name) {
    const ray_sig_entry_t* e = entry_by_full_name(name);
    return e ? e->summary : NULL;
}

/* Does `msg` start with `cand` and then a colon, or "expects", "takes" or
 * "requires"?  ("select by: ..." is a query's own message, not a call shape.) */
static bool starts_with_word(const char* msg, const char* cand, size_t n) {
    if (n == 0 || strncmp(msg, cand, n) != 0) return false;
    if (msg[n] == ':') return true;
    if (msg[n] != ' ') return false;
    const char* w = msg + n + 1;
    return !strncmp(w, "expects", 7) || !strncmp(w, "takes", 5) || !strncmp(w, "requires", 8) ||
           !strncmp(w, "accepts", 7);
}

/* A message is the builtin's own when it begins with its name, written as the
 * builtin writes it: the name without a leading dot, without a trailing digit,
 * with its dashes dropped, or only its first or last dash-separated word
 * ("anti-join" says "antijoin:", "inner-join" says "join:", "scan-left" says
 * "scan:").  A call applied to a non-function says "call:". */
static bool own_message(const char* name, const char* msg) {
    if (!strncmp(msg, "call:", 5)) return true;
    if (*name == '.' && starts_with_word(msg, name, strlen(name))) return true;
    if (*name == '.') name++;
    if (!strcmp(name, "pmap")) name++;   /* pmap reports as map */
    size_t len = strlen(name);
    while (len > 1 && name[len - 1] >= '0' && name[len - 1] <= '9') len--;
    if (starts_with_word(msg, name, len)) return true;
    char joined[40] = {0};
    size_t j = 0;
    for (size_t i = 0; i < len && j + 1 < sizeof joined; i++)
        if (name[i] != '-') joined[j++] = name[i];
    joined[j] = '\0';
    if (starts_with_word(msg, joined, j)) return true;
    const char* dash = memchr(name, '-', len);
    if (!dash) return false;
    if (starts_with_word(msg, name, (size_t)(dash - name))) return true;
    return starts_with_word(msg, dash + 1, len - (size_t)(dash + 1 - name));
}

ray_t* ray_sig_note_err(const char* name, ray_t* result) {
    /* The builtin that was called is the innermost one that noted the error: a call
     * nested inside another keeps the shape of the first, as it keeps the message.
     * Checked first, so an error passing up through nested builtins is looked up once. */
    if (ray_error_usage()) return result;
    if (!result || !RAY_IS_ERR(result) || result == RAY_OOM_OBJ) return result;
    /* type, length, rank and arity are about a call; domain only when about the
     * argument list (below).  The code is at most 7 bytes in the object: compare it by
     * length and bytes, no copy and no strcmp. */
    const char* c = (const char*)result->sdata;
    size_t cn = result->slen;
    bool domain = cn == 6 && !memcmp(c, "domain", 6);
    bool call = domain || (cn == 4 && (!memcmp(c, "type", 4) || !memcmp(c, "rank", 4))) ||
                (cn == 5 && !memcmp(c, "arity", 5)) || (cn == 6 && !memcmp(c, "length", 6));
    if (!call) return result;
    bool own;
    const char* old = ray_err_msg(result);
    /* A domain error is a bad value, not a bad call, unless it is about the
     * argument list: told from the message before any table is read. */
    if (domain && (!old || !strstr(old, " arg"))) return result;
    const ray_sig_entry_t* e = find_entry(name, &own, old);
    if (!e) return result;
    /* Only the own-message table needs the guard. */
    if (own && (!old || !*old || !own_message(e->name, old))) return result;
    if (old && strchr(old, '(')) {
        /* A message that already names the call shape is left alone (only a message
         * with a parenthesis can: most have none, and skip the formatting). */
        char head[40];
        snprintf(head, sizeof head, "(%s ", e->name);
        if (strstr(old, head)) return result;
    }
    /* The message stays what the builtin wrote: the call shape is a field of its
     * own, which the REPL prints and `try` hands to its handler. */
    ray_error_usage_set(e->usage);
    return result;
}

/* Append `src` to buf at *n, never past cap - 1. */
static void put_str(char* buf, size_t cap, size_t* n, const char* src) {
    size_t l = strlen(src);
    if (l > cap - 1 - *n) l = cap - 1 - *n;
    memcpy(buf + *n, src, l);
    *n += l;
}

ray_t* ray_sig_arity(const char* name, const char* want, int got) {
    bool own;
    const ray_sig_entry_t* e = find_entry(name, &own, NULL);
    const char* shown = e ? e->name : name;
    /* "<name>: takes <want> argument(s), got <n>", written by hand: this is the message of
     * a try in a loop, and four printf conversions were most of its cost. */
    char msg[160];
    size_t n = 0;
    put_str(msg, sizeof msg, &n, shown);
    put_str(msg, sizeof msg, &n, ": takes ");
    put_str(msg, sizeof msg, &n, want);
    put_str(msg, sizeof msg, &n, strcmp(want, "1") ? " arguments, got " : " argument, got ");
    char digits[12];
    size_t d = 0;
    unsigned v = got < 0 ? 0u : (unsigned)got;
    do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < sizeof digits);
    char num[12];
    for (size_t i = 0; i < d; i++) num[i] = digits[d - 1 - i];
    num[d] = '\0';
    put_str(msg, sizeof msg, &n, num);
    msg[n] = '\0';
    ray_t* err = ray_error("arity", "%s", msg);
    if (e) ray_error_usage_set(e->usage);
    return err;
}
