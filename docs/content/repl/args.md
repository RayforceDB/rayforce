# Args `args`

Returns command line arguments passed to the script.

```clj
;; When script.rf is run as: rayforce script.rf arg1 arg2
↪ (args)
["arg1" "arg2"]

;; Check if arguments were provided
↪ (> (count (args)) 0)
true

;; Access specific argument
↪ (at (args) 0)
"arg1"
```

!!! info "Syntax"
    ```clj
    (args)
    ```

!!! warning
    - Returns list of strings
    - Empty list if no arguments
    - Does not include script name
    - Only available in script mode
