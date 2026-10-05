# Compressed-columns P0 estimator

Build (release library first):

    make lib
    mkdir -p build
    cc -O2 -std=c17 -Wall -Wextra -Iinclude -Isrc bench/compress_estimate/main.c librayforce.a \
       -lm -lpthread -o build/compress_estimate

Run on a splayed table:

    build/compress_estimate --db /path/to/splayed --preds bench/compress_estimate/predicates.txt \
        --keep-index UserID > report.txt

Run on a parted database (one subdirectory per partition, each holding <table>/):

    build/compress_estimate --db /path/to/parted --table hits --preds bench/compress_estimate/predicates.txt \
        --keep-index UserID > report.txt

Report lines: `col` (per column: raw, encoded, ratio, index bytes), `pred` (parsed predicates),
`sym` (symfile FSST estimate, decompressed snapshot bytes, decode time), `group` (skip rates per
predicate group), `mix` (encoding mix), `breakdown` (whole table), `scenario` (sizes and the
combined-score factor at the size weight 0.1).

The engine's index sites do not consult parted segment indexes, so on a parted root the
skip rates and index sizes describe what the files would allow, not what the engine uses today
(the program prints a `note` line in that case).
