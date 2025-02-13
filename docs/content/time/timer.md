# Timer `timer`

Creates and manages recurring timers.

```clj
;; Create a timer that runs every 1000ms (1 second)
↪ (timer 1000 0 {t: (println "Timer tick:" t)})
0  ;; Returns timer ID

;; Create a timer that runs 5 times
↪ (timer 1000 5 {t: (println "Count:" t)})
1  ;; Returns different timer ID

;; Remove a timer by ID
↪ (timer 0)
null

;; Create infinite timer (runs until removed)
↪ (timer 5000 0 {t: (do
                     (println "Background task at:" t)
                     (gc))})  ;; Run garbage collection every 5 seconds
2
```

!!! info "Syntax"
    ```clj
    ;; Create timer
    (timer interval times callback)
    ;; Remove timer
    (timer id)
    ```
    - `interval`: Time between executions in milliseconds
    - `times`: Number of executions (0 for infinite)
    - `callback`: Lambda to execute (receives current time as argument)
    - `id`: Timer ID to remove

!!! warning
    - Minimum interval is system-dependent
    - Callback execution time affects next interval
    - Timer continues until removed or count reached
    - Timers are cleared when process exits
