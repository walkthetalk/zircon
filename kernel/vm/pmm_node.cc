// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#include "pmm_node.h"

#include <inttypes.h>
#include <kernel/mp.h>
#include <new>
#include <trace.h>
#include <vm/bootalloc.h>
#include <vm/physmap.h>

#include "vm_priv.h"

#define LOCAL_TRACE MAX(VM_GLOBAL_TRACE, 0)

PmmNode::PmmNode() {
}

PmmNode::~PmmNode() {
}

// We disable thread safety analysis here, since this function is only called
// during early boot before threading exists.
zx_status_t PmmNode::AddArena(const pmm_arena_info_t* info) TA_NO_THREAD_SAFETY_ANALYSIS {
    LTRACEF("arena %p name '%s' base %#" PRIxPTR " size %#zx\n", info, info->name, info->base, info->size);

    // Make sure we're in early boot (ints disabled and no active CPUs according
    // to the scheduler).
    DEBUG_ASSERT(mp_get_active_mask() == 0);
    DEBUG_ASSERT(arch_ints_disabled());

    DEBUG_ASSERT(IS_PAGE_ALIGNED(info->base));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(info->size));
    DEBUG_ASSERT(info->size > 0);

    // allocate a c++ arena object
    PmmArena* arena = new (boot_alloc_mem(sizeof(PmmArena))) PmmArena();

    // initialize the object
    auto status = arena->Init(info, this);
    if (status != ZX_OK) {
        // leaks boot allocator memory
        arena->~PmmArena();
        printf("PMM: pmm_add_arena failed to initialize arena\n");
        return status;
    }

    // walk the arena list and add arena based on priority order
    for (auto& a : arena_list_) {
        if (a.priority() > arena->priority()) {
            arena_list_.insert(a, arena);
            goto done_add;
        }
    }

    // walked off the end, add it to the end of the list
    arena_list_.push_back(arena);

done_add:
    arena_cumulative_size_ += info->size;

    return ZX_OK;
}

// called at boot time as arenas are brought online, no locks are acquired
void PmmNode::AddFreePages(list_node* list) TA_NO_THREAD_SAFETY_ANALYSIS {
    LTRACEF("list %p\n", list);

    vm_page *temp, *page;
    list_for_every_entry_safe (list, page, temp, vm_page, queue_node) {
        list_delete(&page->queue_node);
        list_add_tail(&free_list_, &page->queue_node);
        free_count_++;
    }

    LTRACEF("free count now %" PRIu64 "\n", free_count_);
}

static void alloc_page_helper(vm_page* page) {
    LTRACEF("allocating page %p, pa %#" PRIxPTR ", prev state %s\n",
            page, page->paddr(), page_state_to_string(page->state()));

    DEBUG_ASSERT(page->is_free());

    page->set_state(VM_PAGE_STATE_ALLOC);

#if PMM_ENABLE_FREE_FILL
    CheckFreeFill(page);
#endif
}

zx_status_t PmmNode::AllocPage(uint alloc_flags, vm_page_t** page_out, paddr_t* pa_out) {
    Guard<fbl::Mutex> guard{&lock_};

    vm_page* page = list_remove_head_type(&free_list_, vm_page, queue_node);
    if (!page) {
        return ZX_ERR_NO_MEMORY;
    }

    alloc_page_helper(page);

    DEBUG_ASSERT(free_count_ > 0);
    free_count_--;

    if (pa_out) {
        *pa_out = page->paddr();
    }

    if (page_out) {
        *page_out = page;
    }

    return ZX_OK;
}

zx_status_t PmmNode::AllocPages(size_t count, uint alloc_flags, list_node* list) {
    LTRACEF("count %zu\n", count);

    // list must be initialized prior to calling this
    DEBUG_ASSERT(list);

    if (unlikely(count == 0)) {
        return ZX_OK;
    } else if (count == 1) {
        vm_page* page;
        zx_status_t status = AllocPage(alloc_flags, &page, nullptr);
        if (likely(status == ZX_OK)) {
            list_add_tail(list, &page->queue_node);
        }
        return status;
    }

    Guard<fbl::Mutex> guard{&lock_};

    if (unlikely(count > free_count_)) {
        return ZX_ERR_NO_MEMORY;
    }
    free_count_ -= count;

    auto node = &free_list_;
    while (count-- > 0) {
        node = list_next(&free_list_, node);
        alloc_page_helper(containerof(node, vm_page, queue_node));
    }

    list_node tmp_list = LIST_INITIAL_VALUE(tmp_list);
    list_split_after(&free_list_, node, &tmp_list);
    if (list_is_empty(list)) {
        list_move(&free_list_, list);
    } else {
        list_splice_after(&free_list_, list_peek_tail(list));
    }
    list_move(&tmp_list, &free_list_);

    return ZX_OK;
}

zx_status_t PmmNode::AllocRange(paddr_t address, size_t count, list_node* list) {
    LTRACEF("address %#" PRIxPTR ", count %zu\n", address, count);

    // list must be initialized prior to calling this
    DEBUG_ASSERT(list);

    size_t allocated = 0;
    if (count == 0) {
        return ZX_OK;
    }

    address = ROUNDDOWN(address, PAGE_SIZE);

    Guard<fbl::Mutex> guard{&lock_};

    // walk through the arenas, looking to see if the physical page belongs to it
    for (auto& a : arena_list_) {
        while (allocated < count && a.address_in_arena(address)) {
            vm_page_t* page = a.FindSpecific(address);
            if (!page) {
                break;
            }

            if (!page->is_free()) {
                break;
            }

            list_delete(&page->queue_node);

            page->set_state(VM_PAGE_STATE_ALLOC);

            list_add_tail(list, &page->queue_node);

            allocated++;
            address += PAGE_SIZE;
            free_count_--;
        }

        if (allocated == count) {
            break;
        }
    }

    if (allocated != count) {
        // we were not able to allocate the entire run, free these pages
        FreeListLocked(list);
        return ZX_ERR_NOT_FOUND;
    }

    return ZX_OK;
}

zx_status_t PmmNode::AllocContiguous(const size_t count, uint alloc_flags, uint8_t alignment_log2,
                                     paddr_t* pa, list_node* list) {
    LTRACEF("count %zu, align %u\n", count, alignment_log2);

    if (count == 0) {
        return ZX_OK;
    }
    if (alignment_log2 < PAGE_SIZE_SHIFT) {
        alignment_log2 = PAGE_SIZE_SHIFT;
    }

    // pa and list must be valid pointers
    DEBUG_ASSERT(pa);
    DEBUG_ASSERT(list);

    Guard<fbl::Mutex> guard{&lock_};

    for (auto& a : arena_list_) {
        vm_page_t* p = a.FindFreeContiguous(count, alignment_log2);
        if (!p) {
            continue;
        }

        *pa = p->paddr();

        // remove the pages from the run out of the free list
        for (size_t i = 0; i < count; i++, p++) {
            DEBUG_ASSERT_MSG(p->is_free(), "p %p state %u\n", p, p->state());
            DEBUG_ASSERT(list_in_list(&p->queue_node));

            list_delete(&p->queue_node);
            p->set_state(VM_PAGE_STATE_ALLOC);

            DEBUG_ASSERT(free_count_ > 0);

            free_count_--;

#if PMM_ENABLE_FREE_FILL
            CheckFreeFill(p);
#endif

            list_add_tail(list, &p->queue_node);
        }

        return ZX_OK;
    }

    LTRACEF("couldn't find run\n");
    return ZX_ERR_NOT_FOUND;
}

void PmmNode::FreePageHelperLocked(vm_page* page) {
    LTRACEF("page %p state %u paddr %#" PRIxPTR "\n", page, page->state(), page->paddr());

    DEBUG_ASSERT(page->state() != VM_PAGE_STATE_OBJECT || page->object.pin_count == 0);
    DEBUG_ASSERT(!page->is_free());

#if PMM_ENABLE_FREE_FILL
    FreeFill(page);
#endif

    // mark it free
    page->set_state(VM_PAGE_STATE_FREE);
}

void PmmNode::FreePage(vm_page* page) {
    Guard<fbl::Mutex> guard{&lock_};

    // pages freed individually shouldn't be in a queue
    DEBUG_ASSERT(!list_in_list(&page->queue_node));

    FreePageHelperLocked(page);

    // add it to the free queue
    list_add_head(&free_list_, &page->queue_node);

    free_count_++;
}

void PmmNode::FreeListLocked(list_node* list) {
    DEBUG_ASSERT(list);

    // process list backwards so the head is as hot as possible
    uint64_t count = 0;
    for (vm_page* page = list_peek_tail_type(list, vm_page, queue_node); page != nullptr;
            page = list_prev_type(list, &page->queue_node, vm_page, queue_node)) {
        FreePageHelperLocked(page);
        count++;
    }

    // splice list at the head of free_list_
    list_splice_after(list, &free_list_);

    free_count_ += count;
}

void PmmNode::FreeList(list_node* list) {
    Guard<fbl::Mutex> guard{&lock_};

    FreeListLocked(list);
}

// okay if accessed outside of a lock
uint64_t PmmNode::CountFreePages() const TA_NO_THREAD_SAFETY_ANALYSIS {
    return free_count_;
}

uint64_t PmmNode::CountTotalBytes() const TA_NO_THREAD_SAFETY_ANALYSIS {
    return arena_cumulative_size_;
}

void PmmNode::DumpFree() const TA_NO_THREAD_SAFETY_ANALYSIS {
    auto megabytes_free = CountFreePages() / 256u;
    printf(" %zu free MBs\n", megabytes_free);
}

void PmmNode::Dump(bool is_panic) const {
    // No lock analysis here, as we want to just go for it in the panic case without the lock.
    auto dump = [this]() TA_NO_THREAD_SAFETY_ANALYSIS {
        printf("pmm node %p: free_count %zu (%zu bytes), total size %zu\n",
               this, free_count_, free_count_ * PAGE_SIZE, arena_cumulative_size_);
        for (auto& a : arena_list_) {
            a.Dump(false, false);
        }
    };

    if (is_panic) {
        dump();
    } else {
        Guard<fbl::Mutex> guard{&lock_};
        dump();
    }
}

#if PMM_ENABLE_FREE_FILL
void PmmNode::EnforceFill() {
    DEBUG_ASSERT(!enforce_fill_);

    vm_page* page;
    list_for_every_entry (&free_list_, page, vm_page, queue_node) {
        FreeFill(page);
    }

    enforce_fill_ = true;
}

void PmmNode::FreeFill(vm_page_t* page) {
    void* kvaddr = paddr_to_physmap(page->paddr());
    DEBUG_ASSERT(is_kernel_address((vaddr_t)kvaddr));
    memset(kvaddr, PMM_FREE_FILL_BYTE, PAGE_SIZE);
}

void PmmNode::CheckFreeFill(vm_page_t* page) {
    uint8_t* kvaddr = static_cast<uint8_t*>(paddr_to_physmap(page->paddr()));
    for (size_t j = 0; j < PAGE_SIZE; ++j) {
        ASSERT(!enforce_fill_ || *(kvaddr + j) == PMM_FREE_FILL_BYTE);
    }
}
#endif // PMM_ENABLE_FREE_FILL
