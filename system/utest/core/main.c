// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <stdio.h>
#include <sys/uio.h>
#include <unistd.h>

#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/log.h>

#include <unittest/unittest.h>

// Explicitly include zxtest c wrappers, and avoid macro collision with
// unitttest. This is safe, because all macros exported by this header
// are internal, except for RUN_ALL_TESTS. The entry point is public though.
// TODO(gevalentino): Once core-tests are fully migrated remove this comment and
// unittest library import.
#include <zxtest/c/zxtest.h>

// output via debuglog syscalls

static zx_handle_t log_handle;

#define LOGBUF_MAX (ZX_LOG_RECORD_MAX - sizeof(zx_log_record_t))

static void log_write(const void* data, size_t len) {
    while (len > 0) {
        size_t xfer = (len > LOGBUF_MAX) ? LOGBUF_MAX : len;
        zx_debuglog_write(log_handle, 0, data, xfer);
        data += xfer;
        len -= xfer;
    }
}

// libc init and io stubs
// The reason these are here is that the "core" tests intentionally do not
// use fdio. See ./README.md.

static zx_handle_t root_resource;

void __libc_extensions_init(uint32_t count,
                            zx_handle_t handle[], uint32_t info[]) {
    for (unsigned n = 0; n < count; n++) {
        if (info[n] == PA_HND(PA_RESOURCE, 0)) {
            root_resource = handle[n];
            handle[n] = 0;
            info[n] = 0;
            break;
        }
    }
    if (root_resource == ZX_HANDLE_INVALID) {
        static const char kStandaloneMsg[] =
            "*** Standalone core-tests must run directly from userboot ***\n";
        zx_debug_write(kStandaloneMsg, sizeof(kStandaloneMsg) - 1);
        __builtin_trap();
    } else {
        if (zx_debuglog_create(root_resource, 0, &log_handle) != ZX_OK) {
            zx_process_exit(-2);
        }
        static const char kStartMsg[] =
            "*** Running standalone Zircon core tests ***\n";
        zx_debuglog_write(log_handle, 0, kStartMsg, sizeof(kStartMsg));
    }
}

zx_handle_t get_root_resource(void) {
    return root_resource;
}

ssize_t write(int fd, const void* data, size_t count) {
    if ((fd == 1) || (fd == 2)) {
        log_write(data, count);
    }
    return count;
}

ssize_t readv(int fd, const struct iovec* iov, int num) {
    return 0;
}

ssize_t writev(int fd, const struct iovec* iov, int num) {
    ssize_t count = 0;
    ssize_t r;
    while (num > 0) {
        if (iov->iov_len != 0) {
            r = write(fd, iov->iov_base, iov->iov_len);
            if (r < 0) {
                return count ? count : r;
            }
            if ((size_t)r < iov->iov_len) {
                return count + r;
            }
            count += r;
        }
        iov++;
        num--;
    }
    return count;
}

off_t lseek(int fd, off_t offset, int whence) {
    errno = ENOSYS;
    return -1;
}

int isatty(int fd) {
    return 1;
}

// TODO(mcgrathr): When unittest is gone, the zxtest library main will work
// fine here and this can be removed.
int main(int argc, char** argv) {
    const bool ut_ok = unittest_run_all_tests(argc, argv);
    int zxtest_return_code = RUN_ALL_TESTS(argc, argv);
    if (ut_ok && zxtest_return_code == 0) {
        // TODO(mcgrathr): The zircon.py recipe embeds this magic string.  When
        // that's no longer used this can be removed.  It's now redundant with
        // the success message from userboot after we return.
        puts("core-tests succeeded RZMm59f7zOSs6aZUIXZR");
    }
    return ut_ok ? zxtest_return_code : EXIT_FAILURE;
}
