/*
 *   Copyright (c) 2025 Anton Kundenko <singaraiona@gmail.com>
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

#include "progress.h"
#include "term.h"
#include "format.h"
#include <stdio.h>

#define PROGRESS_BAR_WIDTH 40
#define PROGRESS_BAR_FILLED "█"
#define PROGRESS_BAR_EMPTY "░"

nil_t progress_init(progress_p progress, i64_t parts) {
    if (progress == NULL)
        return;

    progress->parts = parts;
    progress->completed = 0;
}

nil_t progress_tick(progress_p progress, i64_t parts, str_p label) {
    i64_t i, percentage, filled_width;
    if (progress == NULL)
        return;

    cursor_move_start();
    line_clear();

    progress->completed += parts;
    if (progress->completed > progress->parts)
        progress->completed = progress->parts;

    percentage = (progress->completed * 100) / progress->parts;
    filled_width = (PROGRESS_BAR_WIDTH * progress->completed) / progress->parts;

    printf(" ");
    for (i = 0; i < PROGRESS_BAR_WIDTH; i++) {
        if (i < filled_width)
            printf(PROGRESS_BAR_FILLED);
        else
            printf(PROGRESS_BAR_EMPTY);
    }

    printf(" %lld/%lld (%lld%%) - %s", progress->completed, progress->parts, percentage, label);
    fflush(stdout);
}

nil_t progress_finalize(progress_p progress) {
    if (progress == NULL)
        return;

    cursor_move_start();
    line_clear();
    fflush(stdout);
}
