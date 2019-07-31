// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fbl/algorithm.h>
#include <lib/fidl/coding.h>
#include <lib/fidl/internal.h>
#include <lib/zx/event.h>
#include <lib/zx/eventpair.h>
#include <limits.h>
#include <unittest/unittest.h>
#include <zircon/syscalls.h>

#include <cstddef>
#include <memory>
#include <new>

#include "fidl/extra_messages.h"
#include "fidl_coded_types.h"
#include "fidl_structs.h"

namespace fidl {
namespace {

// test utility functions

bool IsPeerValid(const zx::unowned_eventpair& handle) {
  zx_signals_t observed_signals = {};
  switch (handle->wait_one(ZX_EVENTPAIR_PEER_CLOSED, zx::deadline_after(zx::msec(1)),
                           &observed_signals)) {
    case ZX_ERR_TIMED_OUT:
      // timeout implies peer-closed was not observed
      return true;
    case ZX_OK:
      return (observed_signals & ZX_EVENTPAIR_PEER_CLOSED) == 0;
    default:
      return false;
  }
}

bool IsPeerValid(zx_handle_t handle) { return IsPeerValid(zx::unowned_eventpair(handle)); }

bool EncodeErrorTest() {
  BEGIN_TEST;

  // If there is only one handle in the message, fidl_encode should not close beyond one handles.
  // Specifically, |event_handle| should remain intact.

  zx_handle_t event_handle;
  ASSERT_EQ(zx_event_create(0, &event_handle), ZX_OK);
  zx_handle_t handles[2] = {
      ZX_HANDLE_INVALID,
      event_handle,
  };

  constexpr uint32_t kMessageSize = sizeof(nonnullable_handle_message_layout);
  std::unique_ptr<uint8_t[]> buffer = std::make_unique<uint8_t[]>(kMessageSize);
  nonnullable_handle_message_layout& message =
      *reinterpret_cast<nonnullable_handle_message_layout*>(buffer.get());
  message.inline_struct.handle = ZX_HANDLE_INVALID;

  const char* error = nullptr;
  uint32_t actual_handles;
  auto status = fidl_encode(&nonnullable_handle_message_type, &message, kMessageSize, handles,
                            fbl::count_of(handles), &actual_handles, &error);

  ASSERT_EQ(status, ZX_ERR_INVALID_ARGS);
  ASSERT_NONNULL(error);
  ASSERT_EQ(handles[0], ZX_HANDLE_INVALID);
  ASSERT_EQ(handles[1], event_handle);

  ASSERT_EQ(zx_handle_close(event_handle), ZX_OK);

  END_TEST;
}

bool EncodeWithNullHandlesTest() {
  BEGIN_TEST;

  // When the |handles| parameter to fidl_encode is nullptr, it should still close all handles
  // inside the message.
  for (uint32_t num_handles : {0u, 1u}) {
    zx::eventpair eventpair_a;
    zx::eventpair eventpair_b;
    ASSERT_EQ(zx::eventpair::create(0, &eventpair_a, &eventpair_b), ZX_OK);

    constexpr uint32_t kMessageSize = sizeof(nonnullable_handle_message_layout);
    std::unique_ptr<uint8_t[]> buffer = std::make_unique<uint8_t[]>(kMessageSize);
    nonnullable_handle_message_layout& message =
        *reinterpret_cast<nonnullable_handle_message_layout*>(buffer.get());
    message.inline_struct.handle = eventpair_a.release();

    const char* error = nullptr;
    uint32_t actual_handles;
    auto status = fidl_encode(&nonnullable_handle_message_type, &message, kMessageSize, nullptr,
                              num_handles, &actual_handles, &error);

    ASSERT_EQ(status, ZX_ERR_INVALID_ARGS);
    ASSERT_NONNULL(error);
    ASSERT_FALSE(IsPeerValid(zx::unowned_eventpair(eventpair_b)));
  }

  END_TEST;
}

bool EncodeWithNullOutActualHandlesTest() {
  BEGIN_TEST;

  // When the |out_actual_handles| parameter to fidl_encode is nullptr, it should still close
  // all handles inside the message.

  zx::eventpair eventpair_a;
  zx::eventpair eventpair_b;
  ASSERT_EQ(zx::eventpair::create(0, &eventpair_a, &eventpair_b), ZX_OK);
  zx_handle_t handles[1] = {};

  constexpr uint32_t kMessageSize = sizeof(nonnullable_handle_message_layout);
  std::unique_ptr<uint8_t[]> buffer = std::make_unique<uint8_t[]>(kMessageSize);
  nonnullable_handle_message_layout& message =
      *reinterpret_cast<nonnullable_handle_message_layout*>(buffer.get());
  message.inline_struct.handle = eventpair_a.release();

  const char* error = nullptr;
  auto status = fidl_encode(&nonnullable_handle_message_type, &message, kMessageSize, handles,
                            fbl::count_of(handles), nullptr, &error);

  ASSERT_EQ(status, ZX_ERR_INVALID_ARGS);
  ASSERT_NONNULL(error);
  ASSERT_FALSE(IsPeerValid(zx::unowned_eventpair(eventpair_b)));

  END_TEST;
}

bool DecodeErrorTest() {
  BEGIN_TEST;

  // If an unknown envelope causes the handles contained within to be closed, and later on
  // an error was encountered, the handles in the unknown envelope should not be closed again.
  zx::eventpair eventpair_a;
  zx::eventpair eventpair_b;
  ASSERT_EQ(zx::eventpair::create(0, &eventpair_a, &eventpair_b), ZX_OK);

  // It should close all handles in case of failure. Add an extra handle at the end of the
  // handle array to detect this.
  zx::eventpair eventpair_x;
  zx::eventpair eventpair_y;
  ASSERT_EQ(zx::eventpair::create(0, &eventpair_x, &eventpair_y), ZX_OK);

  // Assemble an encoded TableOfStructWithHandle, with first field correctly populated,
  // but second field missing non-nullable handles.
  constexpr uint32_t buf_size = 512;
  uint8_t buffer[buf_size] = {};
  TableOfStructLayout* msg = reinterpret_cast<TableOfStructLayout*>(&buffer[0]);
  msg->envelope_vector.set_data(reinterpret_cast<fidl_envelope_t*>(FIDL_ALLOC_PRESENT));
  msg->envelope_vector.set_count(2);
  msg->envelopes.a = fidl_envelope_t{.num_bytes = sizeof(OrdinalOneStructWithHandle),
                                     .num_handles = 1,
                                     .presence = FIDL_ALLOC_PRESENT};
  msg->envelopes.b = fidl_envelope_t{.num_bytes = sizeof(OrdinalTwoStructWithManyHandles),
                                     .num_handles = 0,
                                     .presence = FIDL_ALLOC_PRESENT};
  msg->a = OrdinalOneStructWithHandle{.h = FIDL_HANDLE_PRESENT, .foo = 42};
  msg->b =
      OrdinalTwoStructWithManyHandles{.h1 = ZX_HANDLE_INVALID, .h2 = ZX_HANDLE_INVALID, .hs = {}};

  ASSERT_TRUE(IsPeerValid(zx::unowned_eventpair(eventpair_a)));

  const char* out_error = nullptr;
  zx_handle_t handles[] = {eventpair_b.release(), eventpair_y.release()};
  auto status = fidl_decode(&fidl_test_coding_SmallerTableOfStructWithHandleTable, buffer, buf_size,
                            handles, fbl::count_of(handles), &out_error);
  ASSERT_EQ(status, ZX_ERR_INVALID_ARGS);
  ASSERT_NONNULL(out_error);

  // The peer was closed by the decoder
  ASSERT_FALSE(IsPeerValid(zx::unowned_eventpair(eventpair_a)));
  ASSERT_FALSE(IsPeerValid(zx::unowned_eventpair(eventpair_x)));

  END_TEST;
}

BEGIN_TEST_CASE(on_error_close_handle)
RUN_TEST(EncodeErrorTest)
RUN_TEST(EncodeWithNullHandlesTest)
RUN_TEST(EncodeWithNullOutActualHandlesTest)
RUN_TEST(DecodeErrorTest)
END_TEST_CASE(on_error_close_handle)

}  // namespace
}  // namespace fidl
