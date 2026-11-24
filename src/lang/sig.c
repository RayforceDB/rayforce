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
 * own_sigs below, not here: an error from the code it runs is not its own. */
typedef struct { const char* name; const char* usage; } ray_sig_entry_t;

static const ray_sig_entry_t sigs[] = {
    /* arithmetic and comparison */
    {"+", "(+ <left: num, date, time, timestamp or timespan> <right: the same kinds>)"},
    {"-", "(- <left: num, date, time, timestamp or timespan> <right: the same kinds>)"},
    {"*", "(* <left: num or timespan> <right: num or timespan>)"},
    {"/", "(/ <left: num or timespan> <right: num>)"},
    {"%", "(% <left: num> <right: num>)"},
    {"div", "(div <left: num> <right: num>)"},
    {"pow", "(pow <base: num> <exponent: num>)"},
    {">", "(> <left: any> <right: a comparable value of the same kind>)"},
    {"<", "(< <left: any> <right: a comparable value of the same kind>)"},
    {">=", "(>= <left: any> <right: a comparable value of the same kind>)"},
    {"<=", "(<= <left: any> <right: a comparable value of the same kind>)"},
    {"==", "(== <left: any> <right: a value of the same kind>)"},
    {"!=", "(!= <left: any> <right: a value of the same kind>)"},
    {"not", "(not <x: bool or num, atom or vector>)"},
    {"neg", "(neg <x: num, atom or vector>)"},
    {"floor", "(floor <x: num, atom or vector>)"},
    {"ceil", "(ceil <x: num, atom or vector>)"},
    {"abs", "(abs <x: num, atom or vector>)"},
    {"sqrt", "(sqrt <x: num, atom or vector>)"},
    {"log", "(log <x: num, atom or vector>)"},
    {"exp", "(exp <x: num, atom or vector>)"},
    {"sin", "(sin <x: num, atom or vector>)"},
    {"asin", "(asin <x: num, atom or vector>)"},
    {"cos", "(cos <x: num, atom or vector>)"},
    {"acos", "(acos <x: num, atom or vector>)"},
    {"tan", "(tan <x: num, atom or vector>)"},
    {"atan", "(atan <x: num, atom or vector>)"},
    {"reciprocal", "(reciprocal <x: num, atom or vector>)"},
    {"signum", "(signum <x: num, atom or vector>)"},
    {"round", "(round <x: num, atom or vector>)"},
    /* aggregates over a vector */
    {"sum", "(sum <x: num, atom, vector or list>)"},
    {"prod", "(prod <x: num, atom, vector or list>)"},
    {"avg", "(avg <x: num, atom, vector or list>)"},
    {"med", "(med <x: num, atom, vector or list>)"},
    {"min", "(min <x: vector, atom or list>) or (min <a: num> <b: num>)"},
    {"max", "(max <x: vector, atom or list>) or (max <a: num> <b: num>)"},
    {"var", "(var <x: num, atom, vector or list>)"},
    {"var_pop", "(var_pop <x: num, atom, vector or list>)"},
    {"dev", "(dev <x: num, atom, vector or list>)"},
    {"dev_pop", "(dev_pop <x: num, atom, vector or list>)"},
    {"stddev", "(stddev <x: num, atom, vector or list>)"},
    {"stddev_pop", "(stddev_pop <x: num, atom, vector or list>)"},
    {"mode", "(mode <x: atom, vector or list>)"},
    {"all", "(all <x: numeric or bool vector>)"},
    {"any", "(any <x: numeric or bool vector>)"},
    {"count", "(count <x: any collection>)"},
    {"first", "(first <x: vector or list>)"},
    {"last", "(last <x: vector or list>)"},
    {"cov", "(cov <x: numeric vector> <y: numeric vector>)"},
    {"scov", "(scov <x: numeric vector> <y: numeric vector>)"},
    {"pearson_corr", "(pearson_corr <x: numeric vector> <y: numeric vector>)"},
    {"wavg", "(wavg <weights: numeric vector> <values: numeric vector>)"},
    {"wsum", "(wsum <weights: numeric vector> <values: numeric vector>)"},
    {"percentile", "(percentile <x: numeric vector> <p: num in 0..100>)"},
    {"quantile", "(quantile <x: numeric vector> <p: num in 0..1>)"},
    {".emb.cos-dist", "(.emb.cos-dist <a: numeric vector> <b: numeric vector>)"},
    {".emb.tokens", "(.emb.tokens <model: model handle> <text: str or vector of str>)"},
    {".emb.l2-dist", "(.emb.l2-dist <a: numeric vector> <b: numeric vector>)"},
    {".emb.inner-prod", "(.emb.inner-prod <a: numeric vector> <b: numeric vector>)"},
    {"mmu", "(mmu <a: numeric vector or list of rows> <b: numeric vector or list of rows>)"},
    {"lsq", "(lsq <b: numeric vector or list of rows> <a: square matrix as a list of rows>)"},
    /* windows: the window comes first */
    {"mavg", "(mavg <window: int> <x: numeric vector>)"},
    {"mcount", "(mcount <window: int> <x: numeric vector>)"},
    {"mdev", "(mdev <window: int> <x: numeric vector>)"},
    {"mmax", "(mmax <window: int> <x: numeric vector>)"},
    {"mmin", "(mmin <window: int> <x: numeric vector>)"},
    {"msum", "(msum <window: int> <x: numeric vector>)"},
    {"mvar", "(mvar <window: int> <x: numeric vector>)"},
    {"xrank", "(xrank <buckets: int> <x: numeric vector>)"},
    {"lag", "(lag <x: vector>)"},
    {"lead", "(lead <x: vector>)"},
    {"deltas", "(deltas <x: numeric vector>)"},
    {"ratios", "(ratios <x: numeric vector>)"},
    {"fills", "(fills <x: vector>)"},
    {"sums", "(sums <x: numeric vector>)"},
    {"avgs", "(avgs <x: numeric vector>)"},
    {"mins", "(mins <x: vector>)"},
    {"maxs", "(maxs <x: vector>)"},
    {"prds", "(prds <x: numeric vector>)"},
    {"differ", "(differ <x: vector>)"},
    /* sorting, ranking and sets */
    {"asc", "(asc <x: vector>)"},
    {"desc", "(desc <x: vector>)"},
    {"iasc", "(iasc <x: vector>)"},
    {"idesc", "(idesc <x: vector>)"},
    {"rank", "(rank <x: vector>)"},
    {"distinct", "(distinct <x: vector or list>)"},
    {"diverse", "(diverse <x: vector or list>)"},
    {"group", "(group <x: vector or list>)"},
    {"reverse", "(reverse <x: vector or list>)"},
    {"raze", "(raze <x: list of vectors>)"},
    {"where", "(where <mask: B8 vector>)"},
    {"til", "(til <n: i64 atom>)"},
    {"guid", "(guid <n: int>)"},
    {"bin", "(bin <sorted: I64 vector> <value: i64 atom or I64 vector>)"},
    {"binr", "(binr <sorted: I64 vector> <value: i64 atom or I64 vector>)"},
    {"top", "(top <x: vector> <n: int>)"},
    {"bot", "(bot <x: vector> <n: int>)"},
    {"take", "(take <collection: vector, list, str, dict or table> <count: int, or a [start amount] range>)"},
    {"drop", "(drop <collection: vector, list, str, dict or table> <count: int>)"},
    {"rotate", "(rotate <collection: vector, list, str, dict or table> <count: int>)"},
    {"filter", "(filter <x: vector, list, str or table> <mask: B8 vector, one boolean per element of x>)"},
    {"cut", "(cut <x: vector or list> <indices: sorted integer vector>)"},
    {"within", "(within <x: vector> <range: a [lo hi] vector>)"},
    {"find", "(find <x: vector or list> <value>)"},
    {"in", "(in <value: atom or vector> <set: vector or list>)"},
    {"not-in", "(not-in <value: atom or vector> <set: vector or list>)"},
    {"except", "(except <a: vector or list> <b: vector or list>)"},
    {"sect", "(sect <a: vector or list> <b: vector or list>)"},
    {"union", "(union <a: vector or list> <b: vector or list>)"},
    {"cross", "(cross <a: vector or list> <b: vector or list>)"},
    {"concat", "(concat <left: atom, vector, list, dict or table> <right: any two atoms, vectors or lists, but an atom and a vector only of the atom's type (a str and a STR vector join cell by cell); a dict with a dict; a table with a table that has each of its columns, of the same types>)"},
    {"fill", "(fill <replacement: atom> <x: vector>)"},
    {"xbar", "(xbar <x: numeric or temporal vector> <bucket: non-zero num>)"},
    {"rand", "(rand <count: i64 or i32 atom> <max: i64 or i32 atom, above 0>)"},
    {"at", "(at <collection: vector, list, str, dict or table> <index: int, an integer vector, or a dict key>)"},
    {"get", "(get <source: dict or table> <key: sym>)"},
    {"identical", "(identical <a: any> <b: any>)"},
    /* conversion and inspection */
    {"as", "(as <type: sym, e.g. 'i64> <value: any>)"},
    {"type", "(type <x: any>)"},
    {"nil?", "(nil? <x: any>)"},
    {"key", "(key <x: dict or table>)"},
    {"value", "(value <x: dict or table>)"},
    {"cols", "(cols <t: table>)"},
    {"meta", "(meta <t: table>)"},
    {"fkeys", "(fkeys <t: table>)"},
    {"ungroup", "(ungroup <t: grouped table>)"},
    {"row", "(row <t: table> <index: int>)"},
    {"dict", "(dict <keys: vector> <values: vector or list>)"},
    {"remove", "(remove <d: dict> <key>)"},
    {"unify", "(unify <a: dict or table> <b: dict or table>)"},
    {"union-all", "(union-all <a: table> <b: table>)"},
    {"table", "(table <names: sym vector> <columns: list of vectors>)"},
    {"xasc", "(xasc <t: table> <columns: sym or sym vector>)"},
    {"xdesc", "(xdesc <t: table> <columns: sym or sym vector>)"},
    {"xgroup", "(xgroup <t: table> <columns: sym or sym vector>)"},
    {"xkey", "(xkey <t: table> <columns: sym or sym vector>)"},
    {"xcol", "(xcol <t: table> <names: sym vector, one per column>)"},
    {"xcols", "(xcols <t: table> <columns: sym vector>)"},
    {"split", "(split <text: str or sym> <delimiter: str>)"},
    {"like", "(like <text: str, sym or a vector of them> <pattern: str with * and ?, or a str vector row by row>)"},
    {"str-find", "(str-find <text: str, sym or a vector of them> <pattern: str or sym>)"},
    {"str-join", "(str-join <items: str or sym atom, vector or list> <delimiter: str or sym>)"},
    {"strlen", "(strlen <x: str or sym>)"},
    {"sym-name", "(sym-name <ids: i64 atom or I64 vector>)"},
    {"trunc", "(trunc <unit: sym, one of year quarter month week day hour minute second> <x: date, time or timestamp>)"},
    {"date", "(date <x: timestamp, date or 0>)"},
    {"time", "(time <x: timestamp, time or 0>)"},
    {"timestamp", "(timestamp <ignored: any, usually 0>)"},
    {"yyyy", "(yyyy <x: date, time or timestamp>)"},
    {"mm", "(mm <x: date, time or timestamp>)"},
    {"dd", "(dd <x: date, time or timestamp>)"},
    {"dow", "(dow <x: date, time or timestamp>)"},
    {"doy", "(doy <x: date, time or timestamp>)"},
    {"hh", "(hh <x: date, time or timestamp>)"},
    {"minute", "(minute <x: date, time or timestamp>)"},
    {"ss", "(ss <x: date, time or timestamp>)"},
    {"parse", "(parse <source: str>)"},
    {"read", "(read <path: str>)"},
    {"read-bytes", "(read-bytes <path: str>)"},
    {"write", "(write <path: str> <content: str>)"},
    {"write-bytes", "(write-bytes <path: str> <bytes: U8 vector>)"},
    {".os.setenv", "(.os.setenv <name: str> <value: str>)"},
    {"exit", "(exit <code: int>)"},
    {".sys.cmd", "(.sys.cmd <command: str>)"},
    {".sys.trust", "(.sys.trust <lambda: a lambda, the sym naming one, or a list of them>)"},
    {".log.replay", "(.log.replay <path: str>)"},
    {".log.replay_report", "(.log.replay_report <path: str>)"},
    {".log.validate", "(.log.validate <path: str>)"},
    {".repl.complete", "(.repl.complete <prefix: str>)"},
    {"ser", "(ser <x: any>)"},
    {"de", "(de <bytes: U8 vector>)"},
    {"raise", "(raise <message: str>)"},
    {"dl-eval", "(dl-eval <program: dl-program>)"},
    {"dl-free", "(dl-free <program: dl-program>)"},
    {"dl-stratify", "(dl-stratify <program: dl-program>)"},
    {"dl-query", "(dl-query <program: dl-program> <relation: sym>)"},
    {"dl-provenance", "(dl-provenance <program: dl-program> <relation: sym>)"},
    {".graph.free", "(.graph.free <graph: graph handle>)"},
    {".graph.info", "(.graph.info <graph: graph handle>)"},
    {".idx.uses", "(.idx.uses <t: table>)"},
    {".attr.set", "(.attr.set <attribute: sym, one of sorted unique grouped parted> <x: vector>)"},
    {".col.link", "(.col.link <target: sym naming a table> <v: integer vector>)"},
    {".parquet.meta", "(.parquet.meta <path: nonempty str>)"},
    {".attr.get", "(.attr.get <x: vector>)"},
    {".attr.drop", "(.attr.drop <x: vector>)"},
    {".col.link?", "(.col.link? <x: vector>)"},
    {".col.target", "(.col.target <x: vector>)"},
    {".col.unlink", "(.col.unlink <x: vector>)"},
    {".idx.bloom", "(.idx.bloom <x: vector>)"},
    {".idx.drop", "(.idx.drop <x: vector>)"},
    {".idx.has?", "(.idx.has? <x: vector>)"},
    {".idx.hash", "(.idx.hash <x: vector>)"},
    {".idx.sort", "(.idx.sort <x: vector>)"},
    {".idx.zone", "(.idx.zone <x: vector>)"},
    {".fs.list", "(.fs.list <path: str>)"},
    {".fs.size", "(.fs.size <path: str>)"},
    {".os.getenv", "(.os.getenv <name: str>)"},
    {"flip", "(flip <x: numeric vector or non-empty list of numeric rows>)"},
    {"gram", "(gram <x: numeric vector or non-empty list of numeric rows>)"},
    {"inv", "(inv <x: numeric vector or non-empty list of numeric rows>)"},
    {"norm", "(norm <x: numeric vector (f32, f64, i32 or i64), or a list of them>)"},
    {"lower", "(lower <x: str or sym, atom or vector>)"},
    {"upper", "(upper <x: str or sym, atom or vector>)"},
    {"trim", "(trim <x: str or sym, atom or vector>)"},
    {".emb.hnsw.free", "(.emb.hnsw.free <index: hnsw index handle>)"},
    {".emb.hnsw.info", "(.emb.hnsw.info <index: hnsw index handle>)"},
    {".emb.hnsw.get", "(.emb.hnsw.get <path: str>)"},
    {".emb.model.get", "(.emb.model.get <dir: str>)"},
    {".emb.model.info", "(.emb.model.info <model: model handle>)"},
    /* clock */
    {".time.sleep", "(.time.sleep <ms: int or timespan>)"},
};

/* Builtins that run other code (callbacks, expressions, queries) or take a
 * variable argument list.  An error that passes through them from the code
 * they run is not theirs, so their usage line is added only to a message
 * that starts with their own name (see own_message). */
static const ray_sig_entry_t own_sigs[] = {
    {"map", "(map <fn: fn> <x: vector or list>) or (map <fn: fn> <val: any> <x: vector or list>)"},
    {"pmap", "(pmap <fn: fn> <x: vector or list>) or (pmap <fn: fn> <val: any> <x: vector or list>)"},
    {"map-left", "(map-left <fn: fn> <left: any> <right: any>)"},
    {"map-right", "(map-right <fn: fn> <left: any> <right: any>)"},
    {"apply", "(apply <fn: fn> <left: vector or list> <right: vector or list>)"},
    {"fold", "(fold <fn: fn> <init: any> <x: vector or list>) or (fold <fn: fn> <x: vector or list>)"},
    {"fold-left", "(fold-left <fn: fn> <init: any> <x: vector or list>) or (fold-left <fn: fn> <x: vector or list>)"},
    {"fold-right", "(fold-right <fn: fn> <init: any> <x: vector or list>) or (fold-right <fn: fn> <x: vector or list>)"},
    {"fold-while", "(fold-while <pred: fn> <fn: fn> <init: any> <x: vector or list>)"},
    {"scan", "(scan <fn: fn> <x: vector or list>)"},
    {"scan-left", "(scan-left <fn: fn> <x: vector or list>)"},
    {"scan-right", "(scan-right <fn: fn> <x: vector or list>)"},
    {"prior", "(prior <fn: fn> <x: vector or list>)"},
    {"if", "(if <cond: bool> <then> [<cond> <then> ...] [<else>])"},
    {"while", "(while <cond: bool> <body>...)"},
    {"times", "(times <n: int> <body>...) or (times [<name: sym> <n: int>] <body>...)"},
    {"fn", "(fn <params: sym vector> <body>...)"},
    {"quote", "(quote <x: any>)"},
    {"return", "(return [<x: any>])"},
    {"alter", "(alter <name: quoted sym> <op: fn or sym> <arg: any>...)"},
    {"modify", "(modify <t: table> <column: sym> <fn: fn>)"},
    {"pivot", "(pivot <t: table> <index: sym> <pivot: sym> <value: sym> <agg: fn>)"},
    {"insert", "(insert <target: quoted name of a table, list or vector> <value: row or values>)"},
    {"upsert", "(upsert <t: table> <key: sym or sym vector> <row: dict>)"},
    {"select", "(select <query: dict with from:, and optionally by:, where:, take:, and named columns>)"},
    {"update", "(update <query: dict with from:, and optionally by:, where:, and named columns>)"},
    {"window", "(window <query: dict with from:, part:, order:, frame: and funcs:>)"},
    {"inner-join", "(inner-join <keys: sym vector> <left: table> <right: table>)"},
    {"left-join", "(left-join <keys: sym vector> <left: table> <right: table>)"},
    {"full-join", "(full-join <keys: sym vector> <left: table> <right: table>)"},
    {"anti-join", "(anti-join <keys: sym vector> <left: table> <right: table>)"},
    {"semi-mask", "(semi-mask <keys: sym vector> <left: table> <right: table>)"},
    {"anti-mask", "(anti-mask <keys: sym vector> <left: table> <right: table>)"},
    {"asof-join", "(asof-join <keys: sym vector, the last one the time key> <left: table> <right: table>)"},
    {"window-join", "(window-join <keys: sym vector, the last one the time key> <intervals> <left: table> <right: table> <aggs: dict>)"},
    {"window-join1", "(window-join1 <keys: sym vector, the last one the time key> <intervals> <left: table> <right: table> <aggs: dict>)"},
    {".db.parted.get", "(.db.parted.get <root: str> <table: sym>)"},
    {".db.parted.tables", "(.db.parted.tables <root: str>)"},
    {".db.parted.fill", "(.db.parted.fill <root: str>)"},
    {".db.splayed.get", "(.db.splayed.get <dir: str> [<options>])"},
    {".db.splayed.set", "(.db.splayed.set <dir: str> <table: table> [<options>...])"},
    {".csv.splayed", "(.csv.splayed [<schema>] [<options: dict>] <source: str path> <target: str dir>)"},
    {".csv.parted", "(.csv.parted [<schema>...] [<options: dict>] <source: str path> <root: str dir> <table: sym>)"},
    {".parquet.read", "(.parquet.read [<columns>] <path: nonempty str>)"},
    {".parquet.scan", "(.parquet.scan [<options>] <path: nonempty str>)"},
    {".parquet.splayed", "(.parquet.splayed [<types>] <path: nonempty str> <dir: str>)"},
    {".parquet.parted", "(.parquet.parted [<types>] <paths: nonempty str, or a list of them> <root: str> <table: sym>)"},
    {".parquet.each", "(.parquet.each [<columns>] <path: nonempty str> <callback: fn>)"},
    {"let", "(let <name: sym> <value: any>)"},
    {"set", "(set <name: sym> <value: any>)"},
    {"try", "(try <expr> <handler: fn of the error, or a fallback value>)"},
    {"eval", "(eval <expr: a parsed expression>)"},
    {"load", "(load <path: str>)"},
    {"rc", "(rc <x: any>)"},
    {".mem.objsize", "(.mem.objsize <x: any>)"},
    {"resolve", "(resolve <name: sym> ...)"},
    {"rule", "(rule <head: (name ?var ...)> <body: clause>...)"},
    {"env", "(env <ignored: any, usually 0>)"},
    {"read-csv", "(read-csv [<names: sym vector>] [<types: sym vector>] <path: str>)"},
    {".csv.read", "(.csv.read [<names: sym vector>] [<types: sym vector>] <path: str>)"},
    {"write-csv", "(write-csv <t: table> <path: str> [<options>])"},
    {".csv.write", "(.csv.write <t: table> <path: str> [<options>])"},
    {"read-value", "(read-value <text: str> [<limits: dict>])"},
    {"read-limits", "(read-limits)"},
    {"format", "(format <template: str, each % marks a value> <value: any>...)"},
    {"delete", "(delete <query: dict with from: and where:>)"},
    {"del", "(del <name: quoted sym or dotted path>)"},
    {".emb.hnsw.set", "(.emb.hnsw.set <path: str> <index: hnsw index handle>)"},
    {".ipc.post", "(.ipc.post <handle: i32 or i64> <message: any>)"},
    {".log.open", "(.log.open <mode: sym, `async or `sync> <base: str>)"},
    {".log.close", "(.log.close)"},
    {".log.roll", "(.log.roll)"},
    {".log.snapshot", "(.log.snapshot)"},
    {".log.sync", "(.log.sync)"},
    {".log.posts", "(.log.posts [<on: i64, 0 or 1>])"},
    {".log.purge", "(.log.purge)"},
    {".sys.prof", "(.sys.prof)"},
    {".sys.profile", "(.sys.profile)"},
    {".sys.querylog", "(.sys.querylog)"},
    {".graph.build", "(.graph.build <edges: table> <src: sym> <dst: sym> [<weight: sym>])"},
    {".graph.pagerank", "(.graph.pagerank <graph: graph handle> [<iterations: int>] [<damping: f64>])"},
    {".graph.connected", "(.graph.connected <graph: graph handle>)"},
    {".graph.degree", "(.graph.degree <graph: graph handle>)"},
    {".graph.topsort", "(.graph.topsort <graph: graph handle>)"},
    {".graph.cluster", "(.graph.cluster <graph: graph handle>)"},
    {".graph.mst", "(.graph.mst <graph: graph handle>)"},
    {".graph.louvain", "(.graph.louvain <graph: graph handle> [<max-iter: int>])"},
    {".graph.dijkstra", "(.graph.dijkstra <graph: graph handle> <src: int> [<dst: int>] [<max-depth: int>])"},
    {".graph.dfs", "(.graph.dfs <graph: graph handle> <src: int> [<max-depth: int>])"},
    {".graph.betweenness", "(.graph.betweenness <graph: graph handle> [<sample: int>])"},
    {".graph.closeness", "(.graph.closeness <graph: graph handle> [<sample: int>])"},
    {".graph.random-walk", "(.graph.random-walk <graph: graph handle> <src: int> [<walk-len: int>])"},
    {".graph.k-shortest", "(.graph.k-shortest <graph: graph handle> <src: int> <dst: int> <k: int>)"},
    {".graph.shortest-path", "(.graph.shortest-path <graph: graph handle> <src: int> <dst: int> [<max-depth: int>])"},
    {".graph.expand", "(.graph.expand <graph: graph handle> <src: int> [<direction: sym>])"},
    {".graph.var-expand", "(.graph.var-expand <graph: graph handle> <src: int> <min-depth: int> <max-depth: int> [<direction: sym>] [<track-path: bool>])"},
    {".idx.info", "(.idx.info <col: vector>) or (.idx.info <t: table> <column: sym>)"},
    {".emb.hnsw.build", "(.emb.hnsw.build <col: list of numeric vectors> [<metric>] [<M: int>] [<ef_c: int>])"},
    {".emb.encode", "(.emb.encode <model: model handle> <text: str or vector of str> [<max-tokens: int>])"},
    {".emb.knn", "(.emb.knn <col: list of numeric vectors> <query: numeric vector> <k: int> [<metric>])"},
    {".emb.ann", "(.emb.ann <index: hnsw index handle> <query: numeric vector> <k: int> [<ef_s: int>])"},
    {"replace", "(replace <text: str or sym> <from: str or sym> <to: str or sym>)"},
    {"substr", "(substr <text: str or sym> <start: int, or I64/I32 vector> <length: int, or I64/I32 vector>)"},
    {".mem.ts", "(.mem.ts <expr>)"},
    {".sys.args", "(.sys.args)"},
    {".sys.build", "(.sys.build)"},
    {".sys.info", "(.sys.info)"},
    {".sys.mem", "(.sys.mem)"},
    {".sys.env", "(.sys.env [<name: str>])"},
    {".sys.gc", "(.sys.gc [<mode: i64, 0 or 1>])"},
    {".sys.join.par.min", "(.sys.join.par.min [<rows: i64>])"},
    {".sys.timeit", "(.sys.timeit [<on: i64, 0 or 1>])"},
    {".sys.profile.enable", "(.sys.profile.enable [<on: i64, 0 or 1>])"},
    {".sys.profile.reset", "(.sys.profile.reset)"},
    {".sys.querylog.enable", "(.sys.querylog.enable [<on: i64, 0 or 1>])"},
    {".time.now", "(.time.now)"},
    {"datoms", "(datoms)"},
    {"assert-fact", "(assert-fact <db: datoms table> <entity> <attr: sym> <value>)"},
    {"retract-fact", "(retract-fact <db: datoms table> <entity> <attr: sym> <value>)"},
    {"scan-eav", "(scan-eav <db: datoms table> <attr: sym> [<options>])"},
    {"pull", "(pull <db: datoms table> <entity> [<attrs: sym vector>])"},
    {"dl-program", "(dl-program)"},
    {"dl-add-edb", "(dl-add-edb <program: dl-program> <name: sym> <table: table> <arity: int>)"},
    {"query", "(query <db: datoms table> (find <vars>...) (where <clauses>...) [(rules ...)])"},
    {"repr", "(repr <x: any> [<max-bytes: int>])"},
    {"timeit", "(timeit <expr>)"},
    {"and", "(and <a: bool or num> <b: bool or num> ...)"},
    {"or", "(or <a: bool or num> <b: bool or num> ...)"},
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

const char* ray_sig_usage(const char* name) {
    bool own;
    const ray_sig_entry_t* e = name ? find_entry(name, &own, NULL) : NULL;
    return e ? e->usage : NULL;
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
