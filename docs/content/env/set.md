# Set `set`

`set` is being used for various purposes in the language. It can be used to define a new variable, assign a value to an existing variable, or to update a variable. Another important usecase of `set` is to save inmemory data onto the disk.

```clj
↪ (set x 123)
123
↪ x
123
↪ (set "/tmp/v.dat" [1 2 3])
"/tmp/v.dat"
↪ (get "/tmp/v.dat")
[1 2 3]
```