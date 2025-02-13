# :material-typewriter: Syntax Overview

## Atoms

```clj title="basic types"
;; Boolean
true
false

;; Character
'a'

;; Integer (i64)
42
-17

;; Float (f64)
42.1
42.0
-3.14

;; Symbol
name        ;; Bare symbol
'name       ;; Quoted symbol
```

```clj title="temporal types"
;; Date (days since epoch)
2024.03.15

;; Time (ms since midnight)
10:30:00

;; Timestamp (date + time)
2024.03.15T10:30:00
```

## Compound Types

```clj title="vectors"
;; Integer vector
[1 2 3 4]

;; Float vector
[1.0 2.5 3.3 4.4]

;; Symbol vector (no quotes needed)
[name age score]

;; Date vector
[2024.03.15 2024.03.16 2024.03.17]

;; Time vector
[10:30:00 11:45:00 12:15:00]
```

```clj title="strings and lists"
;; String (character vector)
"Hello, world!"

;; List of strings (must use list for strings)
(list "Alice" "Bob" "Charlie")

;; Heterogeneous list
(list 1 "two" [3 4 5] {x: 6})
```

```clj title="dictionaries"
;; Dictionary with symbol keys
{name: "Alice" 
 age: 30 
 scores: [85 90 95]}

;; Nested dictionaries
{user: {id: 1 name: "Alice"}
 data: {x: 10 y: 20}}
```

```clj title="tables"
;; Table creation
(table [name age score]           ;; Column names as symbols
       (list (list "Alice" "Bob") ;; String column as list
             [25 30]              ;; Number column as vector
             [85 90]))            ;; Another number column

;; Display
┌───────┬─────┬───────┐
│ name  │ age │ score │
├───────┼─────┼───────┤
│ Alice │ 25  │ 85    │
│ Bob   │ 30  │ 90    │
└───────┴─────┴───────┘
```

## Special Types

```clj title="guid"
;; Generate new GUIDs
↪ (guid 1)
[550e8400-e29b-41d4-a716-446655440000]

;; Parse GUID from string
↪ (as 'guid "550e8400-e29b-41d4-a716-446655440000")
550e8400-e29b-41d4-a716-446655440000
```

## Functions

```clj title="functions"
;; Function definition
↪ (set add (fn [x y] (+ x y)))

;; Lambda with multiple expressions
↪ (set process (fn [x] 
                 (set tmp (* x 2))
                 (+ tmp 10)))

;; Immediate lambda execution
↪ ((fn [x] (* x 2)) 21)
42
```

## Expressions

Rayfall uses prefix notation where the function comes before its arguments:

```clj
;; Basic arithmetic
↪ (+ 1 2)     ;; Binary function
3
↪ (- 10 5)
5

;; Function composition
↪ (+ (* 2 3) (/ 10 2))
11

;; Aggregations
↪ (avg [1 2 3 4])  ;; Unary function
2.5

;; Multiple arguments
↪ (list 1 2 3)     ;; Vary function
[1 2 3]
```

## Comments

```clj
;; Single line comment

;; Multi-line comment
;; continues here
;; and here
```

## Special Forms

```clj
;; Quote - prevents evaluation
↪ (quote x)
'x

;; Set - assigns value
↪ (set x 42)
42

;; Try - error handling
↪ (try 
     (dangerous-operation)
     {e: (println "Error:" e)})
```
