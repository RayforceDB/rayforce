# IPC

!!! warning
    For now it is unstable and possible subject of changes in the future!

A RayforceDB can communicate with other RayforceDBs via IPC. It is a very fast and efficient way of communication. It is used to send data between RayforceDBs, to send data to the client, and to send data to the server.

## Listen to a port

To start a Rayforce process that listens to a port, use the `-p` flag:

``` sh
> rayforce -p 5110
```

## Connect to a remote process

To connect to a port call `hopen` function:

``` clj
(set h (hopen "127.0.0.1:5110"))
```

Now, you can send data to the remote process:

``` clj
> (write h "(+ 1 2)")
3
> (write h (list (+ 1 2)))
3
```

## Message format

There are two ways of sending ipc messages: string or list. In case of string it will be parsed an evaluated then, in case of list it will be evaluated as is.

!!! tip
    Do not forget to call hclose for any unused connection handles!

``` clj
(hclose h)
```

There are 3 types of messages:

- Sync (request)
- Response
- Async

## Sync

Sync messages are used to send a request and get a response. Sync messages are blocking, so the sender will wait for the response.

``` clj
> (write h "(+ 1 2)")
3
```

## Response

Response messages are used to send a response to a sync message. Response messages are implicitly sent by the receiver of a sync message.

## Async

Async messages are used to send a message without waiting for a response. Async messages are not blocking, so the sender will not wait for the response. To send an async message, use negate for a connection handle with a `write` function:

``` clj
> (write -h (list (+ 1 2)))
```

## Protocol

The protocol is very simple. One just utilizes serialization to send/receive messages. Just one addition is: handshake. It is used to negotiate the protocol version and to send the credentials (if any).

## Handshake

After a client has opened a connection to a server, the first message sent by the client is a handshake message. It is a simple null-terminated ASCII string followed by a version number byte of next format: ["username:password"]v

Version is encoded as follows:

``` c
(RAYFORCE_MAJOR_VERSION << 3 | RAYFORCE_MINOR_VERSION)
```
