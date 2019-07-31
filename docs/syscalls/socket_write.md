# zx_socket_write

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

Write data to a socket.

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_socket_write(zx_handle_t handle,
                            uint32_t options,
                            const void* buffer,
                            size_t buffer_size,
                            size_t* actual);
```

## DESCRIPTION

`zx_socket_write()` attempts to write *buffer_size* bytes to the socket specified
by *handle*. The pointer to *bytes* may be NULL if *buffer_size* is zero.

If **ZX_SOCKET_CONTROL** is passed to *options*, then `zx_socket_write()`
attempts to write into the socket control plane. A write to the control plane is
never short. If the socket control plane has insufficient space for *buffer*, it
writes nothing and returns **ZX_ERR_OUT_OF_RANGE**. Only a single control plane
message can be active in a socket direction.

If a NULL *actual* is passed in, it will be ignored.

A **ZX_SOCKET_STREAM** socket write can be short if the socket does not have
enough space for all of *buffer*. If a non-zero amount of data was written to
the socket, the amount written is returned via *actual* and the call succeeds.
Otherwise, if the socket was already full, the call returns
**ZX_ERR_SHOULD_WAIT** and the client should wait (e.g., with
[`zx_object_wait_one()`] or [`zx_object_wait_async()`]).


A **ZX_SOCKET_DATAGRAM** socket write is never short. If the socket has
insufficient space for *buffer*, it writes nothing and returns
**ZX_ERR_SHOULD_WAIT**. If the write succeeds, *buffer_size* is returned via
*actual*. Attempting to write a packet larger than the datagram socket's
capacity will fail with **ZX_ERR_OUT_OF_RANGE**.

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

*handle* must be of type **ZX_OBJ_TYPE_SOCKET** and have **ZX_RIGHT_WRITE**.

## RETURN VALUE

`zx_socket_write()` returns **ZX_OK** on success.

## ERRORS

**ZX_ERR_BAD_HANDLE**  *handle* is not a valid handle.

**ZX_ERR_BAD_STATE**  Either:
1. *options* includes **ZX_SOCKET_CONTROL** and the socket was not created
   with **ZX_SOCKET_HAS_CONTROL**, or
2. writing has been disabled for this socket endpoint via [`zx_socket_shutdown()`].

**ZX_ERR_WRONG_TYPE**  *handle* is not a socket handle.

**ZX_ERR_INVALID_ARGS**  *buffer* is an invalid pointer.

**ZX_ERR_ACCESS_DENIED**  *handle* does not have **ZX_RIGHT_WRITE**.

**ZX_ERR_SHOULD_WAIT**  The buffer underlying the socket is full. For the
control plane, a previous control message is still in the socket.

**ZX_ERR_OUT_OF_RANGE**  The socket was created with **ZX_SOCKET_DATAGRAM** and
*buffer* is larger than the remaining space in the socket.

**ZX_ERR_PEER_CLOSED**  The other side of the socket is closed.

**ZX_ERR_NO_MEMORY**  Failure due to lack of memory.
There is no good way for userspace to handle this (unlikely) error.
In a future build this error will no longer occur.

## SEE ALSO

 - [`zx_socket_accept()`]
 - [`zx_socket_create()`]
 - [`zx_socket_read()`]
 - [`zx_socket_share()`]
 - [`zx_socket_shutdown()`]

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_object_wait_async()`]: object_wait_async.md
[`zx_object_wait_one()`]: object_wait_one.md
[`zx_socket_accept()`]: socket_accept.md
[`zx_socket_create()`]: socket_create.md
[`zx_socket_read()`]: socket_read.md
[`zx_socket_share()`]: socket_share.md
[`zx_socket_shutdown()`]: socket_shutdown.md
