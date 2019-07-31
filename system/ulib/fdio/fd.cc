// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fdio/fd.h>

#include <lib/fdio/fdio.h>
#include <lib/fdio/unsafe.h>
#include <zircon/assert.h>

#include "private.h"

__EXPORT
zx_status_t fdio_fd_create(zx_handle_t handle, int* fd_out) {
    fdio_t* io = nullptr;
    zx_status_t status = fdio_create(handle, &io);
    if (status != ZX_OK) {
        return status;
    }
    int fd = fdio_bind_to_fd(io, -1, 0);
    if (fd < 0) {
        fdio_close(io);
        fdio_release(io);
        return ZX_ERR_BAD_STATE;
    }
    *fd_out = fd;
    return ZX_OK;
}

__EXPORT
zx_status_t fdio_cwd_clone(zx_handle_t* out_handle) {
    auto ops = fdio_get_ops(fdio_cwd_handle);
    return ops->clone(fdio_cwd_handle, out_handle);
}

__EXPORT
zx_status_t fdio_fd_clone(int fd, zx_handle_t* out_handle) {
    zx_status_t status;
    fdio_t* io = nullptr;
    if ((io = fdio_unsafe_fd_to_io(fd)) == NULL) {
        return ZX_ERR_INVALID_ARGS;
    }
    // TODO(ZX-973): implement/honor close-on-exec flag
    status = fdio_get_ops(io)->clone(io, out_handle);
    fdio_release(io);
    return status;
}

__EXPORT
zx_status_t fdio_fd_transfer(int fd, zx_handle_t* out_handle) {
    zx_status_t status;
    fdio_t* io = nullptr;
    status = fdio_unbind_from_fd(fd, &io);
    if (status != ZX_OK) {
        return status;
    }
    status = fdio_get_ops(io)->unwrap(io, out_handle);
    fdio_release(io);
    return status;
}
