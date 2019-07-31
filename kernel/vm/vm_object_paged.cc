// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "vm/vm_object_paged.h"

#include "vm_priv.h"

#include <arch/ops.h>
#include <assert.h>
#include <err.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>
#include <inttypes.h>
#include <ktl/move.h>
#include <lib/console.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>
#include <vm/bootreserve.h>
#include <vm/fault.h>
#include <vm/page_source.h>
#include <vm/physmap.h>
#include <vm/vm.h>
#include <vm/vm_address_region.h>
#include <zircon/types.h>

#define LOCAL_TRACE MAX(VM_GLOBAL_TRACE, 0)

namespace {

void ZeroPage(paddr_t pa) {
    void* ptr = paddr_to_physmap(pa);
    DEBUG_ASSERT(ptr);

    arch_zero_page(ptr);
}

void ZeroPage(vm_page_t* p) {
    paddr_t pa = p->paddr();
    ZeroPage(pa);
}

void InitializeVmPage(vm_page_t* p) {
    DEBUG_ASSERT(p->state() == VM_PAGE_STATE_ALLOC);
    p->set_state(VM_PAGE_STATE_OBJECT);
    p->object.pin_count = 0;
    p->object.cow_left_split = 0;
    p->object.cow_right_split = 0;
}

// Allocates a new page and populates it with the data at |parent_paddr|.
bool AllocateCopyPage(uint32_t pmm_alloc_flags, paddr_t parent_paddr,
                      list_node_t* free_list, vm_page_t** clone) {
    paddr_t pa_clone;
    vm_page_t* p_clone = nullptr;
    if (free_list) {
        p_clone = list_remove_head_type(free_list, vm_page, queue_node);
        if (p_clone) {
            pa_clone = p_clone->paddr();
        }
    }
    if (!p_clone) {
        zx_status_t status = pmm_alloc_page(pmm_alloc_flags, &p_clone, &pa_clone);
        if (!p_clone) {
            DEBUG_ASSERT(status == ZX_ERR_NO_MEMORY);
            return false;
        }
        DEBUG_ASSERT(status == ZX_OK);
    }

    InitializeVmPage(p_clone);

    void* dst = paddr_to_physmap(pa_clone);
    DEBUG_ASSERT(dst);

    if (parent_paddr != vm_get_zero_page_paddr()) {
        // do a direct copy of the two pages
        const void* src = paddr_to_physmap(parent_paddr);
        DEBUG_ASSERT(src);
        memcpy(dst, src, PAGE_SIZE);
    } else {
        // avoid pointless fetches by directly zeroing dst
        arch_zero_page(dst);
    }

    *clone = p_clone;

    return true;
}

// round up the size to the next page size boundary and make sure we dont wrap
zx_status_t RoundSize(uint64_t size, uint64_t* out_size) {
    *out_size = ROUNDUP_PAGE_SIZE(size);
    if (*out_size < size) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    // there's a max size to keep indexes within range
    if (*out_size > VmObjectPaged::MAX_SIZE) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    return ZX_OK;
}

} // namespace

VmObjectPaged::VmObjectPaged(
    uint32_t options, uint32_t pmm_alloc_flags, uint64_t size,
    fbl::RefPtr<vm_lock_t> root_lock, fbl::RefPtr<PageSource> page_source)
    : VmObject(ktl::move(root_lock)),
      options_(options),
      size_(size),
      pmm_alloc_flags_(pmm_alloc_flags),
      page_source_(ktl::move(page_source)) {
    LTRACEF("%p\n", this);

    DEBUG_ASSERT(IS_PAGE_ALIGNED(size_));

    // Adding to the global list needs to be done at the end of the ctor, since
    // calls can be made into this object as soon as it is in that list.
    AddToGlobalList();
}

void VmObjectPaged::InitializeOriginalParentLocked(fbl::RefPtr<VmObject> parent, uint64_t offset) {
    DEBUG_ASSERT(lock_.lock().IsHeld());
    DEBUG_ASSERT(parent_ == nullptr);
    DEBUG_ASSERT(original_parent_user_id_ == 0);

    if (parent->is_paged()) {
        page_list_.InitializeSkew(
                VmObjectPaged::AsVmObjectPaged(parent)->page_list_.GetSkew(), offset);
    }

    original_parent_user_id_ = parent->user_id_locked();
    parent_ = ktl::move(parent);
}

VmObjectPaged::~VmObjectPaged() {
    canary_.Assert();

    LTRACEF("%p\n", this);

    RemoveFromGlobalList();

    if (!is_hidden()) {
        // If we're not a hidden vmo, then we need to remove ourself from our parent. This needs
        // to be done before emptying the page list so that a hidden parent can't merge into this
        // vmo and repopulate the page list.
        //
        // To prevent races with a hidden parent merging itself into this vmo, it is necessary
        // to hold the lock over the parent_ check and into the subsequent removal call.
        Guard<fbl::Mutex> guard{&lock_};
        if (parent_) {
            LTRACEF("removing ourself from our parent %p\n", parent_.get());
            parent_->RemoveChild(this, guard.take());
        }
    } else {
        // Most of the hidden vmo's state should have already been cleaned up when it merged
        // itself into its child in ::OnChildRemoved.
        DEBUG_ASSERT(children_list_len_ == 0);
        DEBUG_ASSERT(page_list_.IsEmpty());
    }

    page_list_.ForEveryPage(
        [this](const auto p, uint64_t off) {
            if (this->is_contiguous()) {
                p->object.pin_count--;
            }
            ASSERT(p->object.pin_count == 0);
            return ZX_ERR_NEXT;
        });

    list_node_t list;
    list_initialize(&list);

    // free all of the pages attached to us
    page_list_.RemoveAllPages(&list);

    if (page_source_) {
        page_source_->Close();
    }

    pmm_free(&list);
}

zx_status_t VmObjectPaged::CreateCommon(uint32_t pmm_alloc_flags,
                                        uint32_t options,
                                        uint64_t size, fbl::RefPtr<VmObject>* obj) {
    // make sure size is page aligned
    zx_status_t status = RoundSize(size, &size);
    if (status != ZX_OK) {
        return status;
    }

    fbl::AllocChecker ac;
    auto lock = fbl::AdoptRef<vm_lock_t>(new (&ac) vm_lock_t);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    auto vmo = fbl::AdoptRef<VmObject>(
        new (&ac) VmObjectPaged(options, pmm_alloc_flags, size, ktl::move(lock), nullptr));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    *obj = ktl::move(vmo);

    return ZX_OK;
}

zx_status_t VmObjectPaged::Create(uint32_t pmm_alloc_flags,
                                  uint32_t options,
                                  uint64_t size, fbl::RefPtr<VmObject>* obj) {
    if (options & kContiguous) {
        // Force callers to use CreateContiguous() instead.
        return ZX_ERR_INVALID_ARGS;
    }

    return CreateCommon(pmm_alloc_flags, options, size, obj);
}

zx_status_t VmObjectPaged::CreateContiguous(uint32_t pmm_alloc_flags, uint64_t size,
                                            uint8_t alignment_log2, fbl::RefPtr<VmObject>* obj) {
    DEBUG_ASSERT(alignment_log2 < sizeof(uint64_t) * 8);
    // make sure size is page aligned
    zx_status_t status = RoundSize(size, &size);
    if (status != ZX_OK) {
        return status;
    }

    fbl::RefPtr<VmObject> vmo;
    status = CreateCommon(pmm_alloc_flags, kContiguous, size, &vmo);
    if (status != ZX_OK) {
        return status;
    }

    if (size == 0) {
        *obj = ktl::move(vmo);
        return ZX_OK;
    }

    // allocate the pages
    list_node page_list;
    list_initialize(&page_list);

    size_t num_pages = size / PAGE_SIZE;
    paddr_t pa;
    status = pmm_alloc_contiguous(num_pages, pmm_alloc_flags, alignment_log2, &pa, &page_list);
    if (status != ZX_OK) {
        LTRACEF("failed to allocate enough pages (asked for %zu)\n", num_pages);
        return ZX_ERR_NO_MEMORY;
    }
    auto cleanup_phys_pages = fbl::MakeAutoCall([&page_list]() {
        pmm_free(&page_list);
    });

    // add them to the appropriate range of the object
    VmObjectPaged* vmop = static_cast<VmObjectPaged*>(vmo.get());
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        vm_page_t* p = list_remove_head_type(&page_list, vm_page_t, queue_node);
        ASSERT(p);

        InitializeVmPage(p);

        // TODO: remove once pmm returns zeroed pages
        ZeroPage(p);

        // We don't need thread-safety analysis here, since this VMO has not
        // been shared anywhere yet.
        [&]() TA_NO_THREAD_SAFETY_ANALYSIS {
            status = vmop->page_list_.AddPage(p, off);
        }();
        if (status != ZX_OK) {
            return status;
        }

        // Mark the pages as pinned, so they can't be physically rearranged
        // underneath us.
        p->object.pin_count++;
    }

    cleanup_phys_pages.cancel();
    *obj = ktl::move(vmo);
    return ZX_OK;
}

zx_status_t VmObjectPaged::CreateFromWiredPages(const void* data, size_t size, bool exclusive,
                                                fbl::RefPtr<VmObject>* obj) {
    LTRACEF("data %p, size %zu\n", data, size);

    fbl::RefPtr<VmObject> vmo;
    zx_status_t status = CreateCommon(PMM_ALLOC_FLAG_ANY, 0, size, &vmo);
    if (status != ZX_OK) {
        return status;
    }

    if (size > 0) {
        ASSERT(IS_PAGE_ALIGNED(size));
        ASSERT(IS_PAGE_ALIGNED(reinterpret_cast<uintptr_t>(data)));

        // Do a direct lookup of the physical pages backing the range of
        // the kernel that these addresses belong to and jam them directly
        // into the VMO.
        //
        // NOTE: This relies on the kernel not otherwise owning the pages.
        // If the setup of the kernel's address space changes so that the
        // pages are attached to a kernel VMO, this will need to change.

        paddr_t start_paddr = vaddr_to_paddr(data);
        ASSERT(start_paddr != 0);

        for (size_t count = 0; count < size / PAGE_SIZE; count++) {
            paddr_t pa = start_paddr + count * PAGE_SIZE;
            vm_page_t* page = paddr_to_vm_page(pa);
            ASSERT(page);

            if (page->state() == VM_PAGE_STATE_WIRED) {
                boot_reserve_unwire_page(page);
            } else {
                // This function is only valid for memory in the boot image,
                // which should all be wired.
                panic("page used to back static vmo in unusable state: paddr %#" PRIxPTR
                      " state %u\n", pa, page->state());
            }
            InitializeVmPage(page);

            // XXX hack to work around the ref pointer to the base class
            auto vmo2 = static_cast<VmObjectPaged*>(vmo.get());
            vmo2->AddPage(page, count * PAGE_SIZE);
        }
    }

    if (exclusive && !is_physmap_addr(data)) {
        // unmap it from the kernel
        // NOTE: this means the image can no longer be referenced from original pointer
        status = VmAspace::kernel_aspace()->arch_aspace().Unmap(
                reinterpret_cast<vaddr_t>(data), size / PAGE_SIZE, nullptr);
        ASSERT(status == ZX_OK);
    }

    *obj = ktl::move(vmo);

    return ZX_OK;
}

zx_status_t VmObjectPaged::CreateExternal(fbl::RefPtr<PageSource> src, uint32_t options,
                                          uint64_t size, fbl::RefPtr<VmObject>* obj) {
    // make sure size is page aligned
    zx_status_t status = RoundSize(size, &size);
    if (status != ZX_OK) {
        return status;
    }

    fbl::AllocChecker ac;
    auto lock = fbl::AdoptRef<vm_lock_t>(new (&ac) vm_lock_t);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    auto vmo = fbl::AdoptRef<VmObject>(new (&ac) VmObjectPaged(
            options, PMM_ALLOC_FLAG_ANY, size, ktl::move(lock), ktl::move(src)));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    *obj = ktl::move(vmo);

    return ZX_OK;
}

void VmObjectPaged::InsertHiddenParentLocked(fbl::RefPtr<VmObjectPaged>&& hidden_parent) {
    // Insert the new VmObject |hidden_parent| between between |this| and |parent_|.
    if (parent_) {
        hidden_parent->InitializeOriginalParentLocked(parent_, 0);
        parent_->ReplaceChildLocked(this, hidden_parent.get());
    }
    hidden_parent->AddChildLocked(this);
    parent_ = hidden_parent;

    // We use the user_id to walk the tree looking for the right child observer. This
    // is set after adding the hidden parent into the tree since that's not really
    // a 'real' child.
    hidden_parent->user_id_ = user_id_;
    hidden_parent->page_attribution_user_id_ = user_id_;

    // The hidden parent should have the same view as we had into
    // its parent, and this vmo has a full view into the hidden vmo
    hidden_parent->parent_offset_ = parent_offset_;
    hidden_parent->parent_limit_ = parent_limit_;
    parent_offset_ = 0;
    parent_limit_ = size_;

    // This method should only ever be called on leaf vmos (i.e. non-hidden),
    // so this flag should never be set.
    DEBUG_ASSERT(!partial_cow_release_);
    DEBUG_ASSERT(parent_start_limit_ == 0); // Should only ever be set for hidden vmos

    // Move everything into the hidden parent, for immutability
    hidden_parent->page_list_ = std::move(page_list_);
    hidden_parent->size_ = size_;
}

zx_status_t VmObjectPaged::CreateChildSlice(uint64_t offset, uint64_t size, bool copy_name,
                                             fbl::RefPtr<VmObject>* child_vmo) {
    LTRACEF("vmo %p offset %#" PRIx64 " size %#" PRIx64 "\n", this, offset, size);

    canary_.Assert();

    // Offset must be page aligned.
    if (!IS_PAGE_ALIGNED(offset)) {
        return ZX_ERR_INVALID_ARGS;
    }

    // Make sure size is page aligned.
    zx_status_t status = RoundSize(size, &size);
    if (status != ZX_OK) {
        return status;
    }

    // Slice must be wholly contained.
    uint64_t our_size;
    {
        // size_ is not an atomic variable and although it should not be changing, as we are not
        // allowing this operation on resizable vmo's, we should still be holding the lock to
        // correctly read size_. Unfortunately we must also drop then drop the lock in order to
        // perform the allocation.
        Guard<fbl::Mutex> guard{&lock_};
        our_size = size_;
    }
    if (!InRange(offset, size, our_size)) {
        return ZX_ERR_INVALID_ARGS;
    }

    // Forbid creating children of resizable VMOs. This restriction may be lifted in the future.
    if (is_resizable()) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    // There are two reasons for declaring/allocating the clones outside of the vmo's lock. First,
    // the dtor might require taking the lock, so we need to ensure that it isn't called until
    // after the lock is released. Second, diagnostics code makes calls into vmos while holding
    // the global vmo lock. Since the VmObject ctor takes the global lock, we can't construct
    // any vmos under any vmo lock.
    fbl::AllocChecker ac;
    auto vmo = fbl::AdoptRef<VmObjectPaged>(new (&ac) VmObjectPaged(
        kSlice, pmm_alloc_flags_, size, lock_ptr_, nullptr));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    bool notify_one_child;
    {
        Guard<fbl::Mutex> guard{&lock_};

        // If this VMO is contiguous then we allow creating an uncached slice as we will never
        // have to perform zeroing of pages. Pages will never be zeroed since contiguous VMOs have
        // all of their pages allocated (and so COW of the zero page will never happen). The VMO is
        // also not allowed to be resizable and so will never have to allocate new pages (and zero
        // them).
        if (cache_policy_ != ARCH_MMU_FLAG_CACHED && !is_contiguous()) {
            return ZX_ERR_BAD_STATE;
        }
        vmo->cache_policy_ = cache_policy_;
        vmo->parent_offset_ = offset;
        vmo->parent_limit_ = size;

        vmo->InitializeOriginalParentLocked(fbl::WrapRefPtr(this), offset);

        // add the new vmo as a child before we do anything, since its
        // dtor expects to find it in its parent's child list
        notify_one_child = AddChildLocked(vmo.get());

        if (copy_name) {
            vmo->name_ = name_;
        }
    }

    if (notify_one_child) {
        NotifyOneChild();
    }

    *child_vmo = ktl::move(vmo);

    return ZX_OK;
}

zx_status_t VmObjectPaged::CreateCowClone(Resizability resizable, CloneType type,
                                          uint64_t offset, uint64_t size,
                                          bool copy_name, fbl::RefPtr<VmObject>* child_vmo) {
    LTRACEF("vmo %p offset %#" PRIx64 " size %#" PRIx64 "\n", this, offset, size);

    canary_.Assert();

    // offset must be page aligned
    if (!IS_PAGE_ALIGNED(offset)) {
        return ZX_ERR_INVALID_ARGS;
    }

    // make sure size is page aligned
    zx_status_t status = RoundSize(size, &size);
    if (status != ZX_OK) {
        return status;
    }

    auto options = resizable == Resizability::Resizable ? kResizable : 0u;
    // There are two reasons for declaring/allocating the clones outside of the vmo's lock. First,
    // the dtor might require taking the lock, so we need to ensure that it isn't called until
    // after the lock is released. Second, diagnostics code makes calls into vmos while holding
    // the global vmo lock. Since the VmObject ctor takes the global lock, we can't construct
    // any vmos under any vmo lock.
    fbl::AllocChecker ac;
    auto vmo = fbl::AdoptRef<VmObjectPaged>(new (&ac) VmObjectPaged(
        options, pmm_alloc_flags_, size, lock_ptr_, nullptr));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    fbl::RefPtr<VmObjectPaged> hidden_parent;
    if (type == CloneType::Bidirectional) {
        // To create a bidirectional clone, the kernel creates an artifical parent vmo
        // called a 'hidden vmo'. The content of the original vmo is moved into the hidden
        // vmo, and the original vmo becomes a child of the hidden vmo. Then a second child
        // is created, which is the userspace visible clone.
        //
        // Hidden vmos are an implementation detail that are not exposed to userspace.

        if (!IsBidirectionalClonable()) {
            return ZX_ERR_NOT_SUPPORTED;
        }

        uint32_t options = kHidden;
        if (is_contiguous()) {
            options |= kContiguous;
        }

        // The initial size is 0. It will be initialized as part of the atomic
        // insertion into the child tree.
        hidden_parent = fbl::AdoptRef<VmObjectPaged>(new (&ac) VmObjectPaged(
                options, pmm_alloc_flags_, 0, lock_ptr_, nullptr));
        if (!ac.check()) {
            return ZX_ERR_NO_MEMORY;
        }
    }

    bool notify_one_child;
    {
        Guard<fbl::Mutex> guard{&lock_};

        // check that we're not uncached in some way
        if (cache_policy_ != ARCH_MMU_FLAG_CACHED) {
            return ZX_ERR_BAD_STATE;
        }

        // TODO: ZX-692 make sure that the accumulated parent offset of the entire
        // parent chain doesn't wrap 64bit space.
        vmo->parent_offset_ = offset;
        vmo->parent_limit_ = fbl::min(size, size_ - offset);

        VmObjectPaged* clone_parent;
        if (type == CloneType::Bidirectional) {
            clone_parent = hidden_parent.get();

            InsertHiddenParentLocked(ktl::move(hidden_parent));

            // Invalidate everything the clone will be able to see. They're COW pages now,
            // so any existing mappings can no longer directly write to the pages.
            // TODO: Just change the mappings to RO instead of fully unmapping.
            RangeChangeUpdateLocked(vmo->parent_offset_, vmo->parent_offset_ + vmo->parent_limit_);
        } else  {
            clone_parent = this;
        }

        vmo->InitializeOriginalParentLocked(fbl::WrapRefPtr(clone_parent), offset);

        // add the new vmo as a child before we do anything, since its
        // dtor expects to find it in its parent's child list
        notify_one_child = clone_parent->AddChildLocked(vmo.get());

        if (copy_name) {
            vmo->name_ = name_;
        }
    }

    if (notify_one_child) {
        NotifyOneChild();
    }

    *child_vmo = ktl::move(vmo);

    return ZX_OK;
}

bool VmObjectPaged::OnChildAddedLocked() {
    if (!is_hidden()) {
        return VmObject::OnChildAddedLocked();
    }

    if (user_id_ == ZX_KOID_INVALID) {
        // The original vmo is added as a child of the hidden vmo before setting
        // the user id to prevent counting as its own child.
        return false;
    }

    // After initialization, hidden vmos always have two children - the vmo on which
    // zx_vmo_create_child was invoked and the vmo which that syscall created.
    DEBUG_ASSERT(children_list_len_ == 2);

    // We need to proxy the child add to the original vmo so that
    // it can update it's clone count.
    return [&]() TA_NO_THREAD_SAFETY_ANALYSIS -> bool {
        // Reaching into the children confuses analysis
        for (auto& c : children_list_) {
            if (c.user_id_ == user_id_) {
                return c.OnChildAddedLocked();
            }
        }
        // One of the children should always have a matching user_id.
        panic("no child with matching user_id: %" PRIx64 "\n", user_id_);
    }();
}

void VmObjectPaged::RemoveChild(VmObjectPaged* removed, Guard<fbl::Mutex>&& adopt) {
    if (!is_hidden()) {
        VmObject::RemoveChild(removed, adopt.take());
        return;
    }

    DEBUG_ASSERT(adopt.wraps_lock(lock_ptr_->lock.lock()));
    Guard<fbl::Mutex> guard{AdoptLock, ktl::move(adopt)};

    // Hidden vmos always have 0 or 2 children, but we can't be here with 0 children.
    DEBUG_ASSERT(children_list_len_ == 2);
    // A hidden vmo must be fully initialized to have 2 children.
    DEBUG_ASSERT(user_id_ != ZX_KOID_INVALID);
    bool removed_left = &left_child_locked() == removed;

    DropChildLocked(removed);
    auto& child = children_list_.front();

    // Merge this vmo's content into the remaining child.
    MergeContentWithChildLocked(removed, removed_left);

    // The child which removed itself and led to the invocation should have a reference
    // to us, in addition to child.parent_ which we are about to clear.
    DEBUG_ASSERT(ref_count_debug() >= 2);

    if (child.page_attribution_user_id_ != page_attribution_user_id_) {
        // If the attribution user id of this vmo doesn't match that of its remaining child,
        // then the vmo with the matching attribution user id  was just closed. In that case, we
        // need to reattribute the pages of any ancestor hidden vmos to vmos that still exist.
        //
        // The syscall API doesn't specify how pages are to be attributed among a group of COW
        // clones. One option is to pick a remaining vmo 'arbitrarily' and attribute everything to
        // that vmo. However, it seems fairer to reattribute each remaining hidden vmo with
        // its child whose user id doesn't match the vmo that was just closed. So walk up the
        // clone chain and attribute each hidden vmo to the vmo we didn't just walk through.
        auto cur = this;
        uint64_t user_id_to_skip = page_attribution_user_id_;
        while (cur->parent_ != nullptr) {
            DEBUG_ASSERT(cur->parent_->is_hidden());
            auto parent = VmObjectPaged::AsVmObjectPaged(cur->parent_);

            if (parent->page_attribution_user_id_ == page_attribution_user_id_) {
                uint64_t new_user_id = parent->left_child_locked().page_attribution_user_id_;
                if (new_user_id == user_id_to_skip) {
                    new_user_id = parent->right_child_locked().page_attribution_user_id_;
                }
                DEBUG_ASSERT(new_user_id != page_attribution_user_id_
                             && new_user_id != user_id_to_skip);
                parent->page_attribution_user_id_ = new_user_id;
                user_id_to_skip = new_user_id;

                cur = parent;
            } else {
                break;
            }
        }
    }

    // Drop the child from our list, but don't recurse back into this function. Then
    // remove ourselves from the clone tree.
    DropChildLocked(&child);
    if (parent_) {
        parent_->ReplaceChildLocked(this, &child);
    }
    child.parent_ = std::move(parent_);

    // We need to proxy the closure down to the original user-visible vmo. To find
    // that, we can walk down the clone tree following the user_id_.
    VmObjectPaged* descendant = &child;
    while (descendant && descendant->user_id_ == user_id_) {
        if (!descendant->is_hidden()) {
            descendant->OnUserChildRemoved(guard.take());
            return;
        }
        if (descendant->left_child_locked().user_id_ == user_id_) {
            descendant = &descendant->left_child_locked();
        } else if (descendant->right_child_locked().user_id_ == user_id_) {
            descendant = &descendant->right_child_locked();
        } else {
            descendant = nullptr;
        }
    }
}

void VmObjectPaged::MergeContentWithChildLocked(VmObjectPaged* removed, bool removed_left) {
    DEBUG_ASSERT(children_list_len_ == 1);
    auto& child = children_list_.front();

    list_node freed_pages;
    list_initialize(&freed_pages);

    const uint64_t visibility_start_offset = child.parent_offset_ + child.parent_start_limit_;
    const uint64_t merge_start_offset = child.parent_offset_;
    const uint64_t merge_end_offset = child.parent_offset_ + child.parent_limit_;

    page_list_.RemovePages(0, visibility_start_offset, &freed_pages);
    page_list_.RemovePages(merge_end_offset, MAX_SIZE, &freed_pages);

    if (child.parent_offset_ + child.parent_limit_ > parent_limit_) {
        // Update the child's parent limit to ensure that it won't be able to see more
        // of its new parent than this hidden vmo was able to see.
        if (parent_limit_ < child.parent_offset_) {
            child.parent_limit_ = 0;
            child.parent_start_limit_ = 0;
        } else {
            child.parent_limit_ = parent_limit_ - child.parent_offset_;
            child.parent_start_limit_ = fbl::min(child.parent_start_limit_, child.parent_limit_);
        }
    } else {
        // The child will be able to see less of its new parent than this hidden vmo was
        // able to see, so release any parent pages in that range.
        ReleaseCowParentPagesLocked(merge_end_offset, parent_limit_, &freed_pages);
    }

    if (removed->parent_offset_ + removed->parent_start_limit_ < visibility_start_offset) {
        // If the removed former child has a smaller offset, then there are retained
        // ancestor pages that will no longer be visible and thus should be freed.
        ReleaseCowParentPagesLocked(removed->parent_offset_ + removed->parent_start_limit_,
                                    visibility_start_offset, &freed_pages);
    }

    // Adjust the child's offset so it will still see the correct range.
    bool overflow = add_overflow(parent_offset_, child.parent_offset_, &child.parent_offset_);
    // Overflow here means that something went wrong when setting up parent limits.
    DEBUG_ASSERT(!overflow);

    if (child.is_hidden()) {
        // After the merge, either |child| can't see anything in parent (in which case
        // the parent limits could be anything), or |child|'s first visible offset will be
        // at least as large as |this|'s first visible offset.
        DEBUG_ASSERT(child.parent_start_limit_ == child.parent_limit_
                || parent_offset_ + parent_start_limit_
                        <= child.parent_offset_ + child.parent_start_limit_);
    } else {
        // non-hidden vmos should always have zero parent_start_limit_
        DEBUG_ASSERT(child.parent_start_limit_ == 0);
    }

    if (is_contiguous()) {
        vm_page_t* p;
        list_for_every_entry(&freed_pages, p, vm_page_t, queue_node) {
            // The pages that have been freed all come from contigous hidden vmos, so they can
            // either be contiguously pinned or have been migrated into their other child.
            DEBUG_ASSERT(p->object.pin_count <= 1);
            p->object.pin_count = 0;
        }
    }

    // At this point, we need to merge |this|'s page list and |child|'s page list.
    //
    // In general, COW clones are expected to share most of their pages (i.e. to fork a relatively
    // small number of pages). Because of this, it is preferable to do work proportional to the
    // number of pages which were forked into |removed|. However, there are a few things that can
    // prevent this:
    //   - If |child|'s offset is non-zero then the offsets of all of |this|'s pages will
    //     need to be updated when they are merged into |child|.
    //   - If |this| is contiguous and |child| is not, then all of |this|'s page's pin
    //     counts will need to be updated when they are migrated into |child|.
    //   - If there has been a call to ReleaseCowParentPagesLocked which was not able to
    //     update the parent limits, then there can exist pages in this vmo's page list
    //     which are not visible to |child| but can't be easily freed based on its parent
    //     limits. Finding these pages requires examining the split bits of all pages.
    //   - If |child| is hidden, then there can exist pages in this vmo which were split into
    //     |child|'s subtree and then migrated out of |child|. Those pages need to be freed, and
    //     the simplest way to find those pages is to examine the split bits.
    bool fast_merge = merge_start_offset == 0
            && !(is_contiguous() && !child.is_contiguous())
            && !partial_cow_release_
            && !child.is_hidden();

    if (fast_merge) {
        // Only leaf vmos can be directly removed, so this must always be true. This guarantees
        // that there are no pages that were split into |removed| that have since been migrated
        // to its children.
        DEBUG_ASSERT(!removed->is_hidden());

        // Before merging, find any pages that are present in both |removed| and |this|. Those
        // pages are visibile to |child| but haven't been written to through |child|, so
        // their split bits need to be cleared. Note that ::ReleaseCowParentPagesLocked ensures
        // that pages outside of the parent limit range won't have their split bits set.
        removed->page_list_.ForEveryPageInRange(
                [removed_offset = removed->parent_offset_, this](vm_page_t* page, uint64_t offset)
                // Analysis needs to be disabled since the main function has no information.
                TA_NO_THREAD_SAFETY_ANALYSIS {
            vm_page_t* p_page = page_list_.GetPage(offset + removed_offset);
            if (p_page) {
                // The page is definitely forked into |removed|, but shouldn't be forked twice.
                DEBUG_ASSERT(p_page->object.cow_left_split ^ p_page->object.cow_right_split);
                p_page->object.cow_left_split = 0;
                p_page->object.cow_right_split = 0;
            }
            return ZX_ERR_NEXT;
        }, removed->parent_start_limit_, removed->parent_limit_);

        list_node covered_pages;
        list_initialize(&covered_pages);

        // Now merge |child|'s pages into |this|, overwriting any pages present in |this|, and
        // then move that list to |child|.
        child.page_list_.MergeOnto(page_list_, &covered_pages);
        child.page_list_ = ktl::move(page_list_);

#ifdef DEBUG_ASSERT_IMPLEMENTED
        vm_page_t* p;
        list_for_every_entry(&covered_pages, p, vm_page_t, queue_node) {
            // The page was already present in |child|, so it should be split at least
            // once. And being split twice is obviously bad.
            ASSERT(p->object.cow_left_split ^ p->object.cow_right_split);
            // If |this| is contig, then we're only here if |child| is also contig. In that
            // case, any covered pages must be covered by the original contig page in |child|
            // and must be unpinned themselves.
            ASSERT(p->object.pin_count == 0);
        }
#endif
        list_splice_after(&covered_pages, &freed_pages);
    } else {
        // Merge our page list into the child page list and update all the necessary metadata.
        child.page_list_.MergeFrom(page_list_,
                merge_start_offset, merge_end_offset,
                [this_is_contig = this->is_contiguous()](vm_page* page, uint64_t offset) {
                    if (this_is_contig) {
                        // If this vmo is contiguous, unpin the pages that aren't needed. The pages
                        // are either original contig pages (which should have a pin_count of 1),
                        // or they're forked pages where the original is already in the contig
                        // child (in which case pin_count should be 0).
                        DEBUG_ASSERT(page->object.pin_count <= 1);
                        page->object.pin_count = 0;
                    }
                },
                [this_is_contig = this->is_contiguous(), child_is_contig = child.is_contiguous(),
                 removed_left](vm_page* page, uint64_t offset) -> bool {
                    if (child_is_contig) {
                        // We moved the page into the contiguous vmo, so we expect the page
                        // to be a pinned contiguous page.
                        DEBUG_ASSERT(page->object.pin_count == 1);
                    } else if (this_is_contig) {
                        // This vmo was contiguous but the child isn't, so unpin the pages. Similar
                        // to above, this should be at most 1.
                        DEBUG_ASSERT(page->object.pin_count <= 1);
                        page->object.pin_count = 0;
                    } else {
                        // Neither is contiguous, so the page shouldn't have been pinned.
                        DEBUG_ASSERT(page->object.pin_count == 0);
                    }

                    if (removed_left ? page->object.cow_right_split : page->object.cow_left_split) {
                        // This happens when the pages was already migrated into child but then
                        // was migrated further into child's descendants. The page can be freed.
                        return false;
                    } else {
                        // Since we recursively fork on write, if the child doesn't have the
                        // page, then neither of its children do.
                        page->object.cow_left_split = 0;
                        page->object.cow_right_split = 0;
                        return true;
                    }
                }, &freed_pages);
    }

    if (!list_is_empty(&freed_pages)) {
        pmm_free(&freed_pages);
    }
}

void VmObjectPaged::Dump(uint depth, bool verbose) {
    canary_.Assert();

    // This can grab our lock.
    uint64_t parent_id = parent_user_id();

    Guard<fbl::Mutex> guard{&lock_};

    size_t count = 0;
    page_list_.ForEveryPage([&count](const auto p, uint64_t) {
        count++;
        return ZX_ERR_NEXT;
    });

    for (uint i = 0; i < depth; ++i) {
        printf("  ");
    }
    printf("vmo %p/k%" PRIu64 " size %#" PRIx64 " offset %#" PRIx64
           " limit %#" PRIx64 " pages %zu ref %d parent %p/k%" PRIu64 "\n",
           this, user_id_, size_, parent_offset_, parent_limit_, count,
           ref_count_debug(), parent_.get(), parent_id);

    if (verbose) {
        auto f = [depth](const auto p, uint64_t offset) {
            for (uint i = 0; i < depth + 1; ++i) {
                printf("  ");
            }
            printf("offset %#" PRIx64 " page %p paddr %#" PRIxPTR "\n", offset, p, p->paddr());
            return ZX_ERR_NEXT;
        };
        page_list_.ForEveryPage(f);
    }
}

size_t VmObjectPaged::AttributedPagesInRange(uint64_t offset, uint64_t len) const {
    canary_.Assert();
    Guard<fbl::Mutex> guard{&lock_};
    return AttributedPagesInRangeLocked(offset, len);
}

size_t VmObjectPaged::AttributedPagesInRangeLocked(uint64_t offset, uint64_t len) const {
    if (is_hidden()) {
        return 0;
    }

    uint64_t new_len;
    if (!TrimRange(offset, len, size_, &new_len)) {
        return 0;
    }
    size_t count = 0;
    // TODO: Decide who pages should actually be attribtued to.
    page_list_.ForEveryPageAndGapInRange(
        [&count](const auto p, uint64_t off) {
            count++;
            return ZX_ERR_NEXT;
        },
        [this, &count](uint64_t gap_start, uint64_t gap_end) TA_NO_THREAD_SAFETY_ANALYSIS {
            // If there's no parent, there's no pages to care about. If there is a non-hidden
            // parent, then that owns any pages in the gap, not us.
            if (!parent_ || !parent_->is_hidden()) {
                return ZX_ERR_NEXT;
            }

            for (uint64_t off = gap_start; off < gap_end; off += PAGE_SIZE) {
                if (HasAttributedAncestorPageLocked(off)) {
                    count++;
                }
            }
            return ZX_ERR_NEXT;
        }, offset, offset + new_len);

    return count;
}

bool VmObjectPaged::HasAttributedAncestorPageLocked(uint64_t offset) const {
    // For each offset, walk up the ancestor chain to see if there is a page at that offset
    // that should be attributed to this vmo.
    //
    // Note that we cannot stop just because the page_attribution_user_id_ changes. This is because
    // there might still be a forked page at the offset in question which should be attributed to
    // this vmo. Whenever the attribution user id changes while walking up the ancestors, we need
    // to determine if there is a 'closer' vmo in the sibling subtree to which the offset in
    // question can be attributed, or if it should still be attributed to the current vmo.
    const VmObjectPaged* cur = this;
    uint64_t cur_offset = offset;
    vm_page_t* page = nullptr;
    while (cur_offset < cur->parent_limit_) {
        // For cur->parent_limit_ to be non-zero, it must have a parent.
        DEBUG_ASSERT(cur->parent_);
        DEBUG_ASSERT(cur->parent_->is_paged());

        auto parent = VmObjectPaged::AsVmObjectPaged(cur->parent_);
        uint64_t parent_offset;
        bool overflowed = add_overflow(cur->parent_offset_, cur_offset, &parent_offset);
        DEBUG_ASSERT(!overflowed); // vmo creation should have failed
        DEBUG_ASSERT(parent_offset <= parent->size_); // parent_limit_ prevents this

        page = parent->page_list_.GetPage(parent_offset);
        if (parent->page_attribution_user_id_ != cur->page_attribution_user_id_) {
            bool left = cur == &parent->left_child_locked();

            if (page && (page->object.cow_left_split || page->object.cow_right_split)) {
                // If page has already been split and we can see it, then we know
                // the sibling subtree can't see the page and thus it should be
                // attributed to this vmo.
                return true;
            } else {
                auto& sib = left ?  parent->right_child_locked() : parent->left_child_locked();
                DEBUG_ASSERT(sib.page_attribution_user_id_ == parent->page_attribution_user_id_);
                if (sib.parent_offset_ + sib.parent_start_limit_ <= parent_offset
                        && parent_offset < sib.parent_offset_ + sib.parent_limit_) {
                    // The offset is visible to the current vmo, so there can't be any migrated
                    // pages in the sibling subtree corresponding to the offset. And since the page
                    // is visible to the sibling, there must be a leaf vmo in its subtree which
                    // can actually see the offset.
                    //
                    // Therefore we know that there is a vmo in the sibling subtree which is
                    // 'closer' to this offset and thus to which this offset can be attributed.
                    return false;
                } else {
                    if (page) {
                        // If there is a page and it's not accessible by the sibling,
                        // then it is attributed to |this|.
                        return true;
                    } else {
                        // |sib| can't see the offset, but there might be a sibling further up
                        // the ancestor tree that can, so we have to keep looking.
                    }
                }
            }
        } else {
            // If there's a page, it is attributed to |this|. Otherwise keep looking.
            if (page) {
                return true;
            }
        }

        cur = parent;
        cur_offset = parent_offset;
    }

    // We didn't find a page at all, so nothing to attribute.
    return false;
}

zx_status_t VmObjectPaged::AddPage(vm_page_t* p, uint64_t offset) {
    Guard<fbl::Mutex> guard{&lock_};

    return AddPageLocked(p, offset);
}

zx_status_t VmObjectPaged::AddPageLocked(vm_page_t* p, uint64_t offset, bool do_range_update) {
    canary_.Assert();
    DEBUG_ASSERT(lock_.lock().IsHeld());

    LTRACEF("vmo %p, offset %#" PRIx64 ", page %p (%#" PRIxPTR ")\n", this, offset, p, p->paddr());

    DEBUG_ASSERT(p);

    if (offset >= size_) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    zx_status_t err = page_list_.AddPage(p, offset);
    if (err != ZX_OK) {
        return err;
    }

    if (do_range_update) {
        // other mappings may have covered this offset into the vmo, so unmap those ranges
        RangeChangeUpdateLocked(offset, PAGE_SIZE);
    }

    return ZX_OK;
}

bool VmObjectPaged::IsUniAccessibleLocked(vm_page_t* page, uint64_t offset) const {
    DEBUG_ASSERT(lock_.lock().IsHeld());
    DEBUG_ASSERT(page_list_.GetPage(offset) == page);

    if (page->object.cow_right_split || page->object.cow_left_split) {
        return true;
    }

    if (offset < left_child_locked().parent_offset_ + left_child_locked().parent_start_limit_
            || offset >= left_child_locked().parent_offset_ + left_child_locked().parent_limit_) {
        return true;
    }

    if (offset < right_child_locked().parent_offset_ + right_child_locked().parent_start_limit_
            || offset >= right_child_locked().parent_offset_ + right_child_locked().parent_limit_) {
        return true;
    }

    return false;
}

vm_page_t* VmObjectPaged::CloneCowPageLocked(uint64_t offset, list_node_t* free_list,
                                             VmObjectPaged* page_owner, vm_page_t* page,
                                             uint64_t owner_offset) {
    DEBUG_ASSERT(page != vm_get_zero_page());
    DEBUG_ASSERT(parent_);

    // To avoid the need for rollback logic on allocation failure, we start the forking
    // process from the root-most vmo and work our way towards the leaf vmo. This allows
    // us to maintain the hidden vmo invariants through the whole operation, so that we
    // can stop at any point.
    //
    // To set this up, walk from the leaf to |page_owner|, and keep track of the
    // path via |page_stack_flag_|.
    VmObjectPaged* cur = this;
    do {
        VmObjectPaged* next = VmObjectPaged::AsVmObjectPaged(cur->parent_);
        // We can't make COW clones of physical vmos, so this can only happen if we
        // somehow don't find |page_owner| in the ancestor chain.
        DEBUG_ASSERT(next);

        next->page_stack_flag_ =
                &next->left_child_locked() == cur ? StackDir::Left : StackDir::Right;
        if (next->page_stack_flag_ == StackDir::Right) {
            DEBUG_ASSERT(&next->right_child_locked() == cur);
        }
        cur = next;
    } while (cur != page_owner);
    uint64_t cur_offset = owner_offset;

    // |target_page| is the page we're considering for migration. Cache it
    // across loop iterations.
    vm_page_t* target_page = page;
    VmObjectPaged* last_contig = nullptr;
    uint64_t last_contig_offset = 0;

    bool alloc_failure = false;

    // As long as we're simply migrating |page|, there's no need to update any vmo mappings, since
    // that means the other side of the clone tree has already covered |page| and the current side
    // of the clone tree will still see |page|. As soon as we insert a new page, we'll need to
    // update all mappings at or below that level.
    bool skip_range_update = true;
    do {
        // |target_page| is always located at in |cur| at |cur_offset| at the start of the loop.
        VmObjectPaged* target_page_owner = cur;
        uint64_t target_page_offset = cur_offset;

        cur = cur->page_stack_flag_ == StackDir::Left ?
                &cur->left_child_locked() : &cur->right_child_locked();
        cur_offset -= cur->parent_offset_;

        if (target_page_owner->IsUniAccessibleLocked(target_page, target_page_offset)) {
            // If the page we're covering in the parent is uni-accessible, then we
            // can directly move the page.

            // Assert that we're not trying to split the page the same direction two times. Either
            // some tracking state got corrupted or a page in the subtree we're trying to
            // migrate to got improperly migrated/freed. If we did this migration, then the
            // opposite subtree would lose access to this page.
            DEBUG_ASSERT(!(target_page_owner->page_stack_flag_ == StackDir::Left
                    && target_page->object.cow_left_split));
            DEBUG_ASSERT(!(target_page_owner->page_stack_flag_ == StackDir::Right
                    && target_page->object.cow_right_split));

            target_page->object.cow_left_split = 0;
            target_page->object.cow_right_split = 0;
            vm_page_t* expected_page = target_page;
            bool success =
                    target_page_owner->page_list_.RemovePage(target_page_offset, &target_page);
            DEBUG_ASSERT(success);
            DEBUG_ASSERT(target_page == expected_page);
        } else {
            // Otherwise we need to fork the page.
            vm_page_t* cover_page;
            alloc_failure = !AllocateCopyPage(pmm_alloc_flags_, page->paddr(),
                                              free_list, &cover_page);
            if (unlikely(alloc_failure)) {
                // TODO: plumb through PageRequest once anonymous page source is implemented.
                break;
            }

            // We're going to cover target_page with cover_page, so set appropriate split bit.
            if (target_page_owner->page_stack_flag_ == StackDir::Left) {
                target_page->object.cow_left_split = 1;
                DEBUG_ASSERT(target_page->object.cow_right_split == 0);
            } else {
                target_page->object.cow_right_split = 1;
                DEBUG_ASSERT(target_page->object.cow_left_split == 0);
            }
            target_page = cover_page;

            // To maintain the contiguity of the user-visible vmo, keep track of the
            // leaf-most contiguous vmo that has a page inserted into it. We'll come
            // back later and make sure this vmo sees the original contiguous page.
            if (cur->is_contiguous()) {
                last_contig = cur;
                last_contig_offset = cur_offset;
            }

            skip_range_update = false;
        }

        // Skip the automatic range update so we can do it ourselves more efficiently.
        zx_status_t status = cur->AddPageLocked(target_page, cur_offset, false);
        DEBUG_ASSERT(status == ZX_OK);

        if (!skip_range_update) {
            if (cur != this) {
                // In this case, cur is a hidden vmo and has no direct mappings. Also, its
                // descendents along the page stack will be dealt with by subsequent iterations
                // of this loop. That means that any mappings that need to be touched now are
                // owned by the children on the opposite side of page_stack_flag_.
                DEBUG_ASSERT(cur->mapping_list_len_ == 0);
                VmObjectPaged& other = cur->page_stack_flag_ == StackDir::Left
                        ? cur->right_child_locked() : cur->left_child_locked();
                other.RangeChangeUpdateFromParentLocked(cur_offset, PAGE_SIZE);
            } else {
                // In this case, cur is the last vmo being changed, so update its whole subtree.
                DEBUG_ASSERT(offset == cur_offset);
                RangeChangeUpdateLocked(offset, PAGE_SIZE);
            }
        }
    } while (cur != this);
    DEBUG_ASSERT(alloc_failure || cur_offset == offset);

    if (last_contig != nullptr) {
        ContiguousCowFixupLocked(page_owner, owner_offset, last_contig, last_contig_offset);
        if (last_contig == this) {
            target_page = page;
        }
    }

    if (unlikely(alloc_failure)) {
       // Note that this happens after fixing up the contiguous vmo invariant.
        return nullptr;
    } else {
        return target_page;
    }
}

void VmObjectPaged::ContiguousCowFixupLocked(VmObjectPaged* page_owner, uint64_t page_owner_offset,
                                             VmObjectPaged* last_contig,
                                             uint64_t last_contig_offset) {
    // If we're here, then |last_contig| must be contiguous, and all of its
    // ancestors (including |page_owner|) must be contiguous.
    DEBUG_ASSERT(last_contig->is_contiguous());
    DEBUG_ASSERT(page_owner->is_contiguous());

    // When this function is invoked, we know that the desired contiguous page is somewhere
    // between |page_owner| and |last_contig|. Since ::CloneCowPageLocked will no longer
    // migrate the original page once it forks that page, we know that the desired contiguous
    // page is in the root-most vmo that has a page corresponding to the offset.
    //
    // In other words, we can start searching from |page_owner| and progress towards the
    // leaf vmo, and the first page that is found will be the page that needs to be moved
    // into |last_contig|.

    // Use ::ForEveryPageInRange so that we can directly swap the vm_page_t entries
    // in the page lists without having to worry about allocation.
    bool found = false;
    last_contig->page_list_.ForEveryPageInRange(
        [page_owner, page_owner_offset, last_contig, &found](vm_page_t*& page1, uint64_t off)
                // Walks the clone chain, which confuses analysis
                TA_NO_THREAD_SAFETY_ANALYSIS {

            auto swap_fn = [&page1, &found](vm_page_t*& page2, uint64_t off) {
                // We're guaranteed that the first page we see is the one we want.
                DEBUG_ASSERT(page2->object.pin_count == 1);
                found = true;

                vm_page* tmp = page1;
                page1 = page2;
                page2 = tmp;

                bool flag = page1->object.cow_left_split;
                page1->object.cow_left_split = page2->object.cow_left_split;
                page2->object.cow_left_split = flag;

                flag = page1->object.cow_right_split;
                page1->object.cow_right_split = page2->object.cow_right_split;
                page2->object.cow_right_split = flag;

                // Don't swap the pin counts, since those are relevant to the
                // actual physical pages, not to what vmo they're contained in.

                return ZX_ERR_NEXT;
            };

            VmObjectPaged* cur = page_owner;
            uint64_t cur_offset = page_owner_offset;
            while (!found && cur != last_contig) {
                zx_status_t status = cur->page_list_.ForEveryPageInRange(
                    swap_fn, cur_offset, cur_offset + PAGE_SIZE);
                DEBUG_ASSERT(status == ZX_OK);

                if (found) {
                    cur->RangeChangeUpdateLocked(cur_offset, PAGE_SIZE);
                } else {
                    cur = cur->page_stack_flag_ == StackDir::Left ?
                        &cur->left_child_locked() : &cur->right_child_locked();
                    cur_offset = cur_offset - cur->parent_offset_;

                    DEBUG_ASSERT(cur->is_contiguous());
                }
            }
            return ZX_ERR_NEXT;
        },
        last_contig_offset, last_contig_offset + PAGE_SIZE);
    DEBUG_ASSERT(found);

    // It's not necessary to invoke ::RangeChangeUpdateLocked on the |last_contig|, as it is a
    // descendant of whatever vmo ::RangeChangeUpdateLocked was invoked when pages were swapped.

    DEBUG_ASSERT(last_contig->page_list_.GetPage(last_contig_offset)->object.pin_count == 1);
}

vm_page_t* VmObjectPaged::FindInitialPageContentLocked(uint64_t offset, uint pf_flags,
                                                       VmObject** owner_out,
                                                       uint64_t* owner_offset_out) {
    DEBUG_ASSERT(page_list_.GetPage(offset) == nullptr);

    // Search up the clone chain for any committed pages. cur_offset is the offset
    // into cur we care about. The loop terminates either when that offset contains
    // a committed page or when that offset can't reach into the parent.
    vm_page_t* page = nullptr;
    VmObjectPaged* cur = this;
    uint64_t cur_offset = offset;
    while (!page && cur_offset < cur->parent_limit_) {
        // If there's no parent, then parent_limit_ is 0 and we'll never enter the loop
        DEBUG_ASSERT(cur->parent_);

        uint64_t parent_offset;
        bool overflowed = add_overflow(cur->parent_offset_, cur_offset, &parent_offset);
        ASSERT(!overflowed);
        if (parent_offset >= cur->parent_->size()) {
            // The offset is off the end of the parent, so cur is the VmObject
            // which will provide the page.
            break;
        }

        if (!cur->parent_->is_paged()) {
            uint parent_pf_flags = pf_flags & ~VMM_PF_FLAG_WRITE;
            auto status = cur->parent_->GetPageLocked(parent_offset, parent_pf_flags,
                                                      nullptr, nullptr, &page, nullptr);
            // The first if statement should ensure we never make an out-of-range query into a
            // physical VMO, and physical VMOs will always return a page for all valid offsets.
            DEBUG_ASSERT(status == ZX_OK);
            DEBUG_ASSERT(page != nullptr);

            *owner_out = cur->parent_.get();
            *owner_offset_out = parent_offset;
            return page;
        } else {
            cur = VmObjectPaged::AsVmObjectPaged(cur->parent_);
            cur_offset = parent_offset;
            page = cur->page_list_.GetPage(parent_offset);
        }
    }

    *owner_out = cur;
    *owner_offset_out = cur_offset;

    return page;
}

// Looks up the page at the requested offset, faulting it in if requested and necessary.  If
// this VMO has a parent and the requested page isn't found, the parent will be searched.
//
// |free_list|, if not NULL, is a list of allocated but unused vm_page_t that
// this function may allocate from.  This function will need at most one entry,
// and will not fail if |free_list| is a non-empty list, faulting in was requested,
// and offset is in range.
zx_status_t VmObjectPaged::GetPageLocked(uint64_t offset, uint pf_flags, list_node* free_list,
                                         PageRequest* page_request,
                                         vm_page_t** const page_out, paddr_t* const pa_out) {
    canary_.Assert();
    DEBUG_ASSERT(lock_.lock().IsHeld());
    DEBUG_ASSERT(!is_hidden());

    if (offset >= size_) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    offset = ROUNDDOWN(offset, PAGE_SIZE);

    if (is_slice()) {
        uint64_t parent_offset;
        VmObjectPaged *parent = PagedParentOfSliceLocked(&parent_offset);
        return parent->GetPageLocked(offset + parent_offset, pf_flags, free_list, page_request,
                                    page_out, pa_out);
    }

    vm_page_t* p;

    // see if we already have a page at that offset
    p = page_list_.GetPage(offset);
    if (p) {
        if (page_out) {
            *page_out = p;
        }
        if (pa_out) {
            *pa_out = p->paddr();
        }
        return ZX_OK;
    }

    __UNUSED char pf_string[5];
    LTRACEF("vmo %p, offset %#" PRIx64 ", pf_flags %#x (%s)\n", this, offset, pf_flags,
            vmm_pf_flags_to_string(pf_flags, pf_string));

    VmObject* page_owner;
    uint64_t owner_offset;
    if (!parent_) {
        // Avoid the function call in the common case.
        page_owner = this;
        owner_offset = offset;
    } else {
        p = FindInitialPageContentLocked(offset, pf_flags, &page_owner, &owner_offset);
    }

    if (!p) {
        // If we're not being asked to sw or hw fault in the page, return not found.
        if ((pf_flags & VMM_PF_FLAG_FAULT_MASK) == 0) {
            return ZX_ERR_NOT_FOUND;
        }

        // Since physical VMOs always provide pages for their full range, we should
        // never get here for physical VMOs.
        DEBUG_ASSERT(page_owner->is_paged());
        VmObjectPaged* typed_owner = static_cast<VmObjectPaged*>(page_owner);

        if (typed_owner->page_source_) {
            zx_status_t status =
                    typed_owner->page_source_->GetPage(owner_offset, page_request, &p, nullptr);
            // Pager page sources will never synchronously return a page.
            DEBUG_ASSERT(status != ZX_OK);

            if (typed_owner != this && status == ZX_ERR_NOT_FOUND) {
                // The default behavior of clones of detached pager VMOs fault in zero
                // pages instead of propagating the pager's fault.
                // TODO(stevensd): Add an arg to zx_vmo_create_child to optionally fault here.
                p = vm_get_zero_page();
            } else {
                return status;
            }
        } else {
            // If there's no page source, we're using an anonymous page. It's not
            // necessary to fault a writable page directly into the owning VMO.
            p = vm_get_zero_page();
        }
    }
    DEBUG_ASSERT(p);

    if ((pf_flags & VMM_PF_FLAG_WRITE) == 0) {
        // If we're read-only faulting, return the page so they can map or read from it directly.
        if (page_out) {
            *page_out = p;
        }
        if (pa_out) {
            *pa_out = p->paddr();
        }
        LTRACEF("read only faulting in page %p, pa %#" PRIxPTR " from parent\n", p, p->paddr());
        return ZX_OK;
    }

    vm_page_t* res_page;
    if (!page_owner->is_hidden() || p == vm_get_zero_page()) {
        // If the vmo isn't hidden, we can't move the page. If the page is the zero
        // page, there's no need to try to move the page. In either case, we need to
        // allocate a writable page for this vmo.
        if (!AllocateCopyPage(pmm_alloc_flags_, p->paddr(), free_list, &res_page)) {
             return ZX_ERR_NO_MEMORY;
        }
        zx_status_t status = AddPageLocked(res_page, offset);
        DEBUG_ASSERT(status == ZX_OK);

        // If ARM and not fully cached, clean/invalidate the page after zeroing it. Since
        // clones must be cached, we only need to check this here.
#if ARCH_ARM64
        if (cache_policy_ != ARCH_MMU_FLAG_CACHED) {
            arch_clean_invalidate_cache_range(
                    (addr_t) paddr_to_physmap(res_page->paddr()), PAGE_SIZE);
        }
#endif
    } else {
        // We need a writable page; let ::CloneCowPageLocked handle inserting one.
        res_page = CloneCowPageLocked(offset, free_list,
                                      static_cast<VmObjectPaged*>(page_owner), p, owner_offset);
        if (res_page == nullptr) {
            return ZX_ERR_NO_MEMORY;
        }
    }

    LTRACEF("faulted in page %p, pa %#" PRIxPTR "\n", res_page, res_page->paddr());

    if (page_out) {
        *page_out = res_page;
    }
    if (pa_out) {
        *pa_out = res_page->paddr();
    }

    return ZX_OK;
}

zx_status_t VmObjectPaged::CommitRange(uint64_t offset, uint64_t len) {
    canary_.Assert();
    LTRACEF("offset %#" PRIx64 ", len %#" PRIx64 "\n", offset, len);

    Guard<fbl::Mutex> guard{&lock_};

    // trim the size
    uint64_t new_len;
    if (!TrimRange(offset, len, size_, &new_len)) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    // was in range, just zero length
    if (new_len == 0) {
        return ZX_OK;
    }

    // compute a page aligned end to do our searches in to make sure we cover all the pages
    uint64_t end = ROUNDUP_PAGE_SIZE(offset + new_len);
    DEBUG_ASSERT(end > offset);
    offset = ROUNDDOWN(offset, PAGE_SIZE);

    fbl::RefPtr<PageSource> root_source = GetRootPageSourceLocked();

    // If this vmo has a direct page source, then the source will provide the backing memory. For
    // children that eventually depend on a page source, we skip preallocating memory to avoid
    // potentially overallocating pages if something else touches the vmo while we're blocked on the
    // request. Otherwise we optimize things by preallocating all the pages.
    list_node page_list;
    list_initialize(&page_list);
    if (root_source == nullptr) {
        // make a pass through the list to find out how many pages we need to allocate
        size_t count = (end - offset) / PAGE_SIZE;
        page_list_.ForEveryPageInRange(
            [&count](const auto p, auto off) {
                count--;
                return ZX_ERR_NEXT;
            },
            offset, end);

        if (count == 0) {
            return ZX_OK;
        }

        zx_status_t status = pmm_alloc_pages(count, pmm_alloc_flags_, &page_list);
        if (status != ZX_OK) {
            return status;
        }
    }

    auto list_cleanup = fbl::MakeAutoCall([&page_list]() {
        if (!list_is_empty(&page_list)) {
            pmm_free(&page_list);
        }
    });

    bool retry = false;
    PageRequest page_request(true);
    do {
        if (retry) {
            // If there was a page request that couldn't be fulfilled, we need wait on the
            // request and retry the commit. Note that when we retry the loop, offset is
            // updated past the portion of the vmo that we successfully commited.
            zx_status_t status = ZX_OK;
            guard.CallUnlocked([&page_request, &status]() mutable {
                status = page_request.Wait();
            });
            if (status != ZX_OK) {
                return status;
            }
            retry = false;

            // Re-run the range checks, since size_ could have changed while we were blocked. This
            // is not a failure, since the arguments were valid when the syscall was made. It's as
            // if the commit was successful but then the pages were thrown away.
            if (!TrimRange(offset, new_len, size_, &new_len)) {
                return ZX_OK;
            }

            if (new_len == 0) {
                return ZX_OK;
            }

            end = ROUNDUP_PAGE_SIZE(offset + new_len);
            DEBUG_ASSERT(end > offset);
            offset = ROUNDDOWN(offset, PAGE_SIZE);
        }

        // cur_offset tracks how far we've made page requests, even if they're not done
        uint64_t cur_offset = offset;
        // new_offset tracks how far we've successfully committed and is where we'll
        // restart from if we need to retry the commit
        uint64_t new_offset = offset;
        while (cur_offset < end) {
            // Don't commit if we already have this page
            vm_page_t* p = page_list_.GetPage(cur_offset);
            if (!p) {
                // Check if our parent has the page
                const uint flags = VMM_PF_FLAG_SW_FAULT | VMM_PF_FLAG_WRITE;
                zx_status_t res = GetPageLocked(cur_offset, flags, &page_list,
                                                &page_request, nullptr, nullptr);
                if (res == ZX_ERR_NEXT || res == ZX_ERR_SHOULD_WAIT) {
                    // In either case we'll need to wait on the request and retry, but if we get
                    // ZX_ERR_NEXT we keep faulting until we eventually see ZX_ERR_SHOULD_WAIT.
                    retry = true;
                    if (res == ZX_ERR_SHOULD_WAIT) {
                        break;
                    }
                } else if (res != ZX_OK) {
                    return res;
                }
            }

            cur_offset += PAGE_SIZE;
            if (!retry) {
                new_offset = offset;
            }
        }

        // Unmap all of the pages in the range we touched. This may end up unmapping non-present
        // ranges or unmapping things multiple times, but it's necessary to ensure that we unmap
        // everything that actually is present before anything else sees it.
        if (cur_offset - offset) {
            RangeChangeUpdateLocked(offset, cur_offset - offset);
        }

        if (retry && cur_offset == end) {
            zx_status_t res = root_source->FinalizeRequest(&page_request);
            if (res != ZX_ERR_SHOULD_WAIT) {
                return res;
            }
        }
        offset = new_offset;
    } while (retry);

    return ZX_OK;
}

zx_status_t VmObjectPaged::DecommitRange(uint64_t offset, uint64_t len) {
    canary_.Assert();
    LTRACEF("offset %#" PRIx64 ", len %#" PRIx64 "\n", offset, len);
    list_node_t list;
    list_initialize(&list);
    zx_status_t status;
    {
        Guard<fbl::Mutex> guard{&lock_};
        status = DecommitRangeLocked(offset, len, list);
    }
    if (status == ZX_OK) {
        pmm_free(&list);
    }
    return status;
}

zx_status_t VmObjectPaged::DecommitRangeLocked(uint64_t offset, uint64_t len, list_node_t &free_list) {

    if (options_ & kContiguous) {
        return ZX_ERR_NOT_SUPPORTED;
    }

    if (is_slice()) {
        uint64_t parent_offset;
        VmObjectPaged* parent = PagedParentOfSliceLocked(&parent_offset);
        // Use a lambda to escape thread analysis as it does not understand that we are holding the
        // parents lock right now.
        return [parent, &free_list, len] (uint64_t offset)
                TA_NO_THREAD_SAFETY_ANALYSIS -> zx_status_t {
            return parent->DecommitRangeLocked(offset, len, free_list);
        }(offset + parent_offset);
    }

    // trim the size
    uint64_t new_len;
    if (!TrimRange(offset, len, size_, &new_len)) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    // was in range, just zero length
    if (new_len == 0) {
        return ZX_OK;
    }

    // figure the starting and ending page offset
    uint64_t start = ROUNDDOWN(offset, PAGE_SIZE);
    uint64_t end = ROUNDUP_PAGE_SIZE(offset + new_len);
    DEBUG_ASSERT(end > offset);
    DEBUG_ASSERT(end > start);
    uint64_t page_aligned_len = end - start;

    LTRACEF("start offset %#" PRIx64 ", end %#" PRIx64 ", page_aliged_len %#" PRIx64 "\n", start, end,
            page_aligned_len);

    // TODO(teisenbe): Allow decommitting of pages pinned by
    // CommitRangeContiguous

    if (AnyPagesPinnedLocked(start, page_aligned_len)) {
        return ZX_ERR_BAD_STATE;
    }

    // unmap all of the pages in this range on all the mapping regions
    RangeChangeUpdateLocked(start, page_aligned_len);

    page_list_.RemovePages(start, end, &free_list);

    return ZX_OK;
}

zx_status_t VmObjectPaged::Pin(uint64_t offset, uint64_t len) {
    canary_.Assert();

    Guard<fbl::Mutex> guard{&lock_};
    return PinLocked(offset, len);
}

zx_status_t VmObjectPaged::PinLocked(uint64_t offset, uint64_t len) {
    canary_.Assert();

    // verify that the range is within the object
    if (unlikely(!InRange(offset, len, size_))) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    if (unlikely(len == 0)) {
        return ZX_OK;
    }

    if (is_slice()) {
        uint64_t parent_offset;
        VmObjectPaged *parent = PagedParentOfSliceLocked(&parent_offset);
        // Use a lambda to escape thread analysis as it does not understand that we are holding the
        // parents lock right now.
        return [parent, len] (uint64_t offset) TA_NO_THREAD_SAFETY_ANALYSIS -> zx_status_t {
            return parent->PinLocked(offset, len);
        }(offset + parent_offset);
    }

    const uint64_t start_page_offset = ROUNDDOWN(offset, PAGE_SIZE);
    const uint64_t end_page_offset = ROUNDUP(offset + len, PAGE_SIZE);

    uint64_t pin_range_end = start_page_offset;
    zx_status_t status = page_list_.ForEveryPageAndGapInRange(
        [&pin_range_end](const auto p, uint64_t off) {
            DEBUG_ASSERT(p->state() == VM_PAGE_STATE_OBJECT);
            if (p->object.pin_count == VM_PAGE_OBJECT_MAX_PIN_COUNT) {
                return ZX_ERR_UNAVAILABLE;
            }

            p->object.pin_count++;
            pin_range_end = off + PAGE_SIZE;
            return ZX_ERR_NEXT;
        },
        [](uint64_t gap_start, uint64_t gap_end) {
            return ZX_ERR_NOT_FOUND;
        },
        start_page_offset, end_page_offset);

    if (status != ZX_OK) {
        UnpinLocked(start_page_offset, pin_range_end - start_page_offset);
        return status;
    }

    return ZX_OK;
}

void VmObjectPaged::Unpin(uint64_t offset, uint64_t len) {
    Guard<fbl::Mutex> guard{&lock_};
    UnpinLocked(offset, len);
}

void VmObjectPaged::UnpinLocked(uint64_t offset, uint64_t len) {
    canary_.Assert();
    DEBUG_ASSERT(lock_.lock().IsHeld());

    // verify that the range is within the object
    ASSERT(InRange(offset, len, size_));

    if (unlikely(len == 0)) {
        return;
    }

    if (is_slice()) {
        uint64_t parent_offset;
        VmObjectPaged *parent = PagedParentOfSliceLocked(&parent_offset);
        // Use a lambda to escape thread analysis as it does not understand that we are holding the
        // parents lock right now.
        return [parent, len] (uint64_t offset) TA_NO_THREAD_SAFETY_ANALYSIS {
            parent->UnpinLocked(offset, len);
        }(offset + parent_offset);
    }

    const uint64_t start_page_offset = ROUNDDOWN(offset, PAGE_SIZE);
    const uint64_t end_page_offset = ROUNDUP(offset + len, PAGE_SIZE);

    zx_status_t status = page_list_.ForEveryPageAndGapInRange(
        [](const auto p, uint64_t off) {
            DEBUG_ASSERT(p->state() == VM_PAGE_STATE_OBJECT);
            ASSERT(p->object.pin_count > 0);
            p->object.pin_count--;
            return ZX_ERR_NEXT;
        },
        [](uint64_t gap_start, uint64_t gap_end) {
            return ZX_ERR_NOT_FOUND;
        },
        start_page_offset, end_page_offset);
    ASSERT_MSG(status == ZX_OK, "Tried to unpin an uncommitted page");
    return;
}

bool VmObjectPaged::AnyPagesPinnedLocked(uint64_t offset, size_t len) {
    canary_.Assert();
    DEBUG_ASSERT(lock_.lock().IsHeld());
    DEBUG_ASSERT(IS_PAGE_ALIGNED(offset));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(len));

    const uint64_t start_page_offset = offset;
    const uint64_t end_page_offset = offset + len;

    bool found_pinned = false;
    page_list_.ForEveryPageInRange(
        [&found_pinned, start_page_offset, end_page_offset](const auto p, uint64_t off) {
            DEBUG_ASSERT(off >= start_page_offset && off < end_page_offset);
            if (p->object.pin_count > 0) {
                found_pinned = true;
                return ZX_ERR_STOP;
            }
            return ZX_ERR_NEXT;
        },
        start_page_offset, end_page_offset);

    return found_pinned;
}

void VmObjectPaged::ReleaseCowParentPagesLocked(uint64_t start, uint64_t end,
                                                list_node_t* free_list) {
    start = fbl::max(start, parent_start_limit_);
    end = fbl::min(end, parent_limit_);
    if (start >= end) {
        return;
    }

    if (!parent_ || !parent_->is_hidden() || parent_start_limit_ == parent_limit_) {
        return;
    }
    auto parent = VmObjectPaged::AsVmObjectPaged(parent_);
    bool left = this == &parent->left_child_locked();
    auto& other = left ? parent->right_child_locked() : parent->left_child_locked();

    // Compute the range in the parent that cur no longer will be able to see.
    uint64_t parent_range_start, parent_range_end;
    bool overflow = add_overflow(start, parent_offset_, &parent_range_start);
    bool overflow2 = add_overflow(end, parent_offset_, &parent_range_end);
    DEBUG_ASSERT(!overflow && !overflow2); // vmo creation should have failed.

    bool skip_split_bits;
    if (parent_limit_ == end) {
        parent_limit_ = start;
        parent_start_limit_ = fbl::min(parent_limit_, parent_start_limit_);
        skip_split_bits = true;
    } else if (start == parent_start_limit_) {
        parent_start_limit_ = end;
        skip_split_bits = true;
    } else {
        // If the vmo limits can't be updated, this function will need to use the split bits
        // to release pages in the parent. It also means that ancestor pages in the specified
        // range might end up being released based on their current split bits, instead of through
        // subsequent calls to this function. Therefore parent and all ancestors need to have
        // the partial_cow_release_ flag set to prevent fast merge issues in ::RemoveChild.
        auto cur = this;
        uint64_t cur_start = start;
        uint64_t cur_end = end;
        while (cur->parent_ && cur_start < cur_end) {
            auto parent = VmObjectPaged::AsVmObjectPaged(cur->parent_);
            parent->partial_cow_release_ = true;
            cur_start = fbl::max(cur_start + cur->parent_offset_, parent->parent_start_limit_);
            cur_end = fbl::min(cur_end + cur->parent_offset_, parent->parent_limit_);
            cur = parent;
        }
        skip_split_bits = false;
    }

    // Drop any pages in the parent which are outside of the other child's accessibility, and
    // recursively release COW pages in ancestor vmos in those inaccessible regions.
    //
    // There are two newly inaccessible regions to consider here - the region before what the
    // sibling can access and the region after what it can access. We first free the 'head'
    // region and calculate |tail_start| to determine where the 'tail' region starts. Then
    // we can free the 'tail' region.
    //
    // Note that |update_limit| can only be recursively passed into the second call since we
    // don't want the parent's parent_limit_ to be updated if the sibling can still access
    // the pages.
    uint64_t tail_start;
    if (other.parent_start_limit_ != other.parent_limit_) {
        if (parent_range_start < other.parent_offset_ + other.parent_start_limit_) {
            uint64_t head_end =
                    fbl::min(other.parent_offset_ + other.parent_start_limit_, parent_range_end);
            parent->page_list_.RemovePages(parent_range_start, head_end, free_list);
            parent->ReleaseCowParentPagesLocked(parent_range_start, head_end, free_list);
        }
        tail_start = fbl::max(other.parent_offset_ + other.parent_limit_, parent_range_start);
    } else {
        // If the sibling can't access anything in the parent, the whole region
        // we're operating on is the 'tail' region.
        tail_start = parent_range_start;
    }
    if (tail_start < parent_range_end) {
        parent->page_list_.RemovePages(tail_start, parent_range_end, free_list);
        parent->ReleaseCowParentPagesLocked(tail_start, parent_range_end, free_list);
    }

    // Any pages left were accesible by both children. Free any pages that were already split
    // into the other child. For pages that haven't been split into the other child, we need to
    // ensure they're univisible. We don't need to recurse on this range because the visibility of
    // parent's ancestor pages in this range isn't changing.
    parent->page_list_.RemovePages([skip_split_bits, left](vm_page_t*& page, auto offset) -> bool {
        // Simply checking if the page is resident in |this|->page_list_ is insufficient, as the
        // page split into this vmo could have been migrated anywhere into is children. To avoid
        // having to search its entire child subtree, we need to track into which subtree
        // a page is split (i.e. have two directional split bits instead of a single split bit).
        if (left ? page->object.cow_right_split : page->object.cow_left_split) {
            return true;
        }
        if (skip_split_bits) {
            // If we were able to update this vmo's parent limit, that made the pages
            // uniaccessible. We clear the split bits to allow ::OnChildRemoved to efficiently
            // merge vmos without having to worry about pages above parent_limit_.
            page->object.cow_left_split = 0;
            page->object.cow_right_split = 0;
        } else {
            // Otherwise set the appropriate split bit to make the page uniaccessible.
            if (left) {
                page->object.cow_left_split = 1;
            } else {
                page->object.cow_right_split = 1;
            }
        }
        return false;
    }, parent_range_start, parent_range_end, free_list);
}

zx_status_t VmObjectPaged::Resize(uint64_t s) {
    canary_.Assert();

    LTRACEF("vmo %p, size %" PRIu64 "\n", this, s);

    if (!(options_ & kResizable)) {
        return ZX_ERR_UNAVAILABLE;
    }

    // round up the size to the next page size boundary and make sure we dont wrap
    zx_status_t status = RoundSize(s, &s);
    if (status != ZX_OK) {
        return status;
    }

    Guard<fbl::Mutex> guard{&lock_};

    // make sure everything is aligned before we get started
    DEBUG_ASSERT(IS_PAGE_ALIGNED(size_));
    DEBUG_ASSERT(IS_PAGE_ALIGNED(s));

    list_node_t free_list;
    list_initialize(&free_list);

    // see if we're shrinking or expanding the vmo
    if (s < size_) {
        // shrinking
        uint64_t start = s;
        uint64_t end = size_;
        uint64_t len = end - start;

        // bail if there are any pinned pages in the range we're trimming
        if (AnyPagesPinnedLocked(start, len)) {
            return ZX_ERR_BAD_STATE;
        }

        // unmap all of the pages in this range on all the mapping regions
        RangeChangeUpdateLocked(start, len);

        if (page_source_) {
            // Tell the page source that any non-resident pages that are now out-of-bounds
            // were supplied, to ensure that any reads of those pages get woken up.
            zx_status_t status = page_list_.ForEveryPageAndGapInRange(
                [](const auto p, uint64_t off) {
                    return ZX_ERR_NEXT;
                },
                [&](uint64_t gap_start, uint64_t gap_end) {
                    page_source_->OnPagesSupplied(gap_start, gap_end);
                    return ZX_ERR_NEXT;
                },
                start, end);
            DEBUG_ASSERT(status == ZX_OK);
        }

        if (parent_ && parent_->is_hidden()) {
            // Release any COW pages that are no longer necessary. This will also
            // update the parent limit.
            ReleaseCowParentPagesLocked(start, end, &free_list);
        } else {
            parent_limit_ = fbl::min(parent_limit_, s);
        }
        // If the tail of a parent disappears, the children shouldn't be able to see that region
        // again, even if the parent is later reenlarged. So update the child parent limits.
        UpdateChildParentLimitsLocked(s);

        page_list_.RemovePages(start, end, &free_list);
    } else if (s > size_) {
        // expanding
        // figure the starting and ending page offset that is affected
        uint64_t start = size_;
        uint64_t end = s;
        uint64_t len = end - start;

        // inform all our children or mapping that there's new bits
        RangeChangeUpdateLocked(start, len);
    }

    // save bytewise size
    size_ = s;

    guard.Release();
    pmm_free(&free_list);

    return ZX_OK;
}

void VmObjectPaged::UpdateChildParentLimitsLocked(uint64_t new_size) {
    // Note that a child's parent_limit_ will limit that child's descendants' views into
    // this vmo, so this method only needs to touch the direct children.
    for (auto& child : children_list_) {
        if (new_size < child.parent_offset_) {
            child.parent_limit_ = 0;
        } else {
            child.parent_limit_ = fbl::min(child.parent_limit_, new_size - child.parent_offset_);
        }
    }
}

// perform some sort of copy in/out on a range of the object using a passed in lambda
// for the copy routine
template <typename T>
zx_status_t VmObjectPaged::ReadWriteInternal(uint64_t offset, size_t len, bool write, T copyfunc) {
    canary_.Assert();

    Guard<fbl::Mutex> guard{&lock_};

    // are we uncached? abort in this case
    if (cache_policy_ != ARCH_MMU_FLAG_CACHED) {
        return ZX_ERR_BAD_STATE;
    }

    // Test if in range. If we block on a page request, then it's possible for the
    // size to change. If that happens, then any out-of-bounds reads will be caught
    // by GetPageLocked.
    uint64_t end_offset;
    if (add_overflow(offset, len, &end_offset) || end_offset > size_) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    // Walk the list of pages and do the read/write. This is performed in
    // a loop to deal with blocking on asynchronous page requests.
    uint64_t src_offset = offset;
    size_t dest_offset = 0;
    PageRequest page_request;
    bool need_retry = false;
    do {
        if (need_retry) {
            // If we looped because of an asynchronous page request, block on it
            // outside the lock and then resume reading/writing.
            zx_status_t status;
            guard.CallUnlocked([&status, &page_request]() {
                status = page_request.Wait();
            });
            if (status != ZX_OK) {
                return status;
            }
            need_retry = false;
        }

        while (len > 0) {
            size_t page_offset = src_offset % PAGE_SIZE;
            size_t tocopy = fbl::min(PAGE_SIZE - page_offset, len);

            // fault in the page
            paddr_t pa;
            auto status = GetPageLocked(src_offset,
                                        VMM_PF_FLAG_SW_FAULT | (write ? VMM_PF_FLAG_WRITE : 0),
                                        nullptr, &page_request, nullptr, &pa);
            if (status == ZX_ERR_SHOULD_WAIT) {
                need_retry = true;
                break;
            } else if (status != ZX_OK) {
                return status;
            }

            // compute the kernel mapping of this page
            uint8_t* page_ptr = reinterpret_cast<uint8_t*>(paddr_to_physmap(pa));

            // call the copy routine
            auto err = copyfunc(page_ptr + page_offset, dest_offset, tocopy);
            if (err < 0) {
                return err;
            }

            src_offset += tocopy;
            dest_offset += tocopy;
            len -= tocopy;
        }
    } while (need_retry);

    return ZX_OK;
}

zx_status_t VmObjectPaged::Read(void* _ptr, uint64_t offset, size_t len) {
    canary_.Assert();
    // test to make sure this is a kernel pointer
    if (!is_kernel_address(reinterpret_cast<vaddr_t>(_ptr))) {
        DEBUG_ASSERT_MSG(0, "non kernel pointer passed\n");
        return ZX_ERR_INVALID_ARGS;
    }

    // read routine that just uses a memcpy
    uint8_t* ptr = reinterpret_cast<uint8_t*>(_ptr);
    auto read_routine = [ptr](const void* src, size_t offset, size_t len) -> zx_status_t {
        memcpy(ptr + offset, src, len);
        return ZX_OK;
    };

    return ReadWriteInternal(offset, len, false, read_routine);
}

zx_status_t VmObjectPaged::Write(const void* _ptr, uint64_t offset, size_t len) {
    canary_.Assert();
    // test to make sure this is a kernel pointer
    if (!is_kernel_address(reinterpret_cast<vaddr_t>(_ptr))) {
        DEBUG_ASSERT_MSG(0, "non kernel pointer passed\n");
        return ZX_ERR_INVALID_ARGS;
    }

    // write routine that just uses a memcpy
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(_ptr);
    auto write_routine = [ptr](void* dst, size_t offset, size_t len) -> zx_status_t {
        memcpy(dst, ptr + offset, len);
        return ZX_OK;
    };

    return ReadWriteInternal(offset, len, true, write_routine);
}

zx_status_t VmObjectPaged::Lookup(uint64_t offset, uint64_t len,
                                  vmo_lookup_fn_t lookup_fn, void* context) {
    canary_.Assert();
    if (unlikely(len == 0)) {
        return ZX_ERR_INVALID_ARGS;
    }

    Guard<fbl::Mutex> guard{&lock_};

    // verify that the range is within the object
    if (unlikely(!InRange(offset, len, size_))) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    const uint64_t start_page_offset = ROUNDDOWN(offset, PAGE_SIZE);
    const uint64_t end_page_offset = ROUNDUP(offset + len, PAGE_SIZE);

    zx_status_t status = page_list_.ForEveryPageAndGapInRange(
        [lookup_fn, context, start_page_offset](const auto p, uint64_t off) {
            const size_t index = (off - start_page_offset) / PAGE_SIZE;
            paddr_t pa = p->paddr();
            zx_status_t status = lookup_fn(context, off, index, pa);
            if (status != ZX_OK) {
                if (unlikely(status == ZX_ERR_NEXT || status == ZX_ERR_STOP)) {
                    status = ZX_ERR_INTERNAL;
                }
                return status;
            }
            return ZX_ERR_NEXT;
        },
        [this, lookup_fn, context, start_page_offset](uint64_t gap_start, uint64_t gap_end) {
            // If some page was missing from our list, run the more expensive
            // GetPageLocked to see if our parent has it.
            for (uint64_t off = gap_start; off < gap_end; off += PAGE_SIZE) {

                paddr_t pa;
                zx_status_t status = this->GetPageLocked(off, 0, nullptr, nullptr, nullptr, &pa);
                if (status != ZX_OK) {
                    return ZX_ERR_NO_MEMORY;
                }
                const size_t index = (off - start_page_offset) / PAGE_SIZE;
                status = lookup_fn(context, off, index, pa);
                if (status != ZX_OK) {
                    if (unlikely(status == ZX_ERR_NEXT || status == ZX_ERR_STOP)) {
                        status = ZX_ERR_INTERNAL;
                    }
                    return status;
                }
            }
            return ZX_ERR_NEXT;
        },
        start_page_offset, end_page_offset);
    if (status != ZX_OK) {
        return status;
    }

    return ZX_OK;
}

zx_status_t VmObjectPaged::ReadUser(user_out_ptr<void> ptr, uint64_t offset, size_t len) {
    canary_.Assert();

    // read routine that uses copy_to_user
    auto read_routine = [ptr](const void* src, size_t offset, size_t len) -> zx_status_t {
        return ptr.byte_offset(offset).copy_array_to_user(src, len);
    };

    return ReadWriteInternal(offset, len, false, read_routine);
}

zx_status_t VmObjectPaged::WriteUser(user_in_ptr<const void> ptr, uint64_t offset, size_t len) {
    canary_.Assert();

    // write routine that uses copy_from_user
    auto write_routine = [ptr](void* dst, size_t offset, size_t len) -> zx_status_t {
        return ptr.byte_offset(offset).copy_array_from_user(dst, len);
    };

    return ReadWriteInternal(offset, len, true, write_routine);
}

zx_status_t VmObjectPaged::TakePages(uint64_t offset, uint64_t len, VmPageSpliceList* pages) {
    Guard<fbl::Mutex> src_guard{&lock_};
    uint64_t end;
    if (add_overflow(offset, len, &end) || size() < end) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    if (AnyPagesPinnedLocked(offset, len) || parent_ || page_source_) {
        return ZX_ERR_BAD_STATE;
    }

    // This is only used by the userpager API, which has significant restrictions on
    // what sorts of vmos are acceptable. If splice starts being used in more places,
    // then this restriction might need to be lifted.
    // TODO: Check that the region is locked once locking is implemented
    if (mapping_list_len_ || children_list_len_
            || AttributedPagesInRangeLocked(offset , len) != (len / PAGE_SIZE)) {
        return ZX_ERR_BAD_STATE;
    }

    *pages = page_list_.TakePages(offset, len);

    return ZX_OK;
}

zx_status_t VmObjectPaged::SupplyPages(uint64_t offset, uint64_t len, VmPageSpliceList* pages) {
    Guard<fbl::Mutex> guard{&lock_};
    ASSERT(page_source_);

    uint64_t end;
    if (add_overflow(offset, len, &end) || size() < end) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    list_node free_list;
    list_initialize(&free_list);

    // [new_pages_start, new_pages_start + new_pages_len) tracks the current run of
    // consecutive new pages added to this vmo.
    uint64_t new_pages_start = offset;
    uint64_t new_pages_len = 0;
    zx_status_t status = ZX_OK;
    while (!pages->IsDone()) {
        vm_page* src_page = pages->Pop();
        status = AddPageLocked(src_page, offset);
        if (status == ZX_OK) {
            new_pages_len += PAGE_SIZE;
        } else {
            list_add_tail(&free_list, &src_page->queue_node);

            if (likely(status == ZX_ERR_ALREADY_EXISTS)) {
                status = ZX_OK;

                // We hit the end of a run of absent pages, so notify the pager source
                // of any new pages that were added and reset the tracking variables.
                if (new_pages_len) {
                    page_source_->OnPagesSupplied(new_pages_start, new_pages_len);
                }
                new_pages_start = offset + PAGE_SIZE;
                new_pages_len = 0;
            } else {
                break;
            }
        }
        offset += PAGE_SIZE;

        DEBUG_ASSERT(new_pages_start + new_pages_len <= end);
    }
    if (new_pages_len) {
        page_source_->OnPagesSupplied(new_pages_start, new_pages_len);
    }

    if (!list_is_empty(&free_list)) {
        pmm_free(&free_list);
    }

    return status;
}

uint32_t VmObjectPaged::GetMappingCachePolicy() const {
    Guard<fbl::Mutex> guard{&lock_};

    return cache_policy_;
}

zx_status_t VmObjectPaged::SetMappingCachePolicy(const uint32_t cache_policy) {
    // Is it a valid cache flag?
    if (cache_policy & ~ZX_CACHE_POLICY_MASK) {
        return ZX_ERR_INVALID_ARGS;
    }

    Guard<fbl::Mutex> guard{&lock_};

    // conditions for allowing the cache policy to be set:
    // 1) vmo has no pages committed currently
    // 2) vmo has no mappings
    // 3) vmo has no children
    // 4) vmo is not a child
    if (!page_list_.IsEmpty()) {
        return ZX_ERR_BAD_STATE;
    }
    if (!mapping_list_.is_empty()) {
        return ZX_ERR_BAD_STATE;
    }
    if (!children_list_.is_empty()) {
        return ZX_ERR_BAD_STATE;
    }
    if (parent_) {
        return ZX_ERR_BAD_STATE;
    }

    cache_policy_ = cache_policy;

    return ZX_OK;
}

void VmObjectPaged::RangeChangeUpdateFromParentLocked(const uint64_t offset, const uint64_t len) {
    canary_.Assert();

    LTRACEF("offset %#" PRIx64 " len %#" PRIx64 " p_offset %#" PRIx64 " size_ %#" PRIx64 "\n",
            offset, len, parent_offset_, size_);

    // our parent is notifying that a range of theirs changed, see where it intersects
    // with our offset into the parent and pass it on
    uint64_t offset_new;
    uint64_t len_new;
    if (!GetIntersect(parent_offset_, size_, offset, len,
                      &offset_new, &len_new)) {
        return;
    }

    // if they intersect with us, then by definition the new offset must be >= parent_offset_
    DEBUG_ASSERT(offset_new >= parent_offset_);

    // subtract our offset
    offset_new -= parent_offset_;

    // verify that it's still within range of us
    DEBUG_ASSERT(offset_new + len_new <= size_);

    LTRACEF("new offset %#" PRIx64 " new len %#" PRIx64 "\n",
            offset_new, len_new);

    // pass it on
    // TODO: optimize by not passing on ranges that are completely covered by pages local to this vmo
    RangeChangeUpdateLocked(offset_new, len_new);
}

fbl::RefPtr<PageSource> VmObjectPaged::GetRootPageSourceLocked() const {
    auto vm_object = this;
    while (vm_object->parent_) {
        vm_object = VmObjectPaged::AsVmObjectPaged(vm_object->parent_);
        if (!vm_object) {
            return nullptr;
        }
    }
    return vm_object->page_source_;
}

bool VmObjectPaged::IsBidirectionalClonable() const {
    Guard<fbl::Mutex> guard{&lock_};

    // Bidirectional clones of pager vmos aren't supported as we can't
    // efficiently make an immutable snapshot.
    if (page_source_) {
        return false;
    }

    // vmos descended from paged/physical vmos can't be eager cloned.
    auto parent = parent_;
    while (parent) {
        auto p = VmObjectPaged::AsVmObjectPaged(parent);
        if (!p || p->page_source_) {
            return false;
        }
        parent = p->parent_;
    }
    return true;
}

VmObjectPaged* VmObjectPaged::PagedParentOfSliceLocked(uint64_t *offset) {
    DEBUG_ASSERT(is_slice());
    VmObjectPaged *cur = this;
    uint64_t off = 0;
    while (cur->is_slice()) {
        off += cur->parent_offset_;
        DEBUG_ASSERT(cur->parent_);
        DEBUG_ASSERT(cur->parent_->is_paged());
        cur = static_cast<VmObjectPaged*>(cur->parent_.get());
    }
    *offset = off;
    return cur;
}
