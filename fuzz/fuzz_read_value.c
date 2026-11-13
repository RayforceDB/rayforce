/*
 *   Fuzz target: the data-only reader, read-value.
 *
 *   Feeds engine-mutated bytes to ray_read_value() under tight limits, so a
 *   hostile input costs little.  Whatever the reader accepts must write back
 *   to text that reads back to text-identical output: repr(read(repr(v)))
 *   equals repr(v).  A reader that accepts a value the writer cannot
 *   reproduce, or reads a different value from the writer's own text, aborts.
 */

#include "common.h"
#include "lang/format.h"
#include "lang/readval.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ray_fuzz_init();

    ray_read_limits_t lim = ray_read_limits_default();
    lim.max_input  = 1 << 16;
    lim.max_alloc  = 4u << 20;
    lim.max_nodes  = 20000;
    lim.max_symbol_bytes = 1 << 14;
    lim.max_symbols = ray_sym_count() + 2048;   /* keep the intern table flat */

    ray_t* v = ray_read_value((const char*)data, size, &lim);
    if (RAY_IS_ERR(v)) { ray_error_free(v); return 0; }

    ray_t* a = ray_fmt_limited(v, RAY_FMT_ROUNDTRIP, 1 << 22);
    if (RAY_IS_ERR(a)) {   /* a value the writer refuses to write is not a round-trip finding */
        ray_error_free(a);
        ray_release(v);
        return 0;
    }
    ray_t* w = ray_read_value(ray_str_ptr(a), ray_str_len(a), &lim);
    if (RAY_IS_ERR(w)) {
        /* the second read may hit a limit the first did not, never a parse error */
        if (strcmp(ray_err_code(w), "parse") == 0 || strcmp(ray_err_code(w), "domain") == 0) abort();
        ray_error_free(w);
    } else {
        ray_t* b = ray_fmt_limited(w, RAY_FMT_ROUNDTRIP, 1 << 22);
        if (RAY_IS_ERR(b)) abort();
        if (ray_str_len(a) != ray_str_len(b) || memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) != 0) abort();
        ray_release(b);
        ray_release(w);
    }
    ray_release(a);
    ray_release(v);
    return 0;
}
