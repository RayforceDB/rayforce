# :material-card-multiple: Reference card

## Builtin functions

<table class="ray-dense" markdown>
<tbody markdown>
<tr markdown><td markdown>env</td>
<td markdown>
  [set](env/set.md), [let](env/let.md), [env](env/env.md), [memstat](env/memstat.md)
</td>
</tr>
</tr>
<tr markdown><td markdown>control</td>
  <td markdown>
  [if](control/if.md), [try](control/try.md), [raise](control/raise.md), [exit](control/exit.md)
  </td>
</tr>
<tr markdown><td markdown>math</td>
  <td markdown>
    [+](math/add.md), [-](math/sub.md), [*](math/mul.md), [/](math/div.md), [%](math/mod.md), [avg](math/avg.md), [div](math/fdiv.md),  [max](math/max.md), [min](math/min.md), [sum](math/sum.md), [xbar](math/xbar.md), [round](math/round.md), [floor](math/floor.md), [ceil](math/ceil.md), [med](math/med.md), [dev](math/dev.md)
  </td>
</tr>
<tr markdown><td markdown>logic</td>
<td markdown>
  [and](logic/and.md), [or](logic/or.md), [not](logic/not.md), [like](logic/like.md)
</td>
</tr>
<tr markdown><td markdown>items</td>
<td markdown>
  [at](items/at.md), [except](items/except.md), [filter](items/filter.md), [find](items/find.md), [first](items/first.md), [last](items/last.md), [key](items/key.md), [value](items/value.md), [take](items/take.md), [sect](items/sect.md) [union](items/union.md), [where](items/where.md), [in](items/in.md)
</td>
</tr>
<tr markdown><td markdown>order</td>
<td markdown>
  [asc](order/asc.md), [desc](order/desc.md), [iasc](order/iasc.md), [idesc](order/idesc.md), [neg](order/neg.md),
</td>
</tr>
<tr markdown><td markdown>relation</td>
<td markdown>
  [eq](rel/eq.md), [ne](rel/ne.md), [lt](rel/lt.md), [le](rel/le.md), [gt](rel/gt.md), [ge](rel/ge.md)
</td>
</tr>
<tr markdown><td markdown>compose</td>
<td markdown>
  [as](compose/as.md), [concat](compose/concat.md), [dict](compose/dict.md), [table](compose/table.md), [group](compose/group.md), [guid](compose/guid.md), [list](compose/list.md), [enlist](compose/enlist.md), [rand](compose/rand.md), [reverse](compose/reverse.md), [til](compose/til.md), [distinct](compose/distinct.md)
</td>
</tr>
<tr markdown><td markdown>iter</td>
<td markdown>
  [apply](iter/apply.md), [do](iter/do.md), [map](iter/map.md), [fold](iter/fold.md)
</td>
</tr>
<tr markdown><td markdown>items</td>
<td markdown>
  [map](iter/map.md), [fold](iter/fold.md)
</td>
</tr>
<tr markdown><td markdown>join</td>
<td markdown>
  [lj](join/lj.md)
</td>
</tr>
<tr markdown><td markdown>update</td>
<td markdown>
  [alter](update/alter.md), [insert](update/insert.md), [upsert](update/upsert.md), [update](update/update.md)
</td>
</tr>
<tr markdown><td markdown>repl</td>
<td markdown>
  [parse](repl/parse.md), [eval](repl/eval.md), [exec](repl/exec.md), [load](repl/load.md)
</td>
</tr>
<tr markdown><td markdown>misc</td>
<td markdown>
  [count](misc/count.md), [type](misc/type.md), [rc](misc/rc.md)
</td>
</tr>
<tr markdown><td markdown>time</td>
<td markdown>
  [timer](time/timer.md)
</td>
</tr>
<tr markdown><td markdown>ext</td>
<td markdown>
  [loadfn](ext/loadfn.md)
</td>
</tr>
<tr markdown><td markdown>io</td>
<td markdown>
  [write](io/write.md), [read](io/read.md), [csv](io/csv.md), [print](io/print.md), [println](io/println.md), [hopen](io/hopen.md), [hclose](io/hclose.md)
</td>
</tr>
<tr markdown><td markdown>serde</td>
<td markdown>
  [ser](serde/ser.md), [de](serde/de.md)
</td>
</tr>
</tbody></table>

## Datatypes

| Id   |  Name       | Size  |  Description                        |
| ---  |   ---       |  ---  |      ---                            |
|  0   | `List`      |   -   |  Generic List                       |
|  1   | `Bool`      |   1   |  Boolean                            |
|  2   | `Byte`      |   1   |  Byte                               |
|  3   | `I64`       |   8   |  Signed 64-bit Integer              |
|  4   | `F64`       |   8   |  64-bit Floating Point              |
|  5   | `Symbol`    |   8   |  Symbol (interned string)           |
|  6   | `Timestamp` |   8   |  Timestamp                          |
|  7   | `Guid`      |   16  |  Globally Unique Identifier         |
|  8   | `Char`      |   4   |   Character                         |
|  20  | `Enum`      |   -   |  Enumerated Type                    |
|  77  | `AnyMap`    |   -   |  Generic Map                        |
|  78  | `FilterMap` |   -   |  Filter Map                         |
|  79  | `GroupMap`  |   -   |  Group Map                          |
|  98  | `Table`     |   -   |  Table                              |
|  99  | `Dict`      |   -   |  Dictionary                         |
|  100 | `Lambda`    |   -   |  Lambda (user function)             |
|  101 | `Unary`     |   -   |  Unary (function with 1 argument)   |
|  102 | `Binary`    |   -   |  Binary (function with 2 arguments) |
|  103 | `Vary`      |   -   |  Vary (function with n arguments)   |
|  126 | `Null`      |   -   |  Generic NULL                       |
|  127 | `Error`     |   -   |  Error (special type for errors)    |
