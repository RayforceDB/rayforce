-- DuckDB sort benchmark — matches bench_teide.c patterns
-- Usage: duckdb < bench/bench_duckdb.sql

-- Disable progress bar for cleaner output
SET enable_progress_bar = false;

-- 10M rows: reverse-ordered
CREATE TABLE t AS SELECT 10000000 - i AS v FROM range(10000000) t(i);
.timer on
SELECT sum(v) FROM (SELECT v FROM t ORDER BY v);
.timer off
DROP TABLE t;

-- 10M rows: random
CREATE TABLE t AS SELECT hash(i) % 100000000 AS v FROM range(10000000) t(i);
.timer on
SELECT sum(v) FROM (SELECT v FROM t ORDER BY v);
.timer off
DROP TABLE t;

-- 10M rows: already sorted
CREATE TABLE t AS SELECT i AS v FROM range(10000000) t(i);
.timer on
SELECT sum(v) FROM (SELECT v FROM t ORDER BY v);
.timer off
DROP TABLE t;

-- 10M rows: nearly sorted (pre-build with 1% swaps simulated via hash)
CREATE TABLE t AS SELECT v FROM (
    SELECT i AS v, row_number() OVER () AS rn FROM range(10000000) t(i)
) ORDER BY CASE WHEN hash(rn) % 100 = 0 THEN hash(rn) ELSE rn END;
.timer on
SELECT sum(v) FROM (SELECT v FROM t ORDER BY v);
.timer off
DROP TABLE t;
