// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_FDIO_PRIVATE_H_
#define ZIRCON_SYSTEM_ULIB_FDIO_PRIVATE_H_

#include <errno.h>
#include <fuchsia/io/llcpp/fidl.h>
#include <lib/fdio/limits.h>
#include <lib/fdio/vfs.h>
#include <lib/zxio/ops.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <threads.h>
#include <zircon/types.h>

typedef struct fdio fdio_t;
typedef struct fdio_namespace fdio_ns_t;

// FDIO provides open/close/read/write io over various transports
// via the fdio_t interface abstraction.
//
// The PIPE protocol uses message ports as simple, no-flow-control
// io pipes with a maximum message size of ZX_PIPE_SIZE.
//
// The REMOTEIO protocol uses message ports to implement simple
// synchronous remoting of read/write/close operations.
//
// The NULL protocol absorbs writes and is never readable.

typedef zx_status_t (*two_path_op)(fdio_t* io, const char* src, size_t srclen,
                                   zx_handle_t dst_token, const char* dst, size_t dstlen);

typedef struct fdio_ops {
    zx_status_t (*close)(fdio_t* io);
    zx_status_t (*open)(fdio_t* io, const char* path, uint32_t flags,
                        uint32_t mode, fdio_t** out);
    zx_status_t (*clone)(fdio_t* io, zx_handle_t* out_handle);
    zx_status_t (*unwrap)(fdio_t* io, zx_handle_t* out_handle);
    void (*wait_begin)(fdio_t* io, uint32_t events, zx_handle_t* handle,
                       zx_signals_t* signals);
    void (*wait_end)(fdio_t* io, zx_signals_t signals, uint32_t* events);
    ssize_t (*ioctl)(fdio_t* io, uint32_t op, const void* in_buf, size_t in_len,
                     void* out_buf, size_t out_len);
    zx_status_t (*posix_ioctl)(fdio_t* io, int req, va_list va);
    zx_status_t (*get_vmo)(fdio_t* io, int flags, zx_handle_t* out);
    zx_status_t (*get_token)(fdio_t* io, zx_handle_t* out);
    zx_status_t (*get_attr)(fdio_t* io, llcpp::fuchsia::io::NodeAttributes* out);
    zx_status_t (*set_attr)(fdio_t* io, uint32_t flags,
                            const llcpp::fuchsia::io::NodeAttributes* attr);
    zx_status_t (*readdir)(fdio_t* io, void* ptr, size_t max, size_t* actual);
    zx_status_t (*rewind)(fdio_t* io);
    zx_status_t (*unlink)(fdio_t* io, const char* path, size_t len);
    zx_status_t (*truncate)(fdio_t* io, off_t off);
    two_path_op rename;
    two_path_op link;
    zx_status_t (*get_flags)(fdio_t* io, uint32_t* out_flags);
    zx_status_t (*set_flags)(fdio_t* io, uint32_t flags);
    ssize_t (*recvfrom)(fdio_t* io, void* data, size_t len, int flags,
                        struct sockaddr* __restrict addr, socklen_t* __restrict addrlen);
    ssize_t (*sendto)(fdio_t* io, const void* data, size_t len, int flags,
                      const struct sockaddr* addr, socklen_t addrlen);
    ssize_t (*recvmsg)(fdio_t* io, struct msghdr* msg, int flags);
    ssize_t (*sendmsg)(fdio_t* io, const struct msghdr* msg, int flags);
    zx_status_t (*shutdown)(fdio_t* io, int how);
    zx_duration_t (*get_rcvtimeo)(fdio_t* io);
} fdio_ops_t;

// fdio_t ioflag values
#define IOFLAG_CLOEXEC (1 << 0)
#define IOFLAG_EPOLL (1 << 2)
#define IOFLAG_WAITABLE (1 << 3)
#define IOFLAG_SOCKET_CONNECTING (1 << 4)
#define IOFLAG_SOCKET_CONNECTED (1 << 5)
#define IOFLAG_NONBLOCK (1 << 6)

// The subset of fdio_t per-fd flags queryable via fcntl.
// Static assertions in unistd.c ensure we aren't colliding.
#define IOFLAG_FD_FLAGS IOFLAG_CLOEXEC

typedef struct fdio fdio_t;

__BEGIN_CDECLS

// Acquire a reference to a globally shared "fdio_t" object
// acts as a sentinel value for reservation.
//
// It is unsafe to call any ops within this object.
// It is unsafe to change the reference count of this object.
fdio_t* fdio_get_reserved_io(void);

// Access the |zxio_t| field within an |fdio_t|.
zxio_t* fdio_get_zxio(fdio_t* io);

// Initialize an |fdio_t| object with the provided ops table.
//
// Initializes the refcount to one. The refcount may be altered with the |fdio_acquire| and
// |fdio_release| functions. When the refcount reaches zero, the object is destroyed.
fdio_t* fdio_alloc(const fdio_ops_t* ops);

// Increases the refcount of |io| by one.
void fdio_acquire(fdio_t* io);

// Decreases the refcount of |io| by one. If the reference count
// reaches zero, the object is destroyed.
void fdio_release(fdio_t* io);

// Returns true if |io| is the only acquired reference to an object.
bool fdio_is_last_reference(fdio_t* io);

// Accessors for the internal fields of fdio_t:

const fdio_ops_t* fdio_get_ops(const fdio_t* io);
int32_t fdio_get_dupcount(const fdio_t* io);
void fdio_dupcount_acquire(fdio_t* io);
void fdio_dupcount_release(fdio_t* io);
uint32_t* fdio_get_ioflag(fdio_t* io);
zxio_storage_t* fdio_get_zxio_storage(fdio_t* io);

// Lifecycle notes:
//
// Upon creation, fdio objects have a refcount of 1.
// fdio_acquire() and fdio_release() are used to upref
// and downref, respectively.  Upon downref to 0,
// fdio_free() is called, which poisons the object and
// free()s it.
//
// The close hook must be called before free and should
// only be called once.  In normal use, fdio objects are
// accessed through the fdio_fdtab, and when close is
// called they are removed from the fdtab and the reference
// that the fdtab itself is holding is released, at which
// point they will be free()'d unless somebody is holding
// a ref due to an ongoing io transaction, which will
// certainly fail due to underlying handles being closed
// at which point a downref will happen and destruction
// will follow.
//
// dupcount tracks how many fdtab entries an fdio object
// is in.  close() reduces the dupcount, and only actually
// closes the underlying object when it reaches zero.

zx_status_t fdio_close(fdio_t* io);
zx_status_t fdio_wait(fdio_t* io, uint32_t events, zx_time_t deadline,
                      uint32_t* out_pending);

// Wraps a channel with an fdio_t using remote io.
// Takes ownership of h and e.
fdio_t* fdio_remote_create(zx_handle_t h, zx_handle_t event);

// creates a fdio that wraps a log object
// this will allocate a buffer (on demand) to assemble
// entire log-lines and flush them on newline or buffer full.
fdio_t* fdio_logger_create(zx_handle_t);

// Creates an |fdio_t| from a remote directory connection.
//
// Takes ownership of |control|.
fdio_t* fdio_dir_create(zx_handle_t control);

// Creates an |fdio_t| from a remote file connection.
//
// Takes ownership of |control| and |event|.
fdio_t* fdio_file_create(zx_handle_t control, zx_handle_t event);

// Creates a pipe backed by a socket.
//
// Takes ownership of |socket|.
fdio_t* fdio_pipe_create(zx::socket socket);

// Creates a socketpair backed by a socket.
//
// Takes ownership of |socket|.
fdio_t* fdio_socketpair_create(zx::socket socket);

// Creates an |fdio_t| from a VMO.
//
// Takes ownership of |vmo|.
fdio_t* fdio_vmo_create(zx_handle_t vmo, zx_off_t seek);

// Creates an |fdio_t| for a VMO file.
//
// * |control| is an handle to the control channel for the VMO file.
// * |vmo| is the VMO that contains the contents of the file.
// * |offset| is the index of the first byte of the file in the VMO.
// * |length| is the number of bytes in the file.
// * |seek| is the initial seek offset within the file (i.e., relative to
//   |offset| within the underlying VMO).
//
// Always consumes |h| and |vmo|.
fdio_t* fdio_vmofile_create(zx_handle_t control, zx_handle_t vmo,
                            zx_off_t offset, zx_off_t length, zx_off_t seek);

// Wraps a socket with an fdio_t using socket io.
fdio_t* fdio_socket_create(zx::socket socket, zx_info_socket_t info);

// creates a message port and pair of simple io fdio_t's
int fdio_pipe_pair(fdio_t** a, fdio_t** b);

fdio_t* fdio_ns_open_root(fdio_ns_t* ns);

// Open |path| relative to |dir| as an |fdio_t|.
//
// |dir| must be a channel that implements the |fuchsia.io.Directory| protocol.
// The |flags| and |mode| are passed to |fuchsia.io.Directory/Open| as |flags|
// and |mode|, respectively.
//
// If |flags| includes |ZX_FS_FLAG_DESCRIBE|, this function reads the resulting
// |fuchsia.io.Node/OnOpen| event from the newly created channel and creates an
// appropriate |fdio_t| object to interact with the remote object.
//
// Otherwise, this function creates a generic "remote" |fdio_t| object.
zx_status_t fdio_remote_open_at(zx_handle_t dir, const char* path,
                                uint32_t flags, uint32_t mode,
                                fdio_t** out_io);

// io will be consumed by this and must not be shared
void fdio_chdir(fdio_t* io, const char* path);

// Wraps an arbitrary handle with a fdio_t that works with wait hooks.
// Takes ownership of handle unless shared_handle is true.
fdio_t* fdio_waitable_create(zx_handle_t h, zx_signals_t signals_in,
                             zx_signals_t signals_out, bool shared_handle);

// unsupported / do-nothing hooks shared by implementations
zx_status_t fdio_default_get_token(fdio_t* io, zx_handle_t* out);
zx_status_t fdio_default_set_attr(fdio_t* io, uint32_t flags,
                                  const llcpp::fuchsia::io::NodeAttributes* attr);
zx_status_t fdio_default_readdir(fdio_t* io, void* ptr, size_t max, size_t* actual);
zx_status_t fdio_default_rewind(fdio_t* io);
zx_status_t fdio_default_unlink(fdio_t* io, const char* path, size_t len);
zx_status_t fdio_default_truncate(fdio_t* io, off_t off);
zx_status_t fdio_default_rename(fdio_t* io, const char* src, size_t srclen,
                                zx_handle_t dst_token, const char* dst, size_t dstlen);
zx_status_t fdio_default_link(fdio_t* io, const char* src, size_t srclen,
                              zx_handle_t dst_token, const char* dst, size_t dstlen);
zx_status_t fdio_default_get_flags(fdio_t* io, uint32_t* out_flags);
zx_status_t fdio_default_set_flags(fdio_t* io, uint32_t flags);
ssize_t fdio_default_write(fdio_t* io, const void* _data, size_t len);
ssize_t fdio_default_write_at(fdio_t* io, const void* _data, size_t len, off_t offset);
ssize_t fdio_default_recvfrom(fdio_t* io, void* _data, size_t len, int flags,
                              struct sockaddr* __restrict addr,
                              socklen_t* __restrict addrlen);
ssize_t fdio_default_sendto(fdio_t* io, const void* _data, size_t len,
                            int flags, const struct sockaddr* addr,
                            socklen_t addrlen);
ssize_t fdio_default_recvmsg(fdio_t* io, struct msghdr* msg, int flags);
ssize_t fdio_default_sendmsg(fdio_t* io, const struct msghdr* msg, int flags);
zx_status_t fdio_default_get_attr(fdio_t* io, llcpp::fuchsia::io::NodeAttributes* out);
zx_status_t fdio_default_close(fdio_t* io);
zx_status_t fdio_default_open(fdio_t* io, const char* path, uint32_t flags,
                              uint32_t mode, fdio_t** out);
zx_status_t fdio_default_clone(fdio_t* io, zx_handle_t* out_handle);
ssize_t fdio_default_ioctl(fdio_t* io, uint32_t op, const void* in_buf,
                           size_t in_len, void* out_buf, size_t out_len);
void fdio_default_wait_begin(fdio_t* io, uint32_t events, zx_handle_t* handle,
                             zx_signals_t* _signals);
void fdio_default_wait_end(fdio_t* io, zx_signals_t signals, uint32_t* _events);
zx_status_t fdio_default_unwrap(fdio_t* io, zx_handle_t* out_handle);
zx_status_t fdio_default_shutdown(fdio_t* io, int how);
zx_duration_t fdio_default_get_rcvtimeo(fdio_t* io);
zx_status_t fdio_default_posix_ioctl(fdio_t* io, int req, va_list va);
zx_status_t fdio_default_get_vmo(fdio_t* io, int flags, zx_handle_t* out);

typedef struct {
    mtx_t lock;
    mtx_t cwd_lock;
    mode_t umask;
    fdio_t* root;
    fdio_t* cwd;
    // fdtab contains either NULL, or a reference to fdio_reserved_io, or a
    // valid fdio_t pointer. fdio_reserved_io must never be returned for
    // operations.
    fdio_t* fdtab[FDIO_MAX_FD];
    fdio_ns_t* ns;
    char cwd_path[PATH_MAX];
} fdio_state_t;

extern fdio_state_t __fdio_global_state;

#define fdio_lock (__fdio_global_state.lock)
#define fdio_root_handle (__fdio_global_state.root)
#define fdio_cwd_handle (__fdio_global_state.cwd)
#define fdio_cwd_lock (__fdio_global_state.cwd_lock)
#define fdio_cwd_path (__fdio_global_state.cwd_path)
#define fdio_fdtab (__fdio_global_state.fdtab)
#define fdio_root_ns (__fdio_global_state.ns)

// Returns an fd number greater than or equal to |starting_fd|, following the
// same rules as fdio_bind_fd. If there are no free file descriptors, -1 is
// returned and |errno| is set to EMFILE. The returned |fd| is bound to
// fdio_reserved_io that has no ops table, and must not be consumed outside of
// fdio, nor allowed to be used for operations.
int fdio_reserve_fd(int starting_fd);

// Assign the given |io| to the reserved |fd|. If |fd| is not reserved, then -1
// is returned and errno is set to EINVAL.
int fdio_assign_reserved(int fd, fdio_t* io);

// Unassign the reservation at |fd|. If |fd| does not resolve to a reservation
// then -1 is returned and errno is set to EINVAL, otherwise |fd| is returned.
int fdio_release_reserved(int fd);

__END_CDECLS

#endif // ZIRCON_SYSTEM_ULIB_FDIO_PRIVATE_H_
