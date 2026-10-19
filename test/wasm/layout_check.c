/* wasm32 layout check (make check-wasm32-layout): every generation-2 wire
 * struct has the same size and offsets on a 32-bit target.  Compiled with
 * clang --target=wasm32 -ffreestanding -fsyntax-only; the static asserts
 * in the headers are the test. */
#define RAY_ENC_IDX_WIRE_ONLY 1
#include "store/encfmt.h"
#include "store/encidx.h"
_Static_assert(sizeof(void*) == 4, "this check runs on a 32-bit target");
int ray_wasm_layout_ok = 1;
