# Env `env`

The function to return environment dictionary.

```clj
↪ (set add (fn [a b] (return (+ a b)) 77))
↪ (set a 1)
↪ (set b [1 2 3 4])
↪ (env)
{
  add: @add
  a: 1
  b: [1 2 3 4]
}
```
