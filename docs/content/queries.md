# :material-airplane-search: Queries

RayforceDB has an simple but powerful query language. It has dictionary notation which is very similar to JSON and it is easy to use.

## Query format

``` clj
(select
    {
        ;; predicates
        from: <expr>
        [where: <expr>]
        [by: <expr>]
        ;; fields
        [field1: <expr>]
        [field2: <expr>]
        ...
    }
)
```

## Example

``` clj
(select
    {
        from: orders
        where: (= 0 (% Size 2))
        by: Symbol
        id: OrderId
        price: (avg Price)
        tape: (first Tape)
    }
)
```
