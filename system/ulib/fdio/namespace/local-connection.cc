// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>

#include <atomic>
#include <new>

#include <fbl/ref_ptr.h>
#include <lib/fdio/namespace.h>
#include <lib/zxio/null.h>
#include <zircon/types.h>
#include <zircon/device/vfs.h>

#include "../private.h"
#include "local-filesystem.h"
#include "local-vnode.h"

namespace fdio_internal {
namespace {

namespace fio = ::llcpp::fuchsia::io;

// The directory represents a local directory (either / or
// some directory between / and a mount point), so it has
// to emulate directory behavior.
struct LocalConnection {
    zxio_t io;

    // Although these are raw pointers for C compatibility, they are
    // actually strong references to both the namespace and vnode object.
    //
    // On close, they must be destroyed.
    const fdio_namespace* fs;
    const LocalVnode* vn;

    // Readdir sequence number.
    std::atomic<int32_t> seq;
};

static_assert(offsetof(LocalConnection, io) == 0,
              "LocalConnection must be castable to zxio_t");

static_assert(sizeof(LocalConnection) <= sizeof(zxio_storage_t),
              "LocalConnection must fit inside zxio_storage_t.");

LocalConnection* fdio_get_zxio_dir(fdio_t* io) {
    return reinterpret_cast<LocalConnection*>(fdio_get_zxio(io));
}

zx_status_t zxio_dir_close(fdio_t* io) {
    LocalConnection* dir = fdio_get_zxio_dir(io);
    // Reclaim a strong reference to |fs| which was leaked during
    // |CreateLocalConnection()|
    __UNUSED auto fs = fbl::internal::MakeRefPtrNoAdopt<const fdio_namespace>(dir->fs);
    __UNUSED auto vn = fbl::internal::MakeRefPtrNoAdopt<const LocalVnode>(dir->vn);
    dir->fs = nullptr;
    dir->vn = nullptr;
    return ZX_OK;
}

// Expects a canonical path (no ..) with no leading
// slash and no trailing slash
zx_status_t zxio_dir_open(fdio_t* io, const char* path, uint32_t flags,
                          uint32_t mode, fdio_t** out) {
    LocalConnection* dir = fdio_get_zxio_dir(io);

    return dir->fs->Open(fbl::WrapRefPtr(dir->vn), path, flags, mode, out);
}

zx_status_t zxio_dir_get_attr(fdio_t* io, fio::NodeAttributes* attr) {
    *attr = {};
    attr->mode = V_TYPE_DIR | V_IRUSR;
    attr->id = fio::INO_UNKNOWN;
    attr->link_count = 1;
    return ZX_OK;
}

zx_status_t zxio_dir_rewind(fdio_t* io) {
    LocalConnection* dir = fdio_get_zxio_dir(io);
    dir->seq.store(0);
    return ZX_OK;
}

zx_status_t zxio_dir_readdir(fdio_t* io, void* ptr, size_t max, size_t* out_actual) {
    LocalConnection* dir = fdio_get_zxio_dir(io);
    int n = dir->seq.fetch_add(1);
    if (n != 0) {
        *out_actual = 0;
        return ZX_OK;
    }
    return dir->fs->Readdir(*dir->vn, ptr, max, out_actual);
}

zx_status_t zxio_dir_unlink(fdio_t* io, const char* path, size_t len) {
    return ZX_ERR_UNAVAILABLE;
}

constexpr fdio_ops_t kLocalConnectionOps = []() {
    fdio_ops_t ops = {};
    ops.get_attr = zxio_dir_get_attr;
    ops.close = zxio_dir_close;
    ops.open = zxio_dir_open;
    ops.clone = fdio_default_clone;
    ops.ioctl = fdio_default_ioctl;
    ops.wait_begin = fdio_default_wait_begin;
    ops.wait_end = fdio_default_wait_end;
    ops.unwrap = fdio_default_unwrap;
    ops.posix_ioctl = fdio_default_posix_ioctl;
    ops.get_vmo = fdio_default_get_vmo;
    ops.get_token = fdio_default_get_token;
    ops.set_attr = fdio_default_set_attr;
    ops.readdir = zxio_dir_readdir;
    ops.rewind = zxio_dir_rewind;
    ops.unlink = zxio_dir_unlink;
    ops.truncate = fdio_default_truncate;
    ops.rename = fdio_default_rename;
    ops.link = fdio_default_link;
    ops.get_flags = fdio_default_get_flags;
    ops.set_flags = fdio_default_set_flags;
    ops.recvfrom = fdio_default_recvfrom;
    ops.sendto = fdio_default_sendto;
    ops.recvmsg = fdio_default_recvmsg;
    ops.sendmsg = fdio_default_sendmsg;
    ops.shutdown = fdio_default_shutdown;
    return ops;
}();

} // namespace

fdio_t* CreateLocalConnection(fbl::RefPtr<const fdio_namespace> fs,
                              fbl::RefPtr<const LocalVnode> vn) {
    fdio_t* io = fdio_alloc(&kLocalConnectionOps);
    if (io == nullptr) {
        return nullptr;
    }
    // Invoke placement new on the new LocalConnection. Since the object is trivially
    // destructible, we can avoid invoking the destructor.
    static_assert(std::is_trivially_destructible<LocalConnection>::value,
                  "LocalConnection must have trivial destructor");
    char* storage = reinterpret_cast<char*>(fdio_get_zxio_dir(io));
    LocalConnection* dir = new (storage) LocalConnection();
    zxio_null_init(&(fdio_get_zxio_storage(io)->io));

    // Leak a strong reference to |this| which will be reclaimed
    // in |zxio_dir_close()|.
    dir->fs = fs.leak_ref();
    dir->vn = vn.leak_ref();
    return io;
}

} // namespace fdio_internal
