# :material-remote: IPC

!!! warning
    For now it is unsatable and possible subject of changes in the future!

A RayforceDB can communicate with other RayforceDBs via IPC. It is a very fast and efficient way of communication. It is used to send data between RayforceDBs, to send data to the client, and to send data to the server.

## :material-phone-log-outline: Listen to a port

To start a Rayforce process that listens to a port, use the `-p` flag:

``` bash
> rayforce -p 5110
```

## :material-connection: Connect to a remote process

To connect to a port call `hopen` function:

``` clj
(set h (hopen "127.0.0.1:5110"))
```

Now, you can send data to the remote process:

``` clj
↪ (write h "(+ 1 2)")
3
↪ (write h (list (+ 1 2)))
3
```

## :material-message: Message format

There are two ways of sending ipc messages: string or list. In case of string it will be parsed an evaluated then, in case of list it will be evaluated as is.

!!! note
    Do not forget to call hclose for any unused connection handle!

``` clj
(hclose h)
```

There are 3 types of messages:
- Sync (request)
- Response
- Async

## :material-sync: Sync

Sync messages are used to send a request and get a response. Sync messages are blocking, so the sender will wait for the response.

``` clj
↪ (write h "(+ 1 2)")
3
```

## :material-responsive: Response

Response messages are used to send a response to a sync message. Response messages are implicitly sent by the receiver of a sync message.

## :material-sync-off: Async

Async messages are used to send a message without waiting for a response. Async messages are not blocking, so the sender will not wait for the response. To send an async message, use negate for a connection handle with a `write` function:

``` clj
↪ (write (neg h) (list (+ 1 2)))
```

## :material-protocol: Protocol

The protocol is very simple. One just utilizes serialization to send/receive messages. Just one addition is: handshake. It is used to negotiate the protocol version and to send the credentials (if any).

## :material-handshake: Handshake

After a client has opened a connection to a server, the first message sent by the client is a handshake message. It is a simple null-terminated ASCII string followed by a version number byte of next format: ["username:password"]v

Version is encoded as follows:

``` c
(RAYFORCE_MAJOR_VERSION << 3 | RAYFORCE_MINOR_VERSION)
```

### Handshake Process

1. Client connects to the server
2. Client sends handshake message (null-terminated ASCII string followed by a version number byte):
   - Format: `[username:password]v`
   - `username:password` is optional, can be empty `[]`
   - `v` is a single byte version number
3. Server validates the handshake:
   - Checks version compatibility
   - Validates credentials if provided
4. Server responds with:
   - Success: `\x01` (1 byte)
   - Failure: `\x00` (1 byte) followed by error message

Example handshake messages:
```
[]v                    # No credentials, just version
[admin:secret123]v     # With credentials
```

## :material-table: Data Type Serialization

Each IPC message consists of a header followed by the serialized data. The header format is:
```
prefix(1) version(1) flags(1) endian(1) msgtype(1) size(8)
```

The following table describes the serialization format for each data type:

| Type Name | Type ID | Size (bytes) | Literal                                | Null      |
| --------- | ------- | ------------ | -------------------------------------- | --------- |
| List      | 0       |              | `(1 2)`                                | `()`      |
| Bool      | 1       | 2            | `true`                                 | `false`   |
| U8        | 2       | 2            | `255x`                                 | `0x`      |
| I16       | 3       | 3            | `1h`                                   | `0Nh`     |
| I32       | 4       | 5            | `1i`                                   | `0Ni`     |
| I64       | 5       | 9            | `1`                                    | `0Nl`     |
| Symbol    | 6       |              | `'foo`                                 | `''`      |
| Date      | 7       | 4            | `2024.01.01`                           | `0Nd`     |
| Time      | 8       | 4            | `00:00:01`                             | `0Nt`     |
| Timestamp | 9       | 8            | `2024.01.01D00:00:01.000000001`        | `0Np`     |
| F64       | 10      | 8            | `1.0`                                  | `0Nf`     |
| GUID      | 11      | 16           | `01020304-0506-0708-090A-0B0C0D0E0F10` | `0Ng`     |
| C8        | 12      |              | `'A'`    "AAAA"                        | `'\0'` "" |
| Dict      | 99      |              | `{a: 1 b: 2 c: 3}`                     | `{}`      |
| Null      | 126     |              | null                                   | `null`    |

### Serialization Notes

- All numeric values are in network byte order (big-endian)
- Header fields:
  - `prefix`: 0xcefadefa
  - `version`: Protocol version
  - `flags`: Message flags: 0 - no flags
  - `endian`: Endianness indicator: 0 - little, 1 - big
  - `msgtype`: Message type: 0 - sync, 1 - response, 2 - async
  - `size`: Total message size
- String and symbol data is UTF-8 encoded
- Error messages include the error code and description
