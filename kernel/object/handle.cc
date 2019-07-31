// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <object/handle.h>

#include <object/dispatcher.h>
#include <fbl/arena.h>
#include <fbl/mutex.h>
#include <lib/counters.h>
#include <pow2.h>

namespace {

// The number of outstanding (live) handles in the arena.
constexpr size_t kMaxHandleCount = 256 * 1024u;

// Warning level: high_handle_count() is called when
// there are this many outstanding handles.
constexpr size_t kHighHandleCount = (kMaxHandleCount * 7) / 8;

KCOUNTER(handle_count_made, "handles.made")
KCOUNTER(handle_count_duped, "handles.duped")
KCOUNTER(handle_count_live, "handles.live")

// Masks for building a Handle's base_value, which ProcessDispatcher
// uses to create zx_handle_t values.
//
// base_value bit fields:
//   [31..(32 - kHandleReservedBits)]                     : Must be zero
//   [(31 - kHandleReservedBits)..kHandleGenerationShift] : Generation number
//                                                          Masked by kHandleGenerationMask
//   [kHandleGenerationShift-1..0]                        : Index into handle_arena
//                                                          Masked by kHandleIndexMask
constexpr uint32_t kHandleIndexMask = kMaxHandleCount - 1;
static_assert((kHandleIndexMask & kMaxHandleCount) == 0,
              "kMaxHandleCount must be a power of 2");

constexpr uint32_t kHandleReservedBitsMask = ((1 << kHandleReservedBits) - 1)
                                           << (32 - kHandleReservedBits);
constexpr uint32_t kHandleGenerationMask = ~kHandleIndexMask & ~kHandleReservedBitsMask;
constexpr uint32_t kHandleGenerationShift = log2_uint_floor(kMaxHandleCount);
static_assert(((3 << (kHandleGenerationShift - 1)) & kHandleGenerationMask) ==
                  1 << kHandleGenerationShift,
              "Shift is wrong");
static_assert((kHandleGenerationMask >> kHandleGenerationShift) >= 255,
              "Not enough room for a useful generation count");

static_assert((kHandleReservedBitsMask & kHandleGenerationMask) == 0, "Handle Mask Overlap!");
static_assert((kHandleReservedBitsMask & kHandleIndexMask) == 0, "Handle Mask Overlap!");
static_assert((kHandleGenerationMask & kHandleIndexMask) == 0, "Handle Mask Overlap!");
static_assert((kHandleReservedBitsMask | kHandleGenerationMask | kHandleIndexMask) == 0xffffffffu,
              "Handle masks do not cover all bits!");

}  // namespace

fbl::Arena Handle::arena_;

void Handle::Init() TA_NO_THREAD_SAFETY_ANALYSIS {
    arena_.Init("handles", sizeof(Handle), kMaxHandleCount);
}

void Handle::set_process_id(zx_koid_t pid) {
    process_id_.store(pid, ktl::memory_order_relaxed);
    dispatcher_->set_owner(pid);
}

// Returns a new |base_value| based on the value stored in the free
// arena slot pointed to by |addr|. The new value will be different
// from the last |base_value| used by this slot.
uint32_t Handle::GetNewBaseValue(void* addr) TA_REQ(ArenaLock::Get()) {
    // Get the index of this slot within the arena.
    uint32_t handle_index = HandleToIndex(reinterpret_cast<Handle*>(addr));
    DEBUG_ASSERT((handle_index & ~kHandleIndexMask) == 0);

    // Check the free memory for a stashed base_value.
    uint32_t v = *reinterpret_cast<uint32_t*>(addr);
    uint32_t old_gen = 0;
    if (v != 0) {
        // This slot has been used before.
        DEBUG_ASSERT((v & kHandleIndexMask) == handle_index);
        old_gen = (v & kHandleGenerationMask) >> kHandleGenerationShift;
    }
    uint32_t new_gen =
        (((old_gen + 1) << kHandleGenerationShift) & kHandleGenerationMask);
    return (handle_index | new_gen);
}

// Allocate space for a Handle from the arena, but don't instantiate the
// object.  |base_value| gets the value for Handle::base_value_.  |what|
// says whether this is allocation or duplication, for the error message.
void* Handle::Alloc(const fbl::RefPtr<Dispatcher>& dispatcher,
                    const char* what, uint32_t* base_value) {
    size_t outstanding_handles;
    {
        Guard<BrwLockPi, BrwLockPi::Writer> guard{ArenaLock::Get()};
        void* addr = arena_.Alloc();
        outstanding_handles = arena_.DiagnosticCount();
        if (likely(addr)) {
            if (outstanding_handles > kHighHandleCount) {
                // TODO: Avoid calling this for every handle after
                // kHighHandleCount; printfs are slow and we're
                // holding the lock.
                printf("WARNING: High handle count: %zu handles\n",
                       outstanding_handles);
            }
            dispatcher->increment_handle_count();
            *base_value = GetNewBaseValue(addr);
            return addr;
        }
    }

    printf("WARNING: Could not allocate %s handle (%zu outstanding)\n",
           what, outstanding_handles);
    return nullptr;
}

HandleOwner Handle::Make(fbl::RefPtr<Dispatcher> dispatcher,
                         zx_rights_t rights) {
    uint32_t base_value;
    void* addr = Alloc(dispatcher, "new", &base_value);
    if (unlikely(!addr))
        return nullptr;
    kcounter_add(handle_count_made, 1);
    kcounter_add(handle_count_live, 1);
    return HandleOwner(new (addr) Handle(ktl::move(dispatcher),
                                         rights, base_value));
}

HandleOwner Handle::Make(KernelHandle<Dispatcher> kernel_handle,
                         zx_rights_t rights) {
    uint32_t base_value;
    void* addr = Alloc(kernel_handle.dispatcher(), "new", &base_value);
    if (unlikely(!addr))
        return nullptr;
    kcounter_add(handle_count_made, 1);
    kcounter_add(handle_count_live, 1);
    return HandleOwner(new (addr) Handle(kernel_handle.release(),
                                         rights, base_value));
}

// Called only by Make.
Handle::Handle(fbl::RefPtr<Dispatcher> dispatcher, zx_rights_t rights,
               uint32_t base_value)
    : process_id_(0u),
      dispatcher_(ktl::move(dispatcher)),
      rights_(rights),
      base_value_(base_value) {
}

HandleOwner Handle::Dup(Handle* source, zx_rights_t rights) {
    uint32_t base_value;
    void* addr = Alloc(source->dispatcher(), "duplicate", &base_value);
    if (unlikely(!addr))
        return nullptr;
    kcounter_add(handle_count_duped, 1);
    kcounter_add(handle_count_live, 1);
    return HandleOwner(new (addr) Handle(source, rights, base_value));
}

// Called only by Dup.
Handle::Handle(Handle* rhs, zx_rights_t rights, uint32_t base_value)
    : process_id_(rhs->process_id()),
      dispatcher_(rhs->dispatcher_),
      rights_(rights),
      base_value_(base_value) {
}

// Destroys, but does not free, the Handle, and fixes up its memory to protect
// against stale pointers to it. Also stashes the Handle's base_value for reuse
// the next time this slot is allocated.
void Handle::TearDown() TA_EXCL(ArenaLock::Get()) {
    uint32_t old_base_value = base_value();

    // There may be stale pointers to this slot and they will look at process_id. We expect
    // process_id to already have been cleared by the process dispatcher before the handle got to
    // this point.
    DEBUG_ASSERT(process_id() == 0);

    // Explicitly reset the dispatcher to drop the reference, if this deletes the dispatcher then
    // many things could ultimately happen and so it is important that this be outside the lock.
    // Performing an explicit reset instead of letting it happen in the destructor means that the
    // pointer gets reset to null, which is important in case there are stale pointers to this slot.
    this->dispatcher_.reset();
    // The destructor does not do much of interest now since we have already cleaned up the
    // dispatcher_ ref, but call it for completeness.
    this->~Handle();

    // Hold onto the base_value for the next user of this slot, stashing
    // it at the beginning of the free slot.
    *reinterpret_cast<uint32_t*>(this) = old_base_value;
}

void Handle::Delete() {
    fbl::RefPtr<Dispatcher> disp = dispatcher();

    if (disp->is_waitable())
        disp->Cancel(this);

    TearDown();

    bool zero_handles = false;
    {
        Guard<BrwLockPi, BrwLockPi::Writer> guard{ArenaLock::Get()};
        zero_handles = disp->decrement_handle_count();
        arena_.Free(this);
    }

    if (zero_handles)
        disp->on_zero_handles();

    // If |disp| is the last reference then the dispatcher object
    // gets destroyed here.
    kcounter_add(handle_count_live, -1);
}

Handle* Handle::FromU32(uint32_t value) TA_NO_THREAD_SAFETY_ANALYSIS {
    uintptr_t handle_addr = IndexToHandle(value & kHandleIndexMask);
    {
        Guard<BrwLockPi, BrwLockPi::Reader> guard{ArenaLock::Get()};
        if (unlikely(!arena_.in_range(handle_addr)))
            return nullptr;
    }
    auto handle = reinterpret_cast<Handle*>(handle_addr);
    return likely(handle->base_value() == value) ? handle : nullptr;
}

uint32_t Handle::Count(const fbl::RefPtr<const Dispatcher>& dispatcher) {
    // Handle::ArenaLock also guards Dispatcher::handle_count_.
    Guard<BrwLockPi, BrwLockPi::Reader> guard{ArenaLock::Get()};
    return dispatcher->current_handle_count();
}

size_t Handle::diagnostics::OutstandingHandles() {
    Guard<BrwLockPi, BrwLockPi::Reader> guard{ArenaLock::Get()};
    return arena_.DiagnosticCount();
}

void Handle::diagnostics::DumpTableInfo() {
    Guard<BrwLockPi, BrwLockPi::Reader> guard{ArenaLock::Get()};
    arena_.Dump();
}
