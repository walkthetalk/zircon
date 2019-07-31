// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>
#include <lib/zxio/inception.h>
#include <lib/zxio/zxio.h>

#include <unittest/unittest.h>

#define ALPHABET "abcdefghijklmnopqrstuvwxyz"

bool vmofile_basic_test(void) {
    BEGIN_TEST;

    zx::vmo backing;
    ASSERT_EQ(ZX_OK, zx::vmo::create(300u, 0u, &backing));
    size_t len = strlen(ALPHABET);
    ASSERT_EQ(ZX_OK, backing.write(ALPHABET, 0, len));
    ASSERT_EQ(ZX_OK, backing.write(ALPHABET, len, len + len));

    zx::channel h1, h2;
    ASSERT_EQ(ZX_OK, zx::channel::create(0u, &h1, &h2));

    zxio_storage_t storage;
    zxio_vmofile_init(&storage, h1.release(), backing.release(), 4, len, 3);
    zxio_t* io = &storage.io;

    zxio_signals_t observed = ZXIO_SIGNAL_NONE;
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_wait_one(io, ZXIO_READABLE,
                                                  ZX_TIME_INFINITE, &observed));

    zx::channel clone;
    ASSERT_EQ(ZX_OK, zxio_clone(io, clone.reset_and_get_address()));
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_sync(io));

    zxio_node_attr_t attr = {};
    ASSERT_EQ(ZX_OK, zxio_attr_get(io, &attr));
    EXPECT_EQ(len, attr.content_size);
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_attr_set(io, 0u, &attr));
    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    size_t actual = 0u;
    ASSERT_EQ(ZX_OK, zxio_read(io, buffer, 8, &actual));
    EXPECT_EQ(actual, 8);
    EXPECT_STR_EQ("hijklmno", buffer);
    memset(buffer, 0, sizeof(buffer));
    actual = 0u;
    ASSERT_EQ(ZX_OK, zxio_read_at(io, 1u, buffer, 6, &actual));
    EXPECT_EQ(actual, 6);
    EXPECT_STR_EQ("fghijk", buffer);
    ASSERT_EQ(ZX_ERR_WRONG_TYPE, zxio_write(io, buffer, sizeof(buffer), &actual));
    ASSERT_EQ(ZX_ERR_WRONG_TYPE, zxio_write_at(io, 0u, buffer, sizeof(buffer), &actual));
    size_t offset = 2u;
    ASSERT_EQ(ZX_OK, zxio_seek(io, 2u, ::llcpp::fuchsia::io::SeekOrigin::START, &offset));
    EXPECT_EQ(offset, 2u);
    memset(buffer, 0, sizeof(buffer));
    actual = 0u;
    ASSERT_EQ(ZX_OK, zxio_read(io, buffer, 3, &actual));
    EXPECT_STR_EQ("ghi", buffer);
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_truncate(io, 0u));
    uint32_t flags = 0u;
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_flags_get(io, &flags));
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_flags_set(io, flags));
    zx_handle_t vmo = ZX_HANDLE_INVALID;
    size_t size = 0u;
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_vmo_get_copy(io, &vmo, &size));
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_vmo_get_clone(io, &vmo, &size));
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_vmo_get_exact(io, &vmo, &size));

    zxio_t* result = nullptr;
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_open(io, 0u, 0u, "hello", &result));
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED,
              zxio_open_async(io, 0u, 0u, "hello", ZX_HANDLE_INVALID));
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zxio_unlink(io, "hello"));

    ASSERT_EQ(ZX_OK, zxio_close(io));

    END_TEST;
}

BEGIN_TEST_CASE(vmofile_test)
RUN_TEST(vmofile_basic_test);
END_TEST_CASE(vmofile_test)
