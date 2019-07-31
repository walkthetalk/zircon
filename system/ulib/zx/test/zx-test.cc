// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <lib/fzl/time.h>
#include <lib/zx/bti.h>
#include <lib/zx/channel.h>
#include <lib/zx/event.h>
#include <lib/zx/eventpair.h>
#include <lib/zx/handle.h>
#include <lib/zx/iommu.h>
#include <lib/zx/job.h>
#include <lib/zx/port.h>
#include <lib/zx/process.h>
#include <lib/zx/profile.h>
#include <lib/zx/socket.h>
#include <lib/zx/suspend_token.h>
#include <lib/zx/thread.h>
#include <lib/zx/time.h>
#include <lib/zx/vmar.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>
#include <zircon/syscalls/port.h>
#include <zxtest/zxtest.h>

#include <utility>

namespace {

zx_status_t validate_handle(zx_handle_t handle) {
    return zx_object_get_info(handle, ZX_INFO_HANDLE_VALID,
                              nullptr, 0, 0u, nullptr);
}

TEST(ZxTestCase, HandleInvalid) {
    zx::handle handle;
    // A default constructed handle is invalid.
    ASSERT_EQ(handle.release(), ZX_HANDLE_INVALID);
}

TEST(ZxTestCase, HandleClose) {
    zx_handle_t raw_event;
    ASSERT_OK(zx_event_create(0u, &raw_event));
    ASSERT_OK(validate_handle(raw_event));
    {
        zx::handle handle(raw_event);
    }
    // Make sure the handle was closed.
    ASSERT_EQ(validate_handle(raw_event), ZX_ERR_BAD_HANDLE);
}

TEST(ZxTestCase, HandleMove) {
    zx::event event;
    // Check move semantics.
    ASSERT_OK(zx::event::create(0u, &event));
    zx::handle handle(std::move(event));
    ASSERT_EQ(event.release(), ZX_HANDLE_INVALID);
    ASSERT_OK(validate_handle(handle.get()));
}

TEST(ZxTestCase, HandleDuplicate) {
    zx_handle_t raw_event;
    zx::handle dup;
    ASSERT_OK(zx_event_create(0u, &raw_event));
    zx::handle handle(raw_event);
    ASSERT_OK(handle.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup));
    // The duplicate must be valid as well as the original.
    ASSERT_OK(validate_handle(dup.get()));
    ASSERT_OK(validate_handle(raw_event));
}

TEST(ZxTestCase, HandleReplace) {
    zx_handle_t raw_event;
    zx::handle rep;
    ASSERT_OK(zx_event_create(0u, &raw_event));
    {
        zx::handle handle(raw_event);
        ASSERT_OK(handle.replace(ZX_RIGHT_SAME_RIGHTS, &rep));
        ASSERT_EQ(handle.release(), ZX_HANDLE_INVALID);
    }
    // The original shoould be invalid and the replacement should be valid.
    ASSERT_EQ(validate_handle(raw_event), ZX_ERR_BAD_HANDLE);
    ASSERT_OK(validate_handle(rep.get()));
}

TEST(ZxTestCase, Event) {
    zx::event event;
    ASSERT_OK(zx::event::create(0u, &event));
    ASSERT_OK(validate_handle(event.get()));
    // TODO(cpu): test more.
}

TEST(ZxTestCase, EventDuplicate) {
    zx::event event;
    zx::event dup;
    ASSERT_OK(zx::event::create(0u, &event));
    ASSERT_OK(event.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup));
    // The duplicate must be valid as well as the original.
    ASSERT_OK(validate_handle(dup.get()));
    ASSERT_OK(validate_handle(event.get()));
}

TEST(ZxTestCase, BtiCompilation) {
    zx::bti bti;
    // TODO(teisenbe): test more.
}

TEST(ZxTestCase, PmtCompilation) {
    zx::pmt pmt;
    // TODO(teisenbe): test more.
}

TEST(ZxTestCase, IommuCompilation) {
    zx::iommu iommu;
    // TODO(teisenbe): test more.
}

TEST(ZxTestCase, Channel) {
    zx::channel channel[2];
    ASSERT_OK(zx::channel::create(0u, &channel[0], &channel[1]));
    ASSERT_OK(validate_handle(channel[0].get()));
    ASSERT_OK(validate_handle(channel[1].get()));
    // TODO(cpu): test more.
}

TEST(ZxTestCase, ChannelRw) {
    zx::eventpair eventpair[2];
    ASSERT_OK(zx::eventpair::create(0u, &eventpair[0], &eventpair[1]));

    zx::channel channel[2];
    ASSERT_OK(zx::channel::create(0u, &channel[0], &channel[1]));

    zx_handle_t handles[2] = {
        eventpair[0].release(),
        eventpair[1].release()
    };

    zx_handle_t recv[2] = {0};

    ASSERT_OK(channel[0].write(0u, nullptr, 0u, handles, 2));
    ASSERT_OK(channel[1].read(0u, nullptr, recv, 0u, 2, nullptr, nullptr));

    ASSERT_OK(zx_handle_close(recv[0]));
    ASSERT_OK(zx_handle_close(recv[1]));
}

TEST(ZxTestCase, ChannelRwEtc) {
    zx::eventpair eventpair[2];
    ASSERT_OK(zx::eventpair::create(0u, &eventpair[0], &eventpair[1]));

    zx::channel channel[2];
    ASSERT_OK(zx::channel::create(0u, &channel[0], &channel[1]));

    zx_handle_t handles[2] = {
        eventpair[0].release(),
        eventpair[1].release()
    };

    zx_handle_info_t recv[2] = {{}};
    uint32_t h_count = 0;

    ASSERT_OK(channel[0].write(0u, nullptr, 0u, handles, 2));
    ASSERT_OK(channel[1].read_etc(0u, nullptr, recv, 0u, 2, nullptr, &h_count));

    ASSERT_EQ(h_count, 2u);
    ASSERT_EQ(recv[0].type, ZX_OBJ_TYPE_EVENTPAIR);
    ASSERT_EQ(recv[1].type, ZX_OBJ_TYPE_EVENTPAIR);

    ASSERT_OK(zx_handle_close(recv[0].handle));
    ASSERT_OK(zx_handle_close(recv[1].handle));
}

TEST(ZxTestCase, Socket) {
    zx::socket socket[2];
    ASSERT_OK(zx::socket::create(0u, &socket[0], &socket[1]));
    ASSERT_OK(validate_handle(socket[0].get()));
    ASSERT_OK(validate_handle(socket[1].get()));
    // TODO(cpu): test more.
}

TEST(ZxTestCase, EventPair) {
    zx::eventpair eventpair[2];
    ASSERT_OK(zx::eventpair::create(0u, &eventpair[0], &eventpair[1]));
    ASSERT_OK(validate_handle(eventpair[0].get()));
    ASSERT_OK(validate_handle(eventpair[1].get()));
    // TODO(cpu): test more.
}

TEST(ZxTestCase, Vmar) {
    zx::vmar vmar;
    const size_t size = getpagesize();
    uintptr_t addr;
    ASSERT_OK(zx::vmar::root_self()->allocate(0u, size, ZX_VM_CAN_MAP_READ, &vmar, &addr));
    ASSERT_OK(validate_handle(vmar.get()));
    ASSERT_OK(vmar.destroy());
    // TODO(teisenbe): test more.
}

TEST(ZxTestCase, Port) {
    zx::port port;
    ASSERT_OK(zx::port::create(0, &port));
    ASSERT_OK(validate_handle(port.get()));

    zx::channel channel[2];
    auto key = 1111ull;
    ASSERT_OK(zx::channel::create(0u, &channel[0], &channel[1]));
    ASSERT_OK(channel[0].wait_async(port, key, ZX_CHANNEL_READABLE, ZX_WAIT_ASYNC_ONCE));
    ASSERT_OK(channel[1].write(0u, "12345", 5, nullptr, 0u));

    zx_port_packet_t packet = {};
    ASSERT_OK(port.wait(zx::time(), &packet));
    ASSERT_EQ(packet.key, key);
    ASSERT_EQ(packet.type, ZX_PKT_TYPE_SIGNAL_ONE);
    ASSERT_EQ(packet.signal.count, 1u);
}

TEST(ZxTestCase, TimeConstruction) {
    // time construction
    ASSERT_EQ(zx::time().get(), 0);
    ASSERT_EQ(zx::time::infinite().get(), ZX_TIME_INFINITE);
    ASSERT_EQ(zx::time(-1).get(), -1);
    ASSERT_EQ(zx::time(ZX_TIME_INFINITE_PAST).get(), ZX_TIME_INFINITE_PAST);
}

TEST(ZxTestCase, DurationConstruction) {
    // duration construction
    ASSERT_EQ(zx::duration().get(), 0);
    ASSERT_EQ(zx::duration::infinite().get(), ZX_TIME_INFINITE);
    ASSERT_EQ(zx::duration(-1).get(), -1);
    ASSERT_EQ(zx::duration(ZX_TIME_INFINITE_PAST).get(), ZX_TIME_INFINITE_PAST);
}

TEST(ZxTestCase, DurationConversions) {
    // duration to/from nsec, usec, msec, etc.
    ASSERT_EQ(zx::nsec(-10).get(), ZX_NSEC(-10));
    ASSERT_EQ(zx::nsec(-10).to_nsecs(), -10);
    ASSERT_EQ(zx::nsec(10).get(), ZX_NSEC(10));
    ASSERT_EQ(zx::nsec(10).to_nsecs(), 10);
    ASSERT_EQ(zx::usec(10).get(), ZX_USEC(10));
    ASSERT_EQ(zx::usec(10).to_usecs(), 10);
    ASSERT_EQ(zx::msec(10).get(), ZX_MSEC(10));
    ASSERT_EQ(zx::msec(10).to_msecs(), 10);
    ASSERT_EQ(zx::sec(10).get(), ZX_SEC(10));
    ASSERT_EQ(zx::sec(10).to_secs(), 10);
    ASSERT_EQ(zx::min(10).get(), ZX_MIN(10));
    ASSERT_EQ(zx::min(10).to_mins(), 10);
    ASSERT_EQ(zx::hour(10).get(), ZX_HOUR(10));
    ASSERT_EQ(zx::hour(10).to_hours(), 10);

    ASSERT_EQ((zx::time() + zx::usec(19)).get(), ZX_USEC(19));
    ASSERT_EQ((zx::usec(19) + zx::time()).get(), ZX_USEC(19));
    ASSERT_EQ((zx::time::infinite() - zx::time()).get(), ZX_TIME_INFINITE);
    ASSERT_EQ((zx::time::infinite() - zx::time::infinite()).get(), 0);
    ASSERT_EQ((zx::time() + zx::duration::infinite()).get(), ZX_TIME_INFINITE);

    zx::duration d(0u);
    d += zx::nsec(19);
    ASSERT_EQ(d.get(), ZX_NSEC(19));
    d -= zx::nsec(19);
    ASSERT_EQ(d.get(), ZX_NSEC(0));

    d = zx::min(1);
    d *= 19u;
    ASSERT_EQ(d.get(), ZX_MIN(19));
    d /= 19u;
    ASSERT_EQ(d.get(), ZX_MIN(1));

    ASSERT_EQ(zx::sec(19) % zx::sec(7), ZX_SEC(5));

    zx::time t(0u);
    t += zx::msec(19);
    ASSERT_EQ(t.get(), ZX_MSEC(19));
    t -= zx::msec(19);
    ASSERT_EQ(t.get(), ZX_MSEC(0));

    // Just a smoke test
    ASSERT_GE(zx::deadline_after(zx::usec(10)).get(), ZX_USEC(10));
}

TEST(ZxTestCase, TimeNanoSleep) {
    ASSERT_OK(zx::nanosleep(zx::time(ZX_TIME_INFINITE_PAST)));
    ASSERT_OK(zx::nanosleep(zx::time(-1)));
    ASSERT_OK(zx::nanosleep(zx::time(0)));
    ASSERT_OK(zx::nanosleep(zx::time(1)));
}

TEST(ZxTestCase, Ticks) {
    ASSERT_EQ(zx::ticks().get(), 0);

    zx::ticks before = zx::ticks::now();
    ASSERT_GT(before.get(), 0);
    zx::ticks after = before + zx::ticks(1);

    ASSERT_LT(before.get(), after.get());
    ASSERT_TRUE(before < after);
    after -= zx::ticks(1);
    ASSERT_EQ(before.get(), after.get());
    ASSERT_TRUE(before == after);

    ASSERT_EQ(zx::ticks::per_second().get(), zx_ticks_per_second());

    // Compare a duration (nanoseconds) with the ticks equivalent.
    zx::ticks second = zx::ticks::per_second();
    ASSERT_EQ(fzl::TicksToNs(second).get(), zx::sec(1).get());
    ASSERT_TRUE(fzl::TicksToNs(second) == zx::sec(1));

    // Hopefully, we haven't moved backwards in time.
    after = zx::ticks::now();
    ASSERT_LE(before.get(), after.get());
    ASSERT_TRUE(before <= after);
}

template <typename T>
void IsValidHandle(const T& p) {
    ASSERT_TRUE(static_cast<bool>(p), "invalid handle");
}

TEST(ZxTestCase, ThreadSelf) {
    zx_handle_t raw = zx_thread_self();
    ASSERT_OK(validate_handle(raw));

    ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::thread>(*zx::thread::self()));
    EXPECT_OK(validate_handle(raw));

    // This does not compile:
    //const zx::thread self = zx::thread::self();
}

void thread_suspend_test_fn(uintptr_t, uintptr_t) {
    zx_nanosleep(zx_deadline_after(ZX_SEC(1000)));
    zx_thread_exit();
}

TEST(ZxTestCase, ThreadSuspend) {
    zx::thread thread;
    ASSERT_OK(zx::thread::create(*zx::process::self(), "test", 4, 0, &thread));

    // Make a little stack and start the thread. Note: stack grows down so pass the high address.
    alignas(16) static uint8_t stack_storage[64];
    uint8_t* stack = stack_storage + sizeof(stack_storage);
    ASSERT_OK(thread.start(&thread_suspend_test_fn, stack, 0, 0));

    zx::suspend_token suspend;
    EXPECT_OK(thread.suspend(&suspend));
    EXPECT_TRUE(suspend);

    suspend.reset();
    EXPECT_OK(thread.kill());
}

TEST(ZxTestCase, ProcessSelf) {
    zx_handle_t raw = zx_process_self();
    ASSERT_OK(validate_handle(raw));

    ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::process>(*zx::process::self()));
    EXPECT_OK(validate_handle(raw));

    // This does not compile:
    //const zx::process self = zx::process::self();
}

TEST(ZxTestCase, VmarRootSelf) {
    zx_handle_t raw = zx_vmar_root_self();
    ASSERT_OK(validate_handle(raw));

    ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::vmar>(*zx::vmar::root_self()));
    EXPECT_OK(validate_handle(raw));

    // This does not compile:
    //const zx::vmar root_self = zx::vmar::root_self();
}

TEST(ZxTestCase, JobDefault) {
    zx_handle_t raw = zx_job_default();
    ASSERT_OK(validate_handle(raw));

    ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::job>(*zx::job::default_job()));
    EXPECT_OK(validate_handle(raw));

    // This does not compile:
    //const zx::job default_job = zx::job::default_job();
}

bool takes_any_handle(const zx::handle& handle) {
    return handle.is_valid();
}

TEST(ZxTestCase, HandleConversion) {
    EXPECT_TRUE(takes_any_handle(*zx::unowned_handle(zx_thread_self())));
    ASSERT_OK(validate_handle(zx_thread_self()));
}

TEST(ZxTestCase, Unowned) {
    // Create a handle to test with.
    zx::event handle;
    ASSERT_OK(zx::event::create(0, &handle));
    ASSERT_OK(validate_handle(handle.get()));

    // Verify that unowned<T>(zx_handle_t) doesn't close handle on teardown.
    {
      zx::unowned<zx::event> unowned(handle.get());
      EXPECT_EQ(unowned->get(), handle.get());
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));
    }
    ASSERT_OK(validate_handle(handle.get()));

    // Verify that unowned<T>(const T&) doesn't close handle on teardown.
    {
      zx::unowned<zx::event> unowned(handle);
      EXPECT_EQ(unowned->get(), handle.get());
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));
    }
    ASSERT_OK(validate_handle(handle.get()));

    // Verify that unowned<T>(const unowned<T>&) doesn't close on teardown.
    {
      zx::unowned<zx::event> unowned(handle);
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));

      zx::unowned<zx::event> unowned2(unowned);
      EXPECT_EQ(unowned->get(), unowned2->get());
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned2));
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));
    }
    ASSERT_OK(validate_handle(handle.get()));

    // Verify copy-assignment from unowned<> to unowned<> doesn't close.
    {
      zx::unowned<zx::event> unowned(handle);
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));

      zx::unowned<zx::event> unowned2;
      ASSERT_FALSE(unowned2->is_valid());

      const zx::unowned<zx::event>& assign_ref = unowned2 = unowned;
      EXPECT_EQ(assign_ref->get(), unowned2->get());
      EXPECT_EQ(unowned->get(), unowned2->get());
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned2));
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));
    }
    ASSERT_OK(validate_handle(handle.get()));

    // Verify move from unowned<> to unowned<> doesn't close on teardown.
    {
      zx::unowned<zx::event> unowned(handle);
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));

      zx::unowned<zx::event> unowned2(
          static_cast<zx::unowned<zx::event>&&>(unowned));
      EXPECT_EQ(unowned2->get(), handle.get());
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned2));
      EXPECT_FALSE(unowned->is_valid());
    }
    ASSERT_OK(validate_handle(handle.get()));

    // Verify move-assignment from unowned<> to unowned<> doesn't close.
    {
      zx::unowned<zx::event> unowned(handle);
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));

      zx::unowned<zx::event> unowned2;
      ASSERT_FALSE(unowned2->is_valid());

      const zx::unowned<zx::event>& assign_ref =
          unowned2 = static_cast<zx::unowned<zx::event>&&>(unowned);
      EXPECT_EQ(assign_ref->get(), unowned2->get());
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned2));
      EXPECT_FALSE(unowned->is_valid());
    }
    ASSERT_OK(validate_handle(handle.get()));

    // Verify move-assignment into non-empty unowned<>  doesn't close.
    {
      zx::unowned<zx::event> unowned(handle);
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));

      zx::unowned<zx::event> unowned2(handle);
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned2));

      unowned2 = static_cast<zx::unowned<zx::event>&&>(unowned);
      EXPECT_EQ(unowned2->get(), handle.get());
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned2));
      EXPECT_FALSE(unowned->is_valid());
    }
    ASSERT_OK(validate_handle(handle.get()));

    // Explicitly verify dereference operator allows methods to be called.
    {
      zx::unowned<zx::event> unowned(handle);
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));

      const zx::event& event_ref = *unowned;
      zx::event duplicate;
      EXPECT_OK(event_ref.duplicate(ZX_RIGHT_SAME_RIGHTS, &duplicate));
    }
    ASSERT_OK(validate_handle(handle.get()));

    // Explicitly verify member access operator allows methods to be called.
    {
      zx::unowned<zx::event> unowned(handle);
      ASSERT_NO_FATAL_FAILURES(IsValidHandle<zx::event>(*unowned));

      zx::event duplicate;
      EXPECT_OK(unowned->duplicate(ZX_RIGHT_SAME_RIGHTS, &duplicate));
    }
    ASSERT_OK(validate_handle(handle.get()));
}

TEST(ZxTestCase, GetChild) {
    {
        // Verify handle and job overrides of get_child() can find this process
        // by KOID.
        zx_info_handle_basic_t info = {};
        ASSERT_OK(zx_object_get_info(zx_process_self(), ZX_INFO_HANDLE_BASIC,
                                     &info, sizeof(info), nullptr, nullptr));

        zx::handle as_handle;
        ASSERT_OK(zx::job::default_job()->get_child(info.koid, ZX_RIGHT_SAME_RIGHTS, &as_handle));
        ASSERT_OK(validate_handle(as_handle.get()));

        zx::process as_process;
        ASSERT_OK(zx::job::default_job()->get_child(info.koid, ZX_RIGHT_SAME_RIGHTS, &as_process));
        ASSERT_OK(validate_handle(as_process.get()));
    }

    {
        // Verify handle and thread overrides of get_child() can find this
        // thread by KOID.
        zx_info_handle_basic_t info = {};
        ASSERT_OK(zx_object_get_info(zx_thread_self(), ZX_INFO_HANDLE_BASIC,
                                     &info, sizeof(info), nullptr, nullptr));

        zx::handle as_handle;
        ASSERT_OK(zx::process::self()->get_child(info.koid, ZX_RIGHT_SAME_RIGHTS, &as_handle));
        ASSERT_OK(validate_handle(as_handle.get()));

        zx::thread as_thread;
        ASSERT_OK(zx::process::self()->get_child(info.koid, ZX_RIGHT_SAME_RIGHTS, &as_thread));
        ASSERT_OK(validate_handle(as_thread.get()));
    }
}

}  // namespace
