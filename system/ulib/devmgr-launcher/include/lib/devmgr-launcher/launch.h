// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <vector>
#include <utility>

#include <fbl/unique_fd.h>
#include <fbl/vector.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>
#include <lib/zx/job.h>

namespace devmgr_launcher {

using GetBootItemFunction = fit::inline_function<
    zx_status_t(uint32_t type, uint32_t extra, zx::vmo* vmo, uint32_t* length)>;

using GetArgumentsFunction = fit::inline_function<
    zx_status_t(zx::vmo* vmo, uint32_t* length)>;

struct Args {
    // A list of absolute paths (in devmgr's view of the filesystem) to search
    // for drivers in.  The search is non-recursive.  If empty, this uses
    // devmgr's default.
    fbl::Vector<const char*> driver_search_paths;
    // A list of absolute paths (in devmgr's view of the filesystem) to load
    // drivers from.  This differs from |driver_search_paths| in that it
    // specifies specific drivers rather than entire directories.
    fbl::Vector<const char*> load_drivers;
    // An absolute path (in devmgr's view of the filesystem) for which driver
    // should be bound to the sys_device (the top-level device for most
    // devices).  If nullptr, this uses devmgr's default.
    const char* sys_device_driver = nullptr;
    // If valid, the FD to give to devmgr as stdin/stdout/stderr.  Otherwise
    // inherits from the caller of Launch().
    fbl::unique_fd stdio;
    // A list of path prefixes and channels to add to the isolated devmgr's namespace. Note that
    // /boot is always forwarded from the parent namespace, and /svc will be forwarded if
    // |use_system_svchost| is true. This argument may be used to allow the isolated devmgr access
    // to drivers from /system/drivers.
    std::vector<std::pair<const char*, zx::channel>> flat_namespace;
    // Select whether to use the system svchost or to launch a new one.
    bool use_system_svchost = false;
    // If true, the block watcher will be disabled and will not start.
    bool disable_block_watcher = false;
    // If true, the netsvc will be disabled and will not start.
    bool disable_netsvc = false;

    // The following arguments are for devmgr_integration_test::IsolatedDevmgr only.
    // TODO(ZX-4590): Clean this up, devmgr-launcher shouldn't define arguments that are consumed by
    // a different library higher up the stack.

    // Function to handle requests for boot items.
    GetBootItemFunction get_boot_item;
    // Function to handle requests for boot arguments.
    GetArgumentsFunction get_arguments;
};

// Launches an isolated devmgr, passing the given |args| to it.
//
// Returns its containing job and a channel to the root of its devfs.
// To destroy the devmgr, issue |devmgr_job->kill()|.
zx_status_t Launch(Args args, zx::job* devmgr_job, zx::channel* devfs_root);

} // namespace devmgr_launcher
