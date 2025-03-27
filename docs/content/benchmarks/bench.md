# :material-speedometer: DB(s) Benchmark

For benches we are using the following tool:
[H2OAI Bench](https://h2oai.github.io/db-benchmark)

## Prerequisites

Ubuntu:

```sh
sudo apt install r-base
```

Run `R` and then in a R console type: 

```sh
install.packages("data.table")
```

Exit from R and type in a terminal:

```sh
Rscript _data/groupby-datagen.R 1e7 1e2 0 0
```

## Group By behchmark

Dataset: G1_1e7_1e2_0_0.csv (10 million rows)

### DuckDB (multithread turned on)

```.timer "on"```

Load CSV: ```create table t as SELECT * FROM read_csv('./db-benchmark/G1_1e7_1e2_0_0.csv');```

- Q1: ```select id1, sum(v1) AS v1 from t group by id1; --> 63ms```
- Q2: ```select id1, id2, sum(v1) AS v1 from t group by id1, id2; --> 153ms```
- Q3: ```select id3, sum(v1) AS v1, mean(v3) AS v3 from t group by id3; --> 360ms```
- Q4: ```select id4, mean(v1) AS v1, mean(v2) AS v2, mean(v3) AS v3 from t group by id4; --> 23ms```
- Q5: ```select id6, sum(v1) AS v1, sum(v2) AS v2, sum(v3) AS v3 from t group by id6; --> 322ms```
- Q6: ```select id3, max(v1)-min(v2) AS range_v1_v2 from t group by id3; --> 330ms```
- Q7: ```select id1, id2, id3, id4, id5, id6, sum(v3) AS v3, count(*) AS count from t group by id1, id2, id3, id4, id5, id6; --> 878ms```

### DuckDB (multithread turned off)

```.timer "on"```
```SET threads = 1;```

Load CSV: ```create table t as SELECT * FROM read_csv('./db-benchmark/G1_1e7_1e2_0_0.csv');```

- Q1: ```select id1, sum(v1) AS v1 from t group by id1; --> 347ms```
- Q2: ```select id1, id2, sum(v1) AS v1 from t group by id1, id2; --> 690ms```
- Q3: ```select id3, sum(v1) AS v1, mean(v3) AS v3 from t group by id3; --> 601ms```
- Q4: ```select id4, mean(v1) AS v1, mean(v2) AS v2, mean(v3) AS v3 from t group by id4; --> 108ms```
- Q5: ```select id6, sum(v1) AS v1, sum(v2) AS v2, sum(v3) AS v3 from t group by id6; --> 440ms```
- Q6: ```select id3, max(v1)-min(v2) AS range_v1_v2 from t group by id3; --> 528ms```
- Q7: ```select id1, id2, id3, id4, id5, id6, sum(v3) AS v3, count(*) AS count from t group by id1, id2, id3, id4, id5, id6; --> 3269ms```

### ClickHouse 

Load CSV: ```CREATE TABLE t (id1 String, id2 String, id3 String, id4 Int64, id5 Int64, id6 Int64, v1 Int64, v2 Int64, v3 Float64) ENGINE = Memory;```

clickhouse-client -q "INSERT INTO default.t FORMAT CSV" < ./db-benchmark/G1_1e7_1e2_0_0.csv

- Q1: ```select id1, sum(v1) AS v1 from t group by id1; --> 51ms```
- Q2: ```select id1, id2, sum(v1) AS v1 from t group by id1, id2; --> 189ms```
- Q3: ```select id3, sum(v1) AS v1, avg(v3) AS v3 from t group by id3; --> 235ms```
- Q4: ```select id4, avg(v1) AS v1, avg(v2) AS v2, avg(v3) AS v3 from t group by id4; --> 47ms```
- Q5: ```select id6, sum(v1) AS v1, sum(v2) AS v2, sum(v3) AS v3 from t group by id6; --> 265ms```
- Q6: ```select id3, max(v1)-min(v2) AS range_v1_v2 from t group by id3; --> 249ms```
- Q7: ```select id1, id2, id3, id4, id5, id6, sum(v3) AS v3, count(*) AS count from t group by id1, id2, id3, id4, id5, id6; --> 1732ms```


### ? (4.0)

Load CSV: ```t: ("SSSJJJJJF";enlist",") 0: hsym `$":./db-benchmark/G1_1e7_1e2_0_0.csv"```

- Q1: ```\t select v1: sum v1 by id1 from t --> 59ms```
- Q2: ```\t select v1: sum v1 by id1, id2 from t --> 143ms```
- Q3: ```\t select v1: sum v1, v3: avg v3 by id3 from t --> 166ms```
- Q4: ```\t select v1: avg v1, v2: avg v2, v3: avg v3 by id4 from t --> 99ms```
- Q5: ```\t select v1: sum v1, v2: sum v2, v3: sum v3 by id6 from t --> 156ms```
- Q6: ```\t select range_v1_v2: (max v1) - (min v2) by id3 from t --> 551ms```
- Q7: ```\t select v3: sum v3, cnt: count i by id1, id2, id3, id4, id5, id6 from t --> 4497ms```

### Rayforce

Load CSV: ```(set t (csv [Symbol Symbol Symbol I64 I64 I64 I64 I64 F64] "./db-benchmark/G1_1e7_1e2_0_0.csv"))```

- Q1: ```\t (select {v1: (sum v1) from: t by: id1}) --> 60ms```
- Q2: ```\t (select {v1: (sum v1) from: t by: {id1: id1 id2: id2}}) --> 74ms```
- Q3: ```\t (select {v1: (sum v1) v3: (avg v3) from: t by: id3}) --> 118ms```
- Q4: ```\t (select {v1: (avg v1) v2: (avg v2) v3: (avg v3) from: t by: id4}) --> 72ms```
- Q5: ```\t (select {v1: (sum v1) v2: (sum v2) v3: (sum v3) from: t by: id6}) --> 122ms```
- Q6: ```\t (select {range_v1_v2: (- (max v1) (min v2)) from: t by: id3}) --> 104ms```
- Q7: ```\t (select {v3: (sum v3) count: (map count v3) from: t by: {id1: id1 id2: id2 id3: id3 id4: id4 id5: id5 id6: id6}}) --> 1394ms```

### ThePlatform

Load CSV: ```t: ("SSSJJJJJF";enlist",") 0: `$":./db-benchmark/G1_1e7_1e2_0_0.csv"```

- Q1: ```\t 0N#.?[t;();`id1!`id1;`v1!(sum;`v1)] --> 213ms```
- Q2: ```\t 0N#.?[t;();`id1`id2!`id1`id2;`v1!(sum;`v1)] --> 723ms```
- Q3: ```\t 0N#.?[t;();`id3!`id3;`v2`v3!((sum;`v2);(avg;`v3))] --> 507ms```
- Q4: ```\t 0N#.?[t;();`id4!`id4;`v1`v2`v3!((avg;`v1);(avg;`v2);(avg;`v3))] --> 285ms```
- Q5: ```\t 0N#.?[t;();`id6!`id6;`v1`v2`v3!((sum;`v1);(sum;`v2);(sum;`v3))] --> 488ms```
- Q6: ```\t 0N#.?[?[t;();`id3!`id3;`v1`v2!((max;`v1);(min;`v2))];();0b;`id3`range_v1_v2!(`id3;(-;`v1;`v2))] --> 465ms```
- Q7: ```\t 0N#.?[t;();`id1`id2`id3`id4`id5`id6!`id1`id2`id3`id4`id5`id6;`v3`count!((sum;`v3);(count;`v3))] --> 15712ms```

### Group By Results

| DB                              | Q1  | Q2  | Q3  | Q4  | Q5  | Q6  | Q7    |
| ------------------------------- | --- | --- | --- | --- | --- | --- | ----- |
| DuckDB (multithread turned on)  | 63  | 153 | 360 | 23  | 322 | 330 | 878   |
| DuckDB (multithread turned off) | 347 | 690 | 601 | 108 | 440 | 528 | 3269  |
| ClickHouse                      | 51  | 189 | 235 | 47  | 265 | 249 | 1732  |
| ? (4.0)                         | 59  | 143 | 166 | 99  | 156 | 551 | 4497  |
| Rayforce                        | 60  | 74  | 118 | 72  | 122 | 104 | 1394  |
| ThePlatform                     | 213 | 723 | 507 | 285 | 488 | 465 | 15712 |

## Join Benchmark

```sh
Rscript _data/join-datagen.R 1e7 1e2 0 0
```

### DuckDB (multithread turned on)

```.timer "on"```

Load CSV:

- x: ```create table x as SELECT * FROM read_csv('./db-benchmark/J1_1e7_NA_0_0.csv');```
- y: ```create table y as SELECT * FROM read_csv('./db-benchmark/J1_1e7_1e7_0_0.csv');```

Queries:

- Q1: ```select * from x left join y on x.id1 = y.id1 and x.id2 = y.id2; --> OOM```
- Q2: ```select * from x inner join y on x.id1 = y.id1 and x.id2 = y.id2; --> OOM```

### DuckDB (multithread turned off)

```.timer "on"```
```SET threads = 1;```

Queries:

- Q1: ```select * from x left join y on x.id1 = y.id1 and x.id2 = y.id2; --> OOM```
- Q2: ```select * from x inner join y on x.id1 = y.id1 and x.id2 = y.id2; --> OOM```

### ClickHouse

Load CSV:

- ```CREATE TABLE x (id1 Int64, id2 Int64, id3 Int64, id4 String, id5 String, id6 String, v1 Float64) ENGINE = Memory;```
- ```CREATE TABLE y (id1 Int64, id2 Int64, id3 Int64, id4 String, id5 String, id6 String, v2 Float64) ENGINE = Memory;```

clickhouse-client -q "INSERT INTO default.x FORMAT CSV" < ./db-benchmark/J1_1e7_NA_0_0.csv
clickhouse-client -q "INSERT INTO default.y FORMAT CSV" < ./db-benchmark/J1_1e7_1e7_0_0.csv

Queries:

- Q1: ```select * from x left join y on x.id1 = y.id1 and x.id2 = y.id2; --> OOM```
- Q2: ```select * from x inner join y on x.id1 = y.id1 and x.id2 = y.id2; --> OOM```

### ? (4.0)

Load CSV:

- x: ```("JJJSSSF";enlist",") 0: `$":./db-benchmark/J1_1e7_NA_0_0.csv"```
- y: ```("JJJSSSF";enlist",") 0: `$":./db-benchmark/J1_1e7_1e7_0_0.csv"```
  
Queries:

- Q1: ```\t x lj 2!y --> 3174ms```
- Q2: ```\t x ij 2!y --> 3098ms```

### Rayforce

Load CSV:

- ```(set x (csv [I64 I64 I64 Symbol Symbol Symbol F64] "./db-benchmark/J1_1e7_NA_0_0.csv"))```
- ```(set y (csv [I64 I64 I64 Symbol Symbol Symbol F64] "./db-benchmark/J1_1e7_1e7_0_0.csv"))```

Queries:

- Q1: ```\t (lj [id1 id2] x y) --> 3149ms```
- Q2: ```\t (ij [id1 id2] x y) --> 1610ms```

### ThePlatform

Load CSV:

- x: ```("JJJSSSF";enlist",") 0: `$":./db-benchmark/J1_1e7_NA_0_0.csv"```
- y: ```("JJJSSSF";enlist",") 0: `$":./db-benchmark/J1_1e7_1e7_0_0.csv"```

Queries:

- Q1: ```\t 0N#.(?[lj[(`x;x);(`y;y);((~`x`id1;~`x`id2);(~`y`id1;~`y`id2))]; (); 0b; `a`b`c`d`e`f`g`h!(~`y`id1;~`y`id2;`y`id3;~`y`id4;~`y`id5;~`y`id6;~`x`v1;~`y`v2)]) --> 23987ms```
 
- Q2: ```\t 0N#.(?[ij[(`x;x);(`y;y);((~`x`id1;~`x`id2);(~`y`id1;~`y`id2))]; (); 0b; `a`b`c`d`e`f`g`h!(~`y`id1;~`y`id2;`y`id3;~`y`id4;~`y`id5;~`y`id6;~`x`v1;~`y`v2)]) --> 34104ms```
  

### Join Results

| DB                              | Q1    | Q2    |
| ------------------------------- | ----- | ----- |
| DuckDB (multithread turned on)  | OOM   | OOM   |
| DuckDB (multithread turned off) | OOM   | OOM   |
| ClickHouse                      | OOM   | OOM   |
| ? (4.0)                         | 3174  | 3098  |
| Rayforce                        | 3149  | 1610  |
| ThePlatform                     | 23987 | 34104 |
