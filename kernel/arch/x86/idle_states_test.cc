// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <arch/arch_ops.h>
#include <arch/x86.h>
#include <arch/x86/feature.h>
#include <arch/x86/idle_states.h>
#include <kernel/thread.h>
#include <platform.h>
#include <platform/timer.h>
#include <zircon/time.h>

#include <stdlib.h>

#include <lib/unittest/unittest.h>

namespace {

x86_idle_states_t kC1OnlyIdleStates = {.states = {X86_BASE_CSTATE(0)}};

x86_idle_states_t kKabyLakeIdleStates = {
    .states = {{.name = "C6", .mwait_hint = 0x50, .exit_latency = 151, .flushes_tlb = true},
               {.name = "C3", .mwait_hint = 0x20, .exit_latency = 79, .flushes_tlb = false},
               {.name = "C1E", .mwait_hint = 0x01, .exit_latency = 1, .flushes_tlb = false},
               X86_BASE_CSTATE(0)}};

bool test_c1_only() {
    BEGIN_TEST;

    X86IdleStates states(&kC1OnlyIdleStates);
    ASSERT_EQ(states.NumStates(), 1, "");
    X86IdleState* state = states.PickIdleState();
    EXPECT_EQ(strcmp(state->Name(), "C1"), 0, "");
    EXPECT_EQ(state->MwaitHint(), (uint32_t)0x00, "");

    END_TEST;
}

bool test_kbl() {
    BEGIN_TEST;

    X86IdleStates states(&kKabyLakeIdleStates);
    ASSERT_EQ(states.NumStates(), 4, "");
    X86IdleState* state = states.PickIdleState();
    EXPECT_EQ(strcmp(state->Name(), "C1"), 0, "");
    EXPECT_EQ(state->MwaitHint(), (uint32_t)0x00, "");

    END_TEST;
}

volatile uint8_t monitor;

static uint8_t kGuardValue = UINT8_MAX;

static int poke_monitor(void* arg) {
    // A short sleep ensures the main test thread has time to set up the monitor
    // and enter MWAIT.
    thread_sleep_relative(zx_duration_from_msec(1));
    monitor = kGuardValue;
    return 0;
}

bool test_enter_idle_states() {
    BEGIN_TEST;

    monitor = 0;

    if (x86_feature_test(X86_FEATURE_MON)) {
        X86IdleStates states(x86_get_idle_states());
        for (int i = 0; i < states.NumStates(); ++i) {
            const X86IdleState& state = states.States()[i];

            unittest_printf("Entering state '%s' (MWAIT 0x%02x) on CPU %u\n", state.Name(),
                            state.MwaitHint(), arch_curr_cpu_num());

            // Thread must be created and started before arming the monitor,
            // since thread creation appears to trip the monitor latch prematurely.
            thread_t* thrd =
                thread_create("monitor_poker", &poke_monitor, nullptr, DEFAULT_PRIORITY);
            thread_detach_and_resume(thrd);

            monitor = (uint8_t)i;
            smp_mb();
            x86_monitor(&monitor);
            auto start = current_time();
            x86_mwait(state.MwaitHint());

            unittest_printf("Exiting state (%ld ns elapsed)\n",
                            zx_time_sub_time(current_time(), start));

            thread_join(thrd, nullptr, ZX_TIME_INFINITE);
        }
    } else {
        unittest_printf("Skipping test; MWAIT/MONITOR not supported\n");
    }

    END_TEST;
}

} // namespace

UNITTEST_START_TESTCASE(x86_idle_states_tests)
UNITTEST("Select an idle state using data from a CPU with only C1.", test_c1_only)
UNITTEST("Select an idle state using data from a Kabylake CPU.", test_kbl)
UNITTEST("Enter each supported idle state using MWAIT/MONITOR.", test_enter_idle_states)
UNITTEST_END_TESTCASE(x86_idle_states_tests, "x86_idle_states",
                      "Test idle state enumeration and selection (x86 only).");
