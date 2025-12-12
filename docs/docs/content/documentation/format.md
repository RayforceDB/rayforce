# :material-format-text: Format Functions

RayforceDB provides functions for converting objects to string representations and printing formatted output. These functions enable data display, debugging, and user interaction.

## String Formatting

### :material-code-tags: Format

Converts objects to string representations. When given a single argument, formats that object. When given multiple arguments, treats the first as a format string with `%` placeholders that are replaced by subsequent arguments.

```clj
(format 1)
"1"

(format "Total: % Quantity: %" 15025.00 100)
"Total: 15025.00 Quantity: 100"
```

## Output Functions

### :material-printer: Print / Println

Prints formatted output to stdout without a trailing newline.

```clj
(print "% %" 1 [2 3 4])
1 [2 3 4]

(print "Hello")
Hello


(println "% %" 1 [2 3 4])
1 [2 3 4]

(println "Hello")
Hello

;; Print formatted message with newline
(println "Price: % Symbol: %" 150.25 'AAPL)
Price: 150.25 Symbol: AAPL
```

### :material-eye: Show

Prints the full representation of an object to stdout without terminal width constraints. Displays the complete object structure regardless of display width settings.

```clj
(show (til 100))
[0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 71 72 73 74 75 76 77 78 79 80 81 82 83 84 85 86 87 88 89 90 91 92 93 94 95 96 97 98 99]

(set prices [150.25 300.50 125.75 200.00 175.50])
(show prices)
[150.25 300.50 125.75 200.00 175.50]
```
