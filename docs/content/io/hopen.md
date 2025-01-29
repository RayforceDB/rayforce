# Hopen `hopen`

Open a IO handle to a remote process or to a journal log.

## IPC: 

```clj
↪ (hopen h "127.0.0.1:5100")
(write h "Hello, world!")
```

## Journal: 

```clj
↪ (hopen h "/tmp/log")
(write h "Hello, world!")
```