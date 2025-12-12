# :material-remote: IPC

RayforceDB can communicate with other RayforceDB instances via IPC (Inter-Process Communication). This provides a fast and efficient way to send data between processes, enabling distributed computing and client-server architectures.

!!! warning ""
    IPC functionality is currently unstable and may be subject to changes in the future.

## Setup

### Starting a Server

To start a RayforceDB process that listens on a port, use the `-p` flag:

```bash
rayforce -p 5110
```

The process will listen for incoming connections on the specified port.

## Connection Management

### :material-link: Hopen

Opens a handle to a file or IPC connection.

```clj
;; Connect to remote RayforceDB instance
(set h (hopen "127.0.0.1:5100"))
(set h (hopen "localhost:5110"))

;; Open file handle
(set h (hopen "/tmp/log"))

;; Connect with timeout (milliseconds)
(set h (hopen "127.0.0.1:5100" 5000))
```

For IPC connections, provide a `hostname:port` string (supports both hostnames and IP addresses). For files, provide a file path. Optionally accepts a timeout value (in milliseconds) for IPC connections.

Once connected, you can send data to the remote process using `write`:

```clj
(write h "(+ 1 2)")
3

(write h (list (+ 1 2)))
3
```

### :material-link-off: Hclose

Closes a handle opened with `hopen`.

```clj
(hclose h)
```

## Sending Messages

### Message Format

There are two ways of sending IPC messages:

1. **String**: The string is parsed and evaluated on the remote side
2. **List**: The list is evaluated as-is on the remote side

```clj
(write h "(+ 1 2)")        ;; String message
3

(write h (list + 1 2))     ;; List message
3
```

### Message Types

#### :material-sync: Sync Messages

Sync messages are used to send a request and get a response. Sync messages are blocking - the sender waits for the response.

```clj
(write h "(+ 1 2)")
3

(write h "(sum [1 2 3 4 5])")
15
```

!!! note ""
    Sync messages block until a response is received. Use for operations where you need the result immediately.

#### :material-sync-off: Async Messages

Async messages are sent without waiting for a response. To send an async message, negate the connection handle:

```clj
(write (neg h) (list (+ 1 2)))

(write (neg h) "(println \"Processing...\")")
```

!!! note ""
    Async messages are non-blocking, allowing the sender to continue immediately. Use for fire-and-forget operations.

## Accessing Server Variables

When referring to variables that exist only on the server runtime, ensure they are not evaluated on the client side.

For example, if a table exists only on the server and is named `employees`:

```clj
(write h employees)  ;; Error!
```

Instead, pass the variable name as a quoted symbol so it can be resolved on the server:

```clj
(write h 'employees)  ;; Correct - symbol is sent as-is and evaluated server-side
```

!!! warning "Variable Resolution"
    Always use quoted symbols when referencing server-side variables. Unquoted symbols are resolved on the client side, which will cause errors if the variable doesn't exist locally.

## Protocol Details

The IPC protocol uses serialization to send and receive messages. The protocol includes a handshake phase to negotiate version and credentials.

### Handshake

After a client opens a connection to a server, the first message sent by the client is a handshake message. It consists of a null-terminated ASCII string followed by a version number byte in the format: `[username:password]v`

!!! note ""
    `username:password` is optional and can be empty `[]`. `v` is a single byte version number: `(RAYFORCE_MAJOR_VERSION << 3 | RAYFORCE_MINOR_VERSION)`

!!! note ""
    Server response - **Success**: `\x01` (1 byte). **Failure**: `\x00` (1 byte) followed by an error message.

### Message Header

Each IPC message consists of a header followed by serialized data. The header format is:

```
prefix(4) version(1) flags(1) endian(1) msgtype(1) size(8)
```

**Header Fields:**

| Field | Size | Description |
|-------|------|-------------|
| `prefix` | 4 bytes | Magic number `0xcefadefa` |
| `version` | 1 byte | Protocol version |
| `flags` | 1 byte | Message flags (0 = no flags) |
| `endian` | 1 byte | Endianness indicator (0 = little, 1 = big) |
| `msgtype` | 1 byte | Message type (0 = sync, 1 = response, 2 = async) |
| `size` | 8 bytes | Total message size in bytes |
