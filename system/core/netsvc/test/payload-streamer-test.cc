// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "payload-streamer.h"

#include <optional>

#include <lib/async-loop/cpp/loop.h>
#include <zxtest/zxtest.h>

class PayloadStreamerTest : public zxtest::Test {
protected:
    PayloadStreamerTest()
        : loop_(&kAsyncLoopConfigAttachToThread) {

    }

    static zx_status_t DefaultCallback(void* buf, size_t offset, size_t size, size_t* actual) {
        *actual = size;
        return ZX_OK;
    }

    void StartStreamer(netsvc::ReadCallback callback = DefaultCallback) {
        zx::channel server, client;
        ASSERT_OK(zx::channel::create(0, &client, &server));

        client_.emplace(std::move(client));
        payload_streamer_.emplace(std::move(server), std::move(callback));
        loop_.StartThread("payload-streamer-test-loop");
    }

    async::Loop loop_;
    std::optional<::llcpp::fuchsia::paver::PayloadStream::SyncClient> client_;
    std::optional<netsvc::PayloadStreamer> payload_streamer_;
};

TEST_F(PayloadStreamerTest, RegisterVmo) {
    StartStreamer();

    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    zx_status_t status;
    ASSERT_OK(client_->RegisterVmo(std::move(vmo), &status));
    ASSERT_OK(status);
}

TEST_F(PayloadStreamerTest, RegisterVmoTwice) {
    StartStreamer();

    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    zx_status_t status;
    ASSERT_OK(client_->RegisterVmo(std::move(vmo), &status));
    ASSERT_OK(status);

    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    ASSERT_OK(client_->RegisterVmo(std::move(vmo), &status));
    ASSERT_OK(status);
}

TEST_F(PayloadStreamerTest, ReadData) {
    StartStreamer();

    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    zx_status_t status;
    ASSERT_OK(client_->RegisterVmo(std::move(vmo), &status));
    ASSERT_OK(status);

    ::llcpp::fuchsia::paver::ReadResult result;
    ASSERT_OK(client_->ReadData(&result));
    ASSERT_TRUE(result.is_info());
    ASSERT_EQ(result.info().offset, 0);
    ASSERT_EQ(result.info().size, ZX_PAGE_SIZE);
}

TEST_F(PayloadStreamerTest, ReadDataWithoutRegisterVmo) {
    StartStreamer();

    ::llcpp::fuchsia::paver::ReadResult result;
    ASSERT_OK(client_->ReadData(&result));
    ASSERT_TRUE(result.is_err());
    ASSERT_NE(result.err(), ZX_OK);
}

TEST_F(PayloadStreamerTest, ReadDataHalfFull) {
    StartStreamer([](void* buf, size_t offset, size_t size, size_t* actual) {
        *actual = size / 2;
        return ZX_OK;
    });

    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    zx_status_t status;
    ASSERT_OK(client_->RegisterVmo(std::move(vmo), &status));
    ASSERT_OK(status);

    ::llcpp::fuchsia::paver::ReadResult result;
    ASSERT_OK(client_->ReadData(&result));
    ASSERT_TRUE(result.is_info());
    ASSERT_EQ(result.info().offset, 0);
    ASSERT_EQ(result.info().size, ZX_PAGE_SIZE / 2);
}

TEST_F(PayloadStreamerTest, ReadEof) {
    StartStreamer([](void* buf, size_t offset, size_t size, size_t* actual) {
        *actual = 0;
        return ZX_OK;
    });

    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    zx_status_t status;
    ASSERT_OK(client_->RegisterVmo(std::move(vmo), &status));
    ASSERT_OK(status);

    ::llcpp::fuchsia::paver::ReadResult result;
    ASSERT_OK(client_->ReadData(&result));
    ASSERT_TRUE(result.is_eof());
}

TEST_F(PayloadStreamerTest, ReadFailure) {
    StartStreamer([](void* buf, size_t offset, size_t size, size_t* actual) {
        return ZX_ERR_INTERNAL;
    });

    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    zx_status_t status;
    ASSERT_OK(client_->RegisterVmo(std::move(vmo), &status));
    ASSERT_OK(status);

    ::llcpp::fuchsia::paver::ReadResult result;
    ASSERT_OK(client_->ReadData(&result));
    ASSERT_TRUE(result.is_err());
    ASSERT_NE(result.err(), ZX_OK);
}
