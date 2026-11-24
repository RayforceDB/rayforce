#!/usr/bin/env python3
"""Regenerate the DELTA encoding fixtures in test/data/parquet/delta/.
Dev-only: make test never invokes this.

Fixtures are written by the external writer library imported below. Run:

    python3 test/data/parquet/gen-delta.py test/data/parquet/delta

Value formulas (10,000 rows unless stated):
  dbp-i64-v1/v2page/required/2page/multirg/snappy  id = 0..9999 (INT64)
  dbp-i32-v1                                         id = 0..9999 (INT32)
  dbp-i32-nulls-v2                                   id = i, null when i % 7 == 0
  dbp-i32-date                                       id = DATE 1970-01-01 + i, null when i % 7 == 0
  dbp-i32-overflow (4,096 rows)                      id = C[i % 13] with
      C = [INT32_MIN, INT32_MAX, INT32_MIN+1, INT32_MAX-1, 0, -1, 1,
           INT32_MIN+2, INT32_MAX-2, INT32_MIN+3, INT32_MAX-3, -2, 2]
  dbp-allnull (64 rows)                              id = null
  dlba-v1/v2page (1,000 rows)                        s = "track_%03d" % (i % 500)
  dba-v1/v2page (10,000 rows)                        s = "track_%03d" % (i % 500)
  dba-nulls-v2                                       s = "track_%03d" % (i % 500), null when i % 13 == 0
  dba-prefix-long (512 rows)                         s = "P"*65536 + "%04d" % i
  dba-empty (8 rows)                                 ["", "a", "", "ab", "", "abc", "", ""]
  unsupported-bss (8 rows)                           id = 0..7, BYTE_STREAM_SPLIT

Page layouts: data_page_version 1.0 except the *-v2page and dbp-i32-nulls-v2,
dbp-i32-overflow and dba-nulls-v2 files (2.0); use_dictionary=False;
compression none except dbp-i64-snappy (Snappy); dbp-i64-2page uses
data_page_size=128, write_batch_size=1024, row_group_size=10000;
dbp-i64-multirg uses row_group_size=2500. expected.txt in the output
directory records the oracle values these fixtures must reproduce.
"""
import datetime
import os
import sys

import pyarrow as pa
import pyarrow.parquet as pq

OUT = sys.argv[1] if len(sys.argv) > 1 else "test/data/parquet/delta"

DELTA_BINARY_PACKED = "DELTA_BINARY_PACKED"
DELTA_LENGTH_BYTE_ARRAY = "DELTA_LENGTH_BYTE_ARRAY"
DELTA_BYTE_ARRAY = "DELTA_BYTE_ARRAY"
BYTE_STREAM_SPLIT = "BYTE_STREAM_SPLIT"


NEUTRAL_WRITER = b"external-writer"
NEUTRAL_SCHEMA_KEY = b"schema:bytes"


def neutralise(path):
    """Rewrite the writer's footer stamps to the neutral forms.

    The writer names itself in `created_by` and stores one optional schema
    entry under a key of its own. Fixtures here describe format behaviour and
    do not name other products, so both strings are replaced in place with
    the forms recorded in manifest.txt. Each replacement keeps the byte
    length, so no offset, length field, checksum or file size moves.
    """
    written = pq.ParquetFile(path).metadata
    with open(path, "rb") as handle:
        blob = handle.read()
    stamp = written.created_by.encode()
    neutral = NEUTRAL_WRITER + b" (" + stamp.split(b" ", 1)[1] + b")"
    keys = list((written.metadata or {}).keys())
    if len(keys) != 1 or len(keys[0]) != len(NEUTRAL_SCHEMA_KEY):
        raise ValueError("%s: unexpected footer metadata keys %r" % (path, keys))
    for old, new in ((stamp, neutral), (keys[0], NEUTRAL_SCHEMA_KEY)):
        if len(old) != len(new) or blob.count(old) != 1:
            raise ValueError("%s: cannot neutralise %r in place" % (path, old))
        blob = blob.replace(old, new)
    with open(path, "wb") as handle:
        handle.write(blob)


def write(path, table, *, version="1.0", compression="none", encoding=None,
          row_group_size=None, data_page_size=None, write_batch_size=None):
    kw = dict(use_dictionary=False, compression=compression,
              data_page_version=version, column_encoding=encoding)
    if row_group_size is not None:
        kw["row_group_size"] = row_group_size
    if data_page_size is not None:
        kw["data_page_size"] = data_page_size
    if write_batch_size is not None:
        kw["write_batch_size"] = write_batch_size
    target = os.path.join(OUT, path)
    pq.write_table(table, target, **kw)
    neutralise(target)


def ints(n, dtype):
    return pa.array(range(n), type=dtype)


def strings(n, null_mod=0):
    vals = ["track_%03d" % (i % 500) if not (null_mod and i % null_mod == 0) else None
            for i in range(n)]
    return pa.array(vals, type=pa.string())


def main():
    os.makedirs(OUT, exist_ok=True)
    i64 = lambda vals: pa.array(vals, type=pa.int64())
    i32 = lambda vals: pa.array(vals, type=pa.int32())

    # DELTA_BINARY_PACKED, INT64, v1 and v2 pages.
    write("dbp-i64-v1.parquet", pa.table({"id": ints(10000, pa.int64())}),
          encoding={"id": DELTA_BINARY_PACKED})
    write("dbp-i64-v2page.parquet", pa.table({"id": ints(10000, pa.int64())}),
          version="2.0", encoding={"id": DELTA_BINARY_PACKED})

    # INT32, all present; nulls every 7th row, v2 page.
    write("dbp-i32-v1.parquet", pa.table({"id": ints(10000, pa.int32())}),
          encoding={"id": DELTA_BINARY_PACKED})
    write("dbp-i32-nulls-v2.parquet", pa.table({
        "id": pa.array([None if i % 7 == 0 else i for i in range(10000)],
                       type=pa.int32())}),
          version="2.0", encoding={"id": DELTA_BINARY_PACKED})

    # DATE (physical INT32, converted type 6), nulls every 7th row, v1 page.
    epoch = datetime.date(1970, 1, 1)
    dates = [None if i % 7 == 0 else epoch + datetime.timedelta(days=i)
             for i in range(10000)]
    write("dbp-i32-date.parquet",
          pa.table({"id": pa.array(dates, type=pa.date32())}),
          encoding={"id": DELTA_BINARY_PACKED})

    # INT32 wrapping deltas near both bounds. The 13-value cycle makes every
    # delta wrap; row 0 and row 4095 carry INT32_MIN (the native null).
    cycle = [-(2**31), 2**31 - 1, -(2**31) + 1, 2**31 - 2, 0, -1, 1,
             -(2**31) + 2, 2**31 - 3, -(2**31) + 3, 2**31 - 4, -2, 2]
    write("dbp-i32-overflow.parquet", pa.table({
        "id": i32([cycle[i % 13] for i in range(4096)])}),
          version="2.0", encoding={"id": DELTA_BINARY_PACKED})

    # All null, header-only DELTA stream.
    write("dbp-allnull.parquet", pa.table({
        "id": pa.array([None] * 64, type=pa.int32())}),
          encoding={"id": DELTA_BINARY_PACKED})

    # REQUIRED (non-nullable) INT64 column.
    required = pa.schema([pa.field("id", pa.int64(), nullable=False)])
    write("dbp-i64-required.parquet",
          pa.Table.from_arrays([ints(10000, pa.int64())], schema=required),
          encoding={"id": DELTA_BINARY_PACKED})

    # Two data pages in one row group; four row groups.
    write("dbp-i64-2page.parquet", pa.table({"id": ints(10000, pa.int64())}),
          encoding={"id": DELTA_BINARY_PACKED}, data_page_size=128,
          write_batch_size=1024, row_group_size=10000)
    write("dbp-i64-multirg.parquet", pa.table({"id": ints(10000, pa.int64())}),
          encoding={"id": DELTA_BINARY_PACKED}, row_group_size=2500)
    write("dbp-i64-snappy.parquet", pa.table({"id": ints(10000, pa.int64())}),
          compression="snappy", encoding={"id": DELTA_BINARY_PACKED})

    # DELTA_LENGTH_BYTE_ARRAY and DELTA_BYTE_ARRAY string fixtures.  The
    # DLBA pair is trimmed to 1,000 rows: enough to cross the 128-value
    # miniblock and 32-row batch seams the small-batch checks exercise.
    write("dlba-v1.parquet", pa.table({"name": strings(1000)}),
          encoding={"name": DELTA_LENGTH_BYTE_ARRAY})
    write("dlba-v2page.parquet", pa.table({"name": strings(1000)}),
          version="2.0", encoding={"name": DELTA_LENGTH_BYTE_ARRAY})
    write("dba-v1.parquet", pa.table({"name": strings(10000)}),
          encoding={"name": DELTA_BYTE_ARRAY})
    write("dba-v2page.parquet", pa.table({"name": strings(10000)}),
          version="2.0", encoding={"name": DELTA_BYTE_ARRAY})
    prefix = "P" * 65536
    write("dba-prefix-long.parquet", pa.table({
        "name": pa.array([prefix + "%04d" % i for i in range(512)], type=pa.string())}),
          encoding={"name": DELTA_BYTE_ARRAY})
    write("dba-nulls-v2.parquet", pa.table({"name": strings(10000, 13)}),
          version="2.0", encoding={"name": DELTA_BYTE_ARRAY})
    write("dba-empty.parquet", pa.table({
        "name": pa.array(["", "a", "", "ab", "", "abc", "", ""], type=pa.string())}),
          encoding={"name": DELTA_BYTE_ARRAY})

    # BYTE_STREAM_SPLIT refusal fixture.
    write("unsupported-bss.parquet", pa.table({"id": ints(8, pa.int32())}),
          encoding={"id": BYTE_STREAM_SPLIT})



if __name__ == "__main__":
    main()
