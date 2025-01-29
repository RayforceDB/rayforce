# :material-card-multiple: Reference card

## Builtin functions

<table class="ray-dense" markdown>
<tbody markdown>

<tr markdown><td markdown>cmp</td>
<td markdown>
  [eq](cmp/eq.md), [ne](cmp/ne.md), [lt](cmp/lt.md), [le](cmp/le.md), [gt](cmp/gt.md), [ge](cmp/ge.md)
</td>
</tr>

<tr markdown><td markdown>compose</td>
<td markdown>
  [as](compose/as.md), [concat](compose/concat.md), [dict](compose/dict.md), [table](compose/table.md), [group](compose/group.md), [guid](compose/guid.md), [list](compose/list.md), [enlist](compose/enlist.md), [rand](compose/rand.md), [reverse](compose/reverse.md), [til](compose/til.md), [distinct](compose/distinct.md)
</td>
</tr>

<tr markdown><td markdown>control</td>
  <td markdown>
  [if](control/if.md), [try](control/try.md), [raise](control/raise.md), [exit](control/exit.md),
  [do](control/do.md), [return](control/return.md)
  </td>
</tr>

<tr markdown><td markdown>env</td>
<td markdown>
  [set](env/set.md), [let](env/let.md), [env](env/env.md), [memstat](env/memstat.md)
</td>
</tr>

<tr markdown><td markdown>format</td>
<td markdown>
  [format](format/format.md), [print](format/print.md),
  [println](format/println.md), [show](format/show.md)
</td>
</tr>

<tr markdown><td markdown>io</td>
<td markdown>
  [read](io/read.md), [write](io/write.md), [read-csv](io/read_csv.md), [get-parted](io/get_parted.md),
  [get-splayed](io/get_splayed.md), [get](io/get.md), [hopen](io/hopen.md), [hclose](io/hclose.md),
  [set-parted](io/set_parted.md), [set-splayed](io/set_splayed.md)
</td>
</tr>

<tr markdown><td markdown>items</td>
<td markdown>
  [at](items/at.md), [enum](items/enum.md), [except](items/except.md), [filter](items/filter.md), [find](items/find.md), [first](items/first.md), [in](items/in.md), [key](items/key.md), [last](items/last.md), [sect](items/sect.md), [take](items/take.md), [union](items/union.md), [where](items/where.md), [within](items/within.md)
</td>
</tr>

<tr markdown><td markdown>iter</td>
<td markdown>
  [apply](iter/apply.md), [map](iter/map.md), [fold](iter/fold.md)
</td>
</tr>

<tr markdown><td markdown>join</td>
<td markdown>
  [left-join](join/left_join.md), [inner-join](join/inner_join.md)
</td>
</tr>

<tr markdown><td markdown>logic</td>
<td markdown>
  [and](logic/and.md), [or](logic/or.md), [not](logic/not.md), [like](logic/like.md), [nil?](logic/isnil.md)
</td>
</tr>

<tr markdown><td markdown>math</td>
  <td markdown>
    [+](math/add.md), [-](math/sub.md), [*](math/mul.md), [/](math/div.md), [%](math/mod.md), [avg](math/avg.md), [div](math/fdiv.md),  [max](math/max.md), [min](math/min.md), [sum](math/sum.md), [xbar](math/xbar.md), [round](math/round.md), [floor](math/floor.md), [ceil](math/ceil.md), [med](math/med.md), [dev](math/dev.md)
  </td>
</tr>

<tr markdown><td markdown>misc</td>
<td markdown>
  [count](misc/count.md), [type](misc/type.md), [rc](misc/rc.md), [gc](misc/gc.md), [meta](misc/meta.md)
</td>
</tr>

<tr markdown><td markdown>order</td>
<td markdown>
  [asc](order/asc.md), [desc](order/desc.md), [iasc](order/iasc.md), [idesc](order/idesc.md), [neg](order/neg.md), [xasc](order/xasc.md), [xdesc](order/xdesc.md)
</td>
</tr>

<tr markdown><td markdown>query</td>
<td markdown>
  [select](query/select.md)
</td>
</tr>

<tr markdown><td markdown>repl</td>
<td markdown>
  [args](repl/args.md), [commands](repl/commands.md), [eva](repl/eval.md), [exec](repl/exec.md), [load](repl/load.md), [parse](repl/parse.md), [quote](repl/quote.md), [resolve](repl/resolve.md),
  [set-display-width](repl/set_display_width.md), [set-fpr](repl/set_fpr.md), [use-unicode-format](repl/use_unicode_format.md)
</td>
</tr>

<tr markdown><td markdown>serde</td>
<td markdown>
  [ser](serde/ser.md), [de](serde/de.md)
</td>
</tr>

<tr markdown><td markdown>temporal</td>
<td markdown>
  [date](temporal/date.md), [time](temporal/time.md), [timestamp](temporal/timestamp.md)
</td>
</tr>

<tr markdown><td markdown>time</td>
<td markdown>
  [timer](time/timer.md), [timeit](time/timeit.md)
</td>
</tr>

<tr markdown><td markdown>update</td>
<td markdown>
  [alter](update/alter.md), [insert](update/insert.md), [upsert](update/upsert.md), [update](update/update.md)
</td>
</tr>

<tr markdown><td markdown>interfacing</td>
<td markdown>
  [loadfn](interfacing/loadfn.md)
</td>
</tr>

</tbody></table>

## Datatypes

| Id  | Name        | Size | Description                        |
| --- | ----------- | ---- | ---------------------------------- |
| 0   | `List`      | -    | Generic List                       |
| 1   | `B8`        | 1    | Boolean                            |
| 2   | `U8`        | 1    | Byte                               |
| 3   | `I16`       | 2    | Signed Short                       |
| 4   | `I32`       | 4    | Signed 32-bit Integer              |
| 5   | `I64`       | 8    | Signed 64-bit Integer              |
| 6   | `Symbol`    | 8    | Symbol (interned string)           |
| 7   | `Date`      | 4    | Date                               |
| 8   | `Time`      | 4    | Time                               |
| 9   | `Timestamp` | 8    | Timestamp                          |
| 10  | `F64`       | 8    | 64-bit Floating Point              |
| 11  | `Guid`      | 16   | Globally Unique Identifier         |
| 12  | `C8`        | 8    | Char                               |
| 20  | `Enum`      | -    | Enumerated Type                    |
| 98  | `Table`     | -    | Table                              |
| 99  | `Dict`      | -    | Dictionary                         |
| 100 | `Lambda`    | -    | Lambda (user function)             |
| 101 | `Unary`     | -    | Unary (function with 1 argument)   |
| 102 | `Binary`    | -    | Binary (function with 2 arguments) |
| 103 | `Vary`      | -    | Vary (function with n arguments)   |
| 126 | `Null`      | -    | Generic NULL                       |
| 127 | `Error`     | -    | Error (special type for errors)    |
