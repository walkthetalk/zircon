// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <elf.h>
#include <fbl/auto_call.h>
#include <fbl/function.h>
#include <lib/zx/bti.h>
#include <lib/zx/iommu.h>
#include <lib/zx/pager.h>
#include <lib/zx/port.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <link.h>
#include <utility>
#include <zircon/assert.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/iommu.h>
#include <zxtest/zxtest.h>

extern "C" __WEAK zx_handle_t get_root_resource(void);

namespace {

// Some tests below rely on sampling the memory statistics and having only the
// page allocations directly incurred by the test code happen during the test.
// Those samples can be polluted by any COW faults taken by this program itself
// for touching its own data pages.  So avoid the pollution by preemptively
// faulting in all the static data pages beforehand.
class KmemStatsFixture : public zxtest::Test {
public:
    static void SetUpTestCase() {
        if (get_root_resource) {
            root_resource_ = zx::unowned_resource{get_root_resource()};
            ASSERT_TRUE(root_resource_->is_valid());
            ASSERT_EQ(dl_iterate_phdr(&DlIterpatePhdrCallback, nullptr), 0);
        }
    }

    static void KmemStats(zx_info_kmem_stats_t* info) {
        ASSERT_OK(root_resource_->get_info(ZX_INFO_KMEM_STATS, info,
                                           sizeof(*info), nullptr, nullptr));
    }

    static size_t KmemVmoMemUsage() {
        if (root_resource_->is_valid()) {
            zx_info_kmem_stats_t info;
            KmemStats(&info);
            return info.vmo_bytes;
        }
        return 0;
    }

    static const zx::resource& RootResource() {
        return *root_resource_;
    }

private:
    static zx::unowned_resource root_resource_;

    // Touch every page in the region to make sure it's been COW'd.
    __attribute__((no_sanitize("all"))) static void PrefaultPages(
        uintptr_t start, uintptr_t end) {
        while (start < end) {
            auto ptr = reinterpret_cast<volatile uintptr_t*>(start);
            *ptr = *ptr;
            start += ZX_PAGE_SIZE;
        }
    }

    // Called on each loaded module to collect the bounds of its data pages.
    static void PrefaultData(const Elf64_Phdr* const phdrs, uint16_t phnum,
                             uintptr_t bias) {
        // First find the RELRO segment, which may span part or all
        // of a writable segment (that's thus no longer actually writable).
        const Elf64_Phdr* relro = nullptr;
        for (uint_fast16_t i = 0; i < phnum; ++i) {
            const Elf64_Phdr* ph = &phdrs[i];
            if (ph->p_type == PT_GNU_RELRO) {
                relro = ph;
                break;
            }
        }

        // Now process each writable segment.
        for (uint_fast16_t i = 0; i < phnum; ++i) {
            const Elf64_Phdr* const ph = &phdrs[i];
            if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_W)) {
                continue;
            }
            uintptr_t start = ph->p_vaddr;
            uintptr_t end = ph->p_vaddr + ph->p_memsz;
            ASSERT_LE(start, end);
            if (relro && relro->p_vaddr >= start && relro->p_vaddr < end) {
                start = relro->p_vaddr + relro->p_memsz;
                ASSERT_GE(start, ph->p_vaddr);
                if (start >= end) {
                    continue;
                }
            }
            start = (start + ZX_PAGE_SIZE - 1) & -uintptr_t{ZX_PAGE_SIZE};
            end &= -uintptr_t{ZX_PAGE_SIZE};
            PrefaultPages(bias + start, bias + end);
        }
    }

    static int DlIterpatePhdrCallback(dl_phdr_info* info, size_t, void*) {
        PrefaultData(info->dlpi_phdr, info->dlpi_phnum, info->dlpi_addr);
        return 0;
    }
};

zx::unowned_resource KmemStatsFixture::root_resource_;

void VmoWrite(const zx::vmo& vmo, uint32_t data, uint32_t offset = 0) {
    zx_status_t status = vmo.write(static_cast<void*>(&data), offset, sizeof(data));
    ASSERT_OK(status, "write failed");
}

void VmoCheck(const zx::vmo& vmo, uint32_t expected, uint32_t offset = 0) {
    uint32_t data;
    zx_status_t status = vmo.read(static_cast<void*>(&data), offset, sizeof(data));
    ASSERT_OK(status, "read failed");
    ASSERT_EQ(expected, data);
}

// Creates a vmo with |page_count| pages and writes (page_index + 1) to each page.
void InitPageTaggedVmo(uint32_t page_count, zx::vmo* vmo) {
    zx_status_t status;
    status = zx::vmo::create(page_count * ZX_PAGE_SIZE, ZX_VMO_RESIZABLE, vmo);
    ASSERT_OK(status, "create failed");
    for (unsigned i = 0; i < page_count; i++) {
        ASSERT_NO_FATAL_FAILURES(VmoWrite(*vmo, i + 1, i * ZX_PAGE_SIZE));
    }
}

size_t VmoNumChildren(const zx::vmo& vmo) {
    zx_info_vmo_t info;
    if (vmo.get_info(ZX_INFO_VMO, &info, sizeof(info), nullptr, nullptr) != ZX_OK) {
        return UINT64_MAX;
    }
    return info.num_children;
}

size_t VmoCommittedBytes(const zx::vmo& vmo) {
    zx_info_vmo_t info;
    if (vmo.get_info(ZX_INFO_VMO, &info, sizeof(info), nullptr, nullptr) != ZX_OK) {
        return UINT64_MAX;
    }
    return info.committed_bytes;
}

// Simple class for managing vmo mappings w/o any external dependencies.
class Mapping {
public:
    ~Mapping() {
        if (addr_) {
            ZX_ASSERT(zx::vmar::root_self()->unmap(addr_, len_) == ZX_OK);
        }
    }

    zx_status_t Init(const zx::vmo& vmo, size_t len) {
        zx_status_t status = zx::vmar::root_self()->map(0, vmo, 0, len,
                                                        ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, &addr_);
        len_ = len;
        return status;
    }

    uint32_t* ptr() { return reinterpret_cast<uint32_t*>(addr_); }

private:
    uint64_t addr_ = 0;
    size_t len_ = 0;
};

// Helper function which checks that the give vmo is contiguous.
template<size_t N>
void CheckContigState(const zx::bti& bti, const zx::vmo& vmo) {
    zx::pmt pmt;
    zx_paddr_t addrs[N];
    zx_status_t status = bti.pin(ZX_BTI_PERM_READ, vmo, 0, N * ZX_PAGE_SIZE, addrs, N, &pmt);
    ASSERT_OK(status, "pin failed");
    pmt.unpin();

    for (unsigned i = 0; i < N - 1; i++) {
        ASSERT_EQ(addrs[i] + ZX_PAGE_SIZE, addrs[i + 1]);
    }
}

// Helper function for CallPermutations
template <typename T>
void CallPermutationsHelper(T fn, uint32_t count,
                                     uint32_t perm[], bool elts[], uint32_t idx) {
    if (idx == count) {
        ASSERT_NO_FATAL_FAILURES(fn(perm));
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        if (elts[i]) {
            continue;
        }

        elts[i] = true;
        perm[idx] = i;

        ASSERT_NO_FATAL_FAILURES(CallPermutationsHelper(fn, count, perm, elts, idx + 1));

        elts[i] = false;
    }
}

// Function which invokes |fn| with all the permutations of [0...count-1].
template <typename T>
void CallPermutations(T fn, uint32_t count) {
    uint32_t perm[count];
    bool elts[count];

    for (unsigned i = 0; i < count; i++) {
        perm[i] = 0;
        elts[i] = false;
    }

    ASSERT_NO_FATAL_FAILURES(CallPermutationsHelper(fn, count, perm, elts, 0));
}

// Checks the correctness of various zx_info_vmo_t properties.
TEST(VmoClone2TestCase, Info) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    zx_info_vmo_t orig_info;
    EXPECT_OK(vmo.get_info(ZX_INFO_VMO, &orig_info, sizeof(orig_info), nullptr, nullptr));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone));

    zx_info_vmo_t new_info;
    EXPECT_OK(vmo.get_info(ZX_INFO_VMO, &new_info, sizeof(new_info), nullptr, nullptr));

    zx_info_vmo_t clone_info;
    EXPECT_OK(clone.get_info(ZX_INFO_VMO, &clone_info, sizeof(clone_info), nullptr, nullptr));

    // Check for consistency of koids.
    ASSERT_EQ(orig_info.koid, new_info.koid);
    ASSERT_NE(orig_info.koid, clone_info.koid);
    ASSERT_EQ(clone_info.parent_koid, orig_info.koid);

    // Check that flags are properly set.
    constexpr uint32_t kOriginalFlags = ZX_INFO_VMO_TYPE_PAGED | ZX_INFO_VMO_VIA_HANDLE;
    constexpr uint32_t kCloneFlags =
            ZX_INFO_VMO_TYPE_PAGED | ZX_INFO_VMO_IS_COW_CLONE | ZX_INFO_VMO_VIA_HANDLE;
    ASSERT_EQ(orig_info.flags, kOriginalFlags);
    ASSERT_EQ(new_info.flags, kOriginalFlags);
    ASSERT_EQ(clone_info.flags, kCloneFlags);
}

// Tests that reading from a clone gets the correct data.
TEST(VmoClone2TestCase, Read) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    static constexpr uint32_t kOriginalData = 0xdeadbeef;
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, kOriginalData));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone));

    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, kOriginalData));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, kOriginalData));
}

// Tests that zx_vmo_write into the (clone|parent) doesn't affect the other.
void VmoWriteTestHelper(bool clone_write) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    static constexpr uint32_t kOriginalData = 0xdeadbeef;
    static constexpr uint32_t kNewData = 0xc0ffee;
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, kOriginalData));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone));

    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone_write ? clone : vmo, kNewData));

    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, clone_write ? kOriginalData : kNewData));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, clone_write ? kNewData : kOriginalData));
}

TEST(VmoClone2TestCase, CloneVmoWrite) {
    ASSERT_NO_FATAL_FAILURES(VmoWriteTestHelper(true));
}

TEST(VmoClone2TestCase, ParentVmoWrite) {
    ASSERT_NO_FATAL_FAILURES(VmoWriteTestHelper(false));
}

// Tests that writing into the mapped (clone|parent) doesn't affect the other.
void VmarWriteTestHelper(bool clone_write) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    Mapping vmo_mapping;
    ASSERT_OK(vmo_mapping.Init(vmo, ZX_PAGE_SIZE));

    static constexpr uint32_t kOriginalData = 0xdeadbeef;
    static constexpr uint32_t kNewData = 0xc0ffee;
    *vmo_mapping.ptr() = kOriginalData;

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone));

    Mapping clone_mapping;
    ASSERT_OK(clone_mapping.Init(clone, ZX_PAGE_SIZE));

    *(clone_write ? clone_mapping.ptr(): vmo_mapping.ptr()) =  kNewData;

    ASSERT_EQ(*vmo_mapping.ptr(), clone_write ? kOriginalData : kNewData);
    ASSERT_EQ(*clone_mapping.ptr(), clone_write ? kNewData : kOriginalData);
}

TEST(VmoClone2TestCase, CloneVmarWrite) {
    ASSERT_NO_FATAL_FAILURES(VmarWriteTestHelper(true));
}

TEST(VmoClone2TestCase, ParentVmarWrite) {
    ASSERT_NO_FATAL_FAILURES(VmarWriteTestHelper(false));
}

// Tests that closing the (parent|clone) doesn't affect the other.
void CloseTestHelper(bool close_orig) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    static constexpr uint32_t kOriginalData = 0xdeadbeef;
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, kOriginalData));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone));

    (close_orig ? vmo : clone).reset();

    ASSERT_NO_FATAL_FAILURES(VmoCheck(close_orig ? clone : vmo, kOriginalData));
}

TEST(VmoClone2TestCase, CloseOriginal) {
    constexpr bool kCloseOriginal = true;
    ASSERT_NO_FATAL_FAILURES(CloseTestHelper(kCloseOriginal));
}

TEST(VmoClone2TestCase, CloseClone) {
    constexpr bool kCloseClone = false;
    ASSERT_NO_FATAL_FAILURES(CloseTestHelper(kCloseClone));
}

// Basic memory accounting test that checks vmo memory attribution.
TEST(VmoClone2TestCase, ObjMemAccounting) {
    // Create a vmo, write to both pages, and check the committed stats.
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(2 * ZX_PAGE_SIZE, 0, &vmo));

    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 1, 0));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 1, ZX_PAGE_SIZE));

    ASSERT_EQ(VmoCommittedBytes(vmo), 2 * ZX_PAGE_SIZE);

    // Create a clone and check the initialze committed stats.
    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone));

    ASSERT_EQ(VmoCommittedBytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), 0);

    // Write to the clone and check that that forks a page into the clone.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 2, 0));
    ASSERT_EQ(VmoCommittedBytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), ZX_PAGE_SIZE);

    // Write to the orignal and check that that forks a page into the clone.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 2, ZX_PAGE_SIZE));
    ASSERT_EQ(VmoCommittedBytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), 2 * ZX_PAGE_SIZE);

    // Write to the other pages, which shouldn't affect accounting.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 2, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 2, 0));
    ASSERT_EQ(VmoCommittedBytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), 2 * ZX_PAGE_SIZE);
}

using VmoClone2TestCase = KmemStatsFixture;

// Basic memory accounting test that total memory consumption through kmem.
TEST_F(VmoClone2TestCase, KmemAccounting) {
    uint64_t start_size = KmemVmoMemUsage();
    if (start_size == 0) {
        printf("Root resource not available, skipping\n");
        return;
    }

    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(2 * ZX_PAGE_SIZE, 0, &vmo));

    // An new vmo consumes no pages
    ASSERT_EQ(start_size, KmemVmoMemUsage());

    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 1, 0));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 1, ZX_PAGE_SIZE));

    // Check that the two pages were comitted.
    ASSERT_EQ(start_size + 2 * ZX_PAGE_SIZE, KmemVmoMemUsage());

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone));

    // A clone shouldn't allocate more pages.
    ASSERT_EQ(start_size + 2 * ZX_PAGE_SIZE, KmemVmoMemUsage());

    // Forking a page through the original should allocate a page
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 2, 0));
    ASSERT_EQ(start_size + 3 * ZX_PAGE_SIZE, KmemVmoMemUsage());

    // Forking a page through the clone should allocate a page.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 2, ZX_PAGE_SIZE));
    ASSERT_EQ(start_size + 4 * ZX_PAGE_SIZE, KmemVmoMemUsage());

    // Writing to already-forked pages shouldn't allocate anything
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 2, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 2, 0));
    ASSERT_EQ(start_size + 4 * ZX_PAGE_SIZE, KmemVmoMemUsage());

    // Make sure pages are properly freed on close.
    vmo.reset();
    ASSERT_EQ(start_size + 2 * ZX_PAGE_SIZE, KmemVmoMemUsage());

    clone.reset();
    ASSERT_EQ(start_size, KmemVmoMemUsage());
}

// Tests that writes to a COW'ed zero page work and don't require redundant allocations.
TEST_F(VmoClone2TestCase, ZeroPageWrite) {
    const uint64_t original = KmemVmoMemUsage();

    zx::vmo vmos[4];
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, vmos));

    // Create two clones of the original vmo and one clone of one of those clones.
    ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 1));
    ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 2));
    ASSERT_OK(vmos[1].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 3));

    if (original) {
        ASSERT_EQ(original, KmemVmoMemUsage());
    }

    for (unsigned i = 0; i < 4; i++) {
        ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[i], i + 1));
        for (unsigned j = 0; j < 4; j++) {
            ASSERT_NO_FATAL_FAILURES(VmoCheck(vmos[j], j <= i ? j + 1 : 0));
            ASSERT_EQ(VmoCommittedBytes(vmos[j]), (j <= i ? 1u : 0u) * ZX_PAGE_SIZE);
        }
        if (original) {
            ASSERT_EQ(original + (i + 1) * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }
    }
}

// Tests closing a vmo with the last reference to a mostly forked page.
TEST_F(VmoClone2TestCase, SplitPageClosure) {
    const uint64_t original = KmemVmoMemUsage();

    // Create a chain of clones.
    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(1, &vmo));

    zx::vmo clone1;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 1 * ZX_PAGE_SIZE, &clone1));

    zx::vmo clone2;
    ASSERT_OK(clone1.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 1 * ZX_PAGE_SIZE, &clone2));

    // Fork the page into the two clones.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone1, 3));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone2, 4));

    // The page should be unique in each of the 3 vmos.
    ASSERT_EQ(VmoCommittedBytes(vmo), ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone1), ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone2), ZX_PAGE_SIZE);
    if (original) {
        ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }

    // Close the original vmo, check that data is correct and things were freed.
    vmo.reset();
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone1, 3));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 4));
    ASSERT_EQ(VmoCommittedBytes(clone1), ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone2), ZX_PAGE_SIZE);
    if (original) {
        ASSERT_EQ(original + 2 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }

    // Close the first clone, check that data is correct and things were freed.
    clone1.reset();
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 4));
    ASSERT_EQ(VmoCommittedBytes(clone2), ZX_PAGE_SIZE);
    if (original) {
        ASSERT_EQ(original + ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
}

// Tests that a clone with an offset accesses the right data and doesn't
// unnecessarily retain pages when the parent is closed.
TEST_F(VmoClone2TestCase, Offset) {
    const uint64_t original = KmemVmoMemUsage();

    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(3, &vmo));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                               ZX_PAGE_SIZE, 3 * ZX_PAGE_SIZE, &clone));

    // Check that the child has the right data.
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 2));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 3, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 0, 2 * ZX_PAGE_SIZE));

    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 4, ZX_PAGE_SIZE));

    vmo.reset();

    // Check that we don't change the child.
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 2));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 4, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 0, 2 * ZX_PAGE_SIZE));

    // Check that the clone doesn't unnecessarily retain pages.
    ASSERT_EQ(VmoCommittedBytes(clone), 2 * ZX_PAGE_SIZE);
    if (original) {
        ASSERT_EQ(original + 2 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
}

// Tests writing to the clones of a clone created with an offset.
TEST_F(VmoClone2TestCase, OffsetTest2) {
    const uint64_t original = KmemVmoMemUsage();

    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(4, &vmo));

    // Create a clone at an offset.
    zx::vmo offset_clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                               ZX_PAGE_SIZE, 3 * ZX_PAGE_SIZE, &offset_clone));

    // Create two clones to fully divide the previous partial clone.
    zx::vmo clone1;
    ASSERT_OK(offset_clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                        0, 2 * ZX_PAGE_SIZE, &clone1));

    zx::vmo clone2;
    ASSERT_OK(offset_clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                        2 * ZX_PAGE_SIZE, 1 * ZX_PAGE_SIZE, &clone2));

    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone1, 2));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone1, 3, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 4));

    // Write to one of the pages in the offset clone, close the clone, and check that
    // things are still correct.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(offset_clone, 4, ZX_PAGE_SIZE));
    offset_clone.reset();

    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone1, 2));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone1, 3, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 4));

    // Check that the total amount of allocated memory is correct. It's not defined how
    // many pages should be blamed to vmo and clone1 after closing offset_clone (which was
    // forked), but no vmo can be blamed for more pages than its total size.
    const uint64_t kImplCost1 = 4 * ZX_PAGE_SIZE;
    const uint64_t kImplCost2 = ZX_PAGE_SIZE;
    ASSERT_EQ(VmoCommittedBytes(vmo), kImplCost1);
    ASSERT_EQ(VmoCommittedBytes(clone1), kImplCost2);
    ASSERT_EQ(VmoCommittedBytes(clone2), 0);
    static_assert(kImplCost1 <= 4 * ZX_PAGE_SIZE && kImplCost2 <= 2 * ZX_PAGE_SIZE);
    if (original) {
        ASSERT_EQ(original + kImplCost1 + kImplCost2, KmemVmoMemUsage());
    }

    // Clone the first clone and check that any extra pages were freed.
    clone1.reset();
    ASSERT_EQ(VmoCommittedBytes(vmo), 4 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone2), 0);
    if (original) {
        ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }

    clone2.reset();

    if (original) {
        ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
}

// Tests writes to a page in a clone that is offset from the original and has a clone itself.
TEST_F(VmoClone2TestCase, OffsetProgressiveWrite) {
    const uint64_t original = KmemVmoMemUsage();

    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(2, &vmo));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                               ZX_PAGE_SIZE, 2 * ZX_PAGE_SIZE, &clone));

    // Check that the child has the right data.
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 2));

    // Write to the clone and check that everything still has the correct data.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 3));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 3));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, 1));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, 2, ZX_PAGE_SIZE));

    zx::vmo clone2;
    ASSERT_OK(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                 ZX_PAGE_SIZE, ZX_PAGE_SIZE, &clone2));

    // Write to the clone again, and check that the write doesn't consume any
    // extra pages as the page isn't accessible by clone2.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 4));

    ASSERT_EQ(VmoCommittedBytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone2), 0);
    if (original) {
        ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }

    // Reset the original vmo and clone2, and make sure that the clone stays correct.
    vmo.reset();
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 4));

    clone2.reset();
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 4));

    // Check that the clone doesn't unnecessarily retain pages.
    ASSERT_EQ(VmoCommittedBytes(clone), ZX_PAGE_SIZE);
    if (original) {
        ASSERT_EQ(original + ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
}

// Tests that a clone of a clone which overflows its parent properly interacts with
// both of its ancestors (i.e. the orignal vmo and the first clone).
TEST_F(VmoClone2TestCase, Overflow) {
    const uint64_t original = KmemVmoMemUsage();

    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(1, &vmo));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone));

    // Check that the child has the right data.
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 1));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 0, ZX_PAGE_SIZE));

    // Write to the child and then clone it.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 2, ZX_PAGE_SIZE));
    zx::vmo clone2;
    ASSERT_OK(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 3 * ZX_PAGE_SIZE, &clone2));

    // Check that the second clone is correct.
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 1));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 2, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 0, 2 * ZX_PAGE_SIZE));

    // Write the dedicated page in 2nd child and then check that accounting is correct.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone2, 3, 2 * ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 3, 2 * ZX_PAGE_SIZE));

    // Check that accounting is correct.
    ASSERT_EQ(VmoCommittedBytes(vmo), ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone2), ZX_PAGE_SIZE);

    if (original) {
        ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }

    // Completely fork the final clone and check that things are correct.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone2, 4, 0));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone2, 5, ZX_PAGE_SIZE));

    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, 1, 0));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 1, 0));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 2, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 4, 0));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 5, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 3, 2 * ZX_PAGE_SIZE));

    // Check that the total amount of allocated memory is correct. The amount allocated
    // is implementation dependent, but no vmo can be blamed for more pages than its total size.
    constexpr uint64_t kImplCost1 = ZX_PAGE_SIZE;
    constexpr uint64_t kImplCost2 = 2 * ZX_PAGE_SIZE;
    constexpr uint64_t kImplCost3 = 3 * ZX_PAGE_SIZE;
    static_assert(kImplCost1 <= ZX_PAGE_SIZE && kImplCost2 <= 2 * ZX_PAGE_SIZE
            && kImplCost3 <= 3 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(vmo), kImplCost1);
    ASSERT_EQ(VmoCommittedBytes(clone), kImplCost2);
    ASSERT_EQ(VmoCommittedBytes(clone2), kImplCost3);
    if (original) {
        ASSERT_EQ(original + kImplCost1 + kImplCost2 + kImplCost3, KmemVmoMemUsage());
    }

    // Close the middle clone and check that things are still correct. Memory usage
    // between the two vmos is not implementation dependent.
    clone.reset();

    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, 1, 0));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 4));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 5, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 3, 2 * ZX_PAGE_SIZE));

    ASSERT_EQ(VmoCommittedBytes(vmo), ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone2), 3 * ZX_PAGE_SIZE);
    if (original) {
        ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
}

// Tests that a small clone doesn't require allocations for pages which it doesn't
// have access to and that unneeded pages get freed if the original vmo is closed.
TEST_F(VmoClone2TestCase, SmallClone) {
    const uint64_t original = KmemVmoMemUsage();

    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(3, &vmo));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                               ZX_PAGE_SIZE, ZX_PAGE_SIZE, &clone));

    // Check that the child has the right data.
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 2));

    // Check that a write into the original vmo out of bounds of the first clone
    // doesn't allocate any memory.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 4, 0));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 5, 2 * ZX_PAGE_SIZE));
    ASSERT_EQ(VmoCommittedBytes(vmo), 3 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), 0);
    if (original) {
        ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }

    vmo.reset();

    // Check that clone has the right data after closing the parent and that
    // all the extra pages are freed.
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 2));
    ASSERT_EQ(VmoCommittedBytes(clone), ZX_PAGE_SIZE);
    if (original) {
        ASSERT_EQ(original + ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
}

// Tests that a small clone properly interrupts access into the parent.
TEST(VmoClone2TestCase, SmallCloneChild) {
    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(3, &vmo));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                               ZX_PAGE_SIZE, ZX_PAGE_SIZE, &clone));

    // Check that the child has the right data.
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 2));

    // Create a clone of the first clone and check that it has the right data (incl. that
    // it can't access the original vmo).
    zx::vmo clone2;
    ASSERT_OK(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone2));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 2));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 0, ZX_PAGE_SIZE));
}

// Tests that closing a vmo with multiple small clones properly frees pages.
TEST_F(VmoClone2TestCase, SmallClones) {
    const uint64_t original = KmemVmoMemUsage();

    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(3, &vmo));

    // Create a clone and populate one of its pages
    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 4, ZX_PAGE_SIZE));

    // Create a second clone
    zx::vmo clone2;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 1 * ZX_PAGE_SIZE, &clone2));

    ASSERT_EQ(VmoCommittedBytes(vmo), 3 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone2), 0);
    if (original) {
        ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }

    vmo.reset();

    // The inaccessible 3rd page should be freed, and vmo's copy of page 2 should be freed. The
    // fact that both are blamed to clone (vs 1 being blamed to clone2) is implementation
    // dependent.
    constexpr uint64_t kImplClone1Cost = 2 * ZX_PAGE_SIZE;
    constexpr uint64_t kImplClone2Cost = 0;
    static_assert(kImplClone1Cost <= 2 * ZX_PAGE_SIZE && kImplClone2Cost <= ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), kImplClone1Cost);
    ASSERT_EQ(VmoCommittedBytes(clone2), kImplClone2Cost);
    if (original) {
        ASSERT_EQ(original + 2 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
}

// Tests that disjoint clones work (i.e. create multiple clones, none of which
// overlap) and that they don't unnecessarily retain/allocate memory after
// closing the original VMO. This tests two cases - resetting the original vmo
// before writing to the clones and resetting the original vmo after writing to
// the clones.
struct VmoCloneDisjointClonesTests : public KmemStatsFixture {
    static void DisjointClonesTest(bool early_close) {
        const uint64_t original = KmemVmoMemUsage();

        zx::vmo vmo;
        ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(4, &vmo));

        // Create a disjoint clone for each page in the original vmo: 2 direct and 2 through another
        // intermediate COW clone.
        zx::vmo clone;
        ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   1 * ZX_PAGE_SIZE, 2 * ZX_PAGE_SIZE, &clone));

        zx::vmo leaf_clones[4];
        ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, leaf_clones));
        ASSERT_OK(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, leaf_clones + 1));
        ASSERT_OK(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                     ZX_PAGE_SIZE, ZX_PAGE_SIZE, leaf_clones + 2));
        ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   3 * ZX_PAGE_SIZE, ZX_PAGE_SIZE, leaf_clones + 3));

        if (early_close) {
            vmo.reset();
            clone.reset();
        }

        // Check that each clone's has the correct data and then write to the clone.
        for (unsigned i = 0; i < 4; i++) {
            ASSERT_NO_FATAL_FAILURES(VmoCheck(leaf_clones[i], i + 1));
            ASSERT_NO_FATAL_FAILURES(VmoWrite(leaf_clones[i], i + 5));
        }

        if (!early_close) {
            // The number of allocated pages is implementation dependent, but it must be less
            // than the total user-visible vmo size.
            constexpr uint32_t kImplTotalPages = 10;
            static_assert(kImplTotalPages <= 10);
            if (original) {
                ASSERT_EQ(original + 10 * ZX_PAGE_SIZE, KmemVmoMemUsage());
            }
            vmo.reset();
            clone.reset();
        }

        // Check that the clones have the correct data and that nothing
        // is unnecessary retained/allocated.
        for (unsigned i = 0; i < 4; i++) {
            ASSERT_NO_FATAL_FAILURES(VmoCheck(leaf_clones[i], i + 5));
            ASSERT_EQ(VmoCommittedBytes(leaf_clones[i]), ZX_PAGE_SIZE);
        }
        if (original) {
            ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }
    }
};

TEST_F(VmoCloneDisjointClonesTests, DisjointCloneEarlyClose) {
    ASSERT_NO_FATAL_FAILURES(DisjointClonesTest(true));
}

TEST_F(VmoCloneDisjointClonesTests, DisjointCloneLateClose) {
    ASSERT_NO_FATAL_FAILURES(DisjointClonesTest(false));
}

// A second disjoint clone test that checks that closing the disjoint clones which haven't
// yet been written to doesn't affect the contents of other disjoint clones.
TEST_F(VmoClone2TestCase, DisjointCloneTest2) {
    const uint64_t original = KmemVmoMemUsage();

    auto test_fn = [original](uint32_t perm[]) -> void {
        zx::vmo vmo;
        ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(4, &vmo));

        // Create a disjoint clone for each page in the orignal vmo: 2 direct and 2 through another
        // intermediate COW clone.
        zx::vmo clone;
        ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   1 * ZX_PAGE_SIZE, 2 * ZX_PAGE_SIZE, &clone));

        zx::vmo leaf_clones[4];
        ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   0, ZX_PAGE_SIZE, leaf_clones));
        ASSERT_OK(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                     0, ZX_PAGE_SIZE, leaf_clones + 1));
        ASSERT_OK(clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                     ZX_PAGE_SIZE, ZX_PAGE_SIZE, leaf_clones + 2));
        ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                   3 * ZX_PAGE_SIZE, ZX_PAGE_SIZE, leaf_clones + 3));

        vmo.reset();
        clone.reset();

        // Check that each clone's has the correct data and then write to the clone.
        for (unsigned i = 0; i < 4; i++) {
            ASSERT_NO_FATAL_FAILURES(VmoCheck(leaf_clones[i], i + 1));
        }

        // Nothing should have been allocated by the writes.
        if (original) {
            ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }

        // Close the clones in the order specified by |perm|, and at each step
        // check the rest of the clones.
        bool closed[4] = {};
        for (unsigned i = 0; i < 4; i++) {
            leaf_clones[perm[i]].reset();
            closed[perm[i]] = true;

            for (unsigned j = 0; j < 4; j++) {
                if (!closed[j]) {
                    ASSERT_NO_FATAL_FAILURES(VmoCheck(leaf_clones[j], j + 1));
                    ASSERT_EQ(VmoCommittedBytes(leaf_clones[j]), ZX_PAGE_SIZE);
                }
            }
            if (original) {
                ASSERT_EQ(original + (3 - i) * ZX_PAGE_SIZE, KmemVmoMemUsage());
            }
        }
    };

    ASSERT_NO_FATAL_FAILURES(CallPermutations(test_fn, 4));
}

// Tests a case where a clone is written to and then a series of subsequent clones
// are created with various offsets and sizes. This test is constructed to catch issues
// due to partial COW releases in the current implementation.
TEST(VmoClone2TestCase, DisjointCloneProgressive) {
    zx::vmo vmo, main_clone, clone1, clone2, clone3, clone4;

    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(6, &vmo));

    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                 ZX_PAGE_SIZE, 5 * ZX_PAGE_SIZE, &main_clone));

    ASSERT_NO_FATAL_FAILURES(VmoWrite(main_clone, 7, 3 * ZX_PAGE_SIZE));

    // A clone which references the written page.
    ASSERT_OK(main_clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                 1 * ZX_PAGE_SIZE, 4 * ZX_PAGE_SIZE, &clone1));
    // A clone after the written page.
    ASSERT_OK(main_clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                 4 * ZX_PAGE_SIZE, 1 * ZX_PAGE_SIZE, &clone2));
    // A clone before the written page.
    ASSERT_OK(main_clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                 2 * ZX_PAGE_SIZE, 1 * ZX_PAGE_SIZE, &clone3));
    // A clone which doesn't reference any pages, but it needs to be in the clone tree.
    ASSERT_OK(main_clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                 10 * ZX_PAGE_SIZE, 1 * ZX_PAGE_SIZE, &clone4));

    main_clone.reset();
    clone1.reset();
    clone3.reset();
    clone4.reset();
    clone2.reset();

    zx::vmo last_clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 6 * ZX_PAGE_SIZE, &last_clone));
    for (unsigned i = 0; i < 6; i++) {
        ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, i + 1, i * ZX_PAGE_SIZE));
        ASSERT_NO_FATAL_FAILURES(VmoCheck(last_clone, i + 1, i * ZX_PAGE_SIZE));
    }

    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 8, 4 * ZX_PAGE_SIZE));

    for (unsigned i = 0; i < 6; i++) {
        ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, i == 4 ? 8 : i + 1, i * ZX_PAGE_SIZE));
        ASSERT_NO_FATAL_FAILURES(VmoCheck(last_clone, i + 1, i * ZX_PAGE_SIZE));
    }
}

enum class Contiguity {
    Contig,
    NonContig,
};

enum class ResizeTarget {
    Parent,
    Child,
};

// Tests that resizing a (clone|cloned) vmo frees unnecessary pages.
class VmoCloneResizeTests : public KmemStatsFixture {
 protected:
    static void ResizeTest(Contiguity contiguity, ResizeTarget target) {
        bool contiguous = contiguity == Contiguity::Contig;
        bool resize_child = target == ResizeTarget::Child;

        const uint64_t original = KmemVmoMemUsage();
        if (contiguous && original == 0) {
            printf("Root resource not available, skipping\n");
            return;
        }

        // Create a vmo and a clone of the same size.
        zx::iommu iommu;
        zx::bti bti;
        zx::vmo vmo;
        if (contiguous) {
            zx_iommu_desc_dummy_t desc;
            ASSERT_OK(zx::iommu::create(RootResource(), ZX_IOMMU_TYPE_DUMMY,
                                      &desc, sizeof(desc), &iommu));
            ASSERT_OK(zx::bti::create(iommu, 0, 0xdeadbeef, &bti));
            ASSERT_OK(zx::vmo::create_contiguous(bti, 4 * ZX_PAGE_SIZE, 0, &vmo));
        } else {
            ASSERT_OK(zx::vmo::create(4 * ZX_PAGE_SIZE, ZX_VMO_RESIZABLE, &vmo));
        }

        for (unsigned i = 0; i < 4; i++) {
            ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, i + 1, i * ZX_PAGE_SIZE));
        }

        zx::vmo clone;
        ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2 | ZX_VMO_CHILD_RESIZABLE,
                                   0, 4 * ZX_PAGE_SIZE, &clone));

        // Write to one page in each vmo.
        ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 5, ZX_PAGE_SIZE));
        ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 5, 2 * ZX_PAGE_SIZE));

        ASSERT_EQ(VmoCommittedBytes(vmo), 4 * ZX_PAGE_SIZE);
        ASSERT_EQ(VmoCommittedBytes(clone), 2 * ZX_PAGE_SIZE);
        if (original) {
            ASSERT_EQ(original + 6 * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }

        const zx::vmo& resize_target = resize_child ? clone : vmo;
        const zx::vmo& original_size_vmo = resize_child ? vmo : clone;

        if (contiguous && !resize_child) {
            // Contiguous vmos can't be resizable.
            ASSERT_EQ(resize_target.set_size(ZX_PAGE_SIZE), ZX_ERR_UNAVAILABLE);
            return;
        } else {
            ASSERT_OK(resize_target.set_size(ZX_PAGE_SIZE));
        }

        // Check that the data in both vmos is correct.
        for (unsigned i = 0; i < 4; i++) {
            // The index of original_size_vmo's page we wrote to depends on which vmo it is
            uint32_t written_page_idx = resize_child ? 1 : 2;
            // If we're checking the page we wrote to, look for 5, otherwise look for the tagged value.
            uint32_t expected_val = i == written_page_idx ? 5 : i + 1;
            ASSERT_NO_FATAL_FAILURES(VmoCheck(original_size_vmo, expected_val, i * ZX_PAGE_SIZE));
        }
        ASSERT_NO_FATAL_FAILURES(VmoCheck(resize_target, 1));

        // Check that pages are properly allocated/blamed.
        ASSERT_EQ(VmoCommittedBytes(vmo), (resize_child ? 4 : 1) * ZX_PAGE_SIZE);
        ASSERT_EQ(VmoCommittedBytes(clone), (resize_child ? 0 : 3) * ZX_PAGE_SIZE);
        if (original) {
            ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }

        // Check that growing the shrunk vmo doesn't expose anything.
        ASSERT_OK(resize_target.set_size(2 * ZX_PAGE_SIZE));
        ASSERT_NO_FATAL_FAILURES(VmoCheck(resize_target, 0, ZX_PAGE_SIZE));

        // Check that writes into the non-resized vmo don't require allocating pages.
        ASSERT_NO_FATAL_FAILURES(VmoWrite(original_size_vmo, 6, 3 * ZX_PAGE_SIZE));
        ASSERT_EQ(VmoCommittedBytes(vmo), (resize_child ? 4 : 1) * ZX_PAGE_SIZE);
        ASSERT_EQ(VmoCommittedBytes(clone), (resize_child ? 0 : 3) * ZX_PAGE_SIZE);
        if (original) {
            ASSERT_EQ(original + 4 * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }

        // Check that closing the non-resized vmo frees the the inaccessible pages.
        if (contiguous) {
            ASSERT_NO_FATAL_FAILURES(CheckContigState<4>(bti, vmo));
        }

        // Check that closing the non-resized VMO frees the the inaccessible pages.
        if (resize_child) {
            vmo.reset();
        } else {
            clone.reset();
        }

        ASSERT_NO_FATAL_FAILURES(VmoCheck(resize_target, 1));
        ASSERT_EQ(VmoCommittedBytes(resize_target), ZX_PAGE_SIZE);
        if (original) {
            ASSERT_EQ(original + ZX_PAGE_SIZE, KmemVmoMemUsage());
        }
    }
};

TEST_F(VmoCloneResizeTests, ResizeChild) {
    ASSERT_NO_FATAL_FAILURES(ResizeTest(Contiguity::NonContig, ResizeTarget::Child));
}

TEST_F(VmoCloneResizeTests, ResizeOriginal) {
    ASSERT_NO_FATAL_FAILURES(ResizeTest(Contiguity::NonContig, ResizeTarget::Parent));
}

// Tests that growing a clone exposes zeros and doesn't consume memory on parent writes.
TEST_F(VmoClone2TestCase, ResizeGrow) {
    const uint64_t original = KmemVmoMemUsage();

    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(2, &vmo));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2 | ZX_VMO_CHILD_RESIZABLE,
                               0, ZX_PAGE_SIZE, &clone));

    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 1));

    ASSERT_OK(clone.set_size(2 * ZX_PAGE_SIZE));

    // Check that the new page in the clone is 0.
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 0, ZX_PAGE_SIZE));

    // Check that writing to the second page of the original vmo doesn't require
    // forking a page and doesn't affect the clone.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 3, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 0, ZX_PAGE_SIZE));

    ASSERT_EQ(VmoCommittedBytes(vmo), 2 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), 0);
    if (original) {
        ASSERT_EQ(original + 2 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
}

// Tests that a vmo with a child that has a non-zero offset can be truncated without
// affecting the child.
TEST_F(VmoClone2TestCase, ResizeOffsetChild) {
    const uint64_t original = KmemVmoMemUsage();

    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(3, &vmo));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, ZX_PAGE_SIZE, ZX_PAGE_SIZE, &clone));

    ASSERT_OK(vmo.set_size(0));

    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 2));
    ASSERT_EQ(VmoCommittedBytes(vmo), 0);
    ASSERT_EQ(VmoCommittedBytes(clone), ZX_PAGE_SIZE);
    if (original) {
        ASSERT_EQ(original + ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
}

// Tests that resize works with multiple disjoint children.
TEST_F(VmoClone2TestCase, ResizeDisjointChild) {
    const uint64_t original = KmemVmoMemUsage();

    auto test_fn = [original](uint32_t perm[]) -> void {
        zx::vmo vmo;
        ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(3, &vmo));

        // Clone one clone for each page.
        zx::vmo clones[3];
        for (unsigned i = 0; i < 3; i++) {
            ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2 | ZX_VMO_CHILD_RESIZABLE,
                                       i * ZX_PAGE_SIZE, ZX_PAGE_SIZE, clones + i));
            ASSERT_NO_FATAL_FAILURES(VmoCheck(clones[i], i + 1));
            ASSERT_EQ(VmoCommittedBytes(clones[i]), 0);
        }

        // Nothing new should have been allocated and everything still belongs to the first vmo.
        ASSERT_EQ(VmoCommittedBytes(vmo), 3 * ZX_PAGE_SIZE);
        if (original) {
            ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }

        // Shrink two of the clones and then the original, and then check that the
        // remaining clone is okay.
        ASSERT_OK(clones[perm[0]].set_size(0));
        ASSERT_OK(clones[perm[1]].set_size(0));
        ASSERT_OK(vmo.set_size(0));

        ASSERT_NO_FATAL_FAILURES(VmoCheck(clones[perm[2]], perm[2] + 1));
        ASSERT_EQ(VmoCommittedBytes(vmo), 0);
        ASSERT_EQ(VmoCommittedBytes(clones[perm[0]]), 0);
        ASSERT_EQ(VmoCommittedBytes(clones[perm[1]]), 0);
        ASSERT_EQ(VmoCommittedBytes(clones[perm[2]]), ZX_PAGE_SIZE);
        if (original) {
            ASSERT_EQ(original + 1 * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }

        ASSERT_OK(clones[perm[2]].set_size(0));

        ASSERT_EQ(VmoCommittedBytes(clones[perm[2]]), 0);
        if (original) {
            ASSERT_EQ(original, KmemVmoMemUsage());
        }
    };

    ASSERT_NO_FATAL_FAILURES(CallPermutations(test_fn, 3));
}

// Tests that resize works when with progressive writes.
TEST_F(VmoClone2TestCase, ResizeMultipleProgressive) {
    uint64_t original = KmemVmoMemUsage();

    zx::vmo vmo;
    ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(3, &vmo));

    // Clone the vmo and fork a page into both.
    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2 | ZX_VMO_CHILD_RESIZABLE,
                               0, 2 * ZX_PAGE_SIZE, &clone));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 4, 0 * ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(clone, 5, 1 * ZX_PAGE_SIZE));

    // Create another clone of the original vmo.
    zx::vmo clone2;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone2));

    // Resize the first clone, check the contents and allocations.
    ASSERT_OK(clone.set_size(0));

    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, 4, 0 * ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, 2, 1 * ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, 3, 2 * ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 4, 0 * ZX_PAGE_SIZE));

    // Nothing new should have been allocated and everything still belongs to the first vmo.
    ASSERT_EQ(VmoCommittedBytes(vmo), 3 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone), 0 * ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(clone2), 0 * ZX_PAGE_SIZE);
    if (original) {
        ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }

    // Resize the original vmo and make sure it frees the necessary pages. Which of the clones
    // gets blamed is implementation dependent.
    ASSERT_OK(vmo.set_size(0));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone2, 4, 0 * ZX_PAGE_SIZE));

    constexpr uint64_t kImplClone1Cost = 0;
    constexpr uint64_t kImplClone2Cost = ZX_PAGE_SIZE;
    static_assert(kImplClone1Cost + kImplClone2Cost == ZX_PAGE_SIZE);
    ASSERT_EQ(VmoCommittedBytes(vmo), 0);
    ASSERT_EQ(VmoCommittedBytes(clone), kImplClone1Cost);
    ASSERT_EQ(VmoCommittedBytes(clone2), kImplClone2Cost);
    if (original) {
        ASSERT_EQ(original + ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
}

// Tests the basic operation of the ZX_VMO_ZERO_CHILDREN signal.
TEST(VmoClone2TestCase, Children) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    zx_signals_t o;
    ASSERT_OK(vmo.wait_one(ZX_VMO_ZERO_CHILDREN, zx::time::infinite_past(), &o));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone));

    ASSERT_EQ(vmo.wait_one(ZX_VMO_ZERO_CHILDREN, zx::time::infinite_past(), &o),
              ZX_ERR_TIMED_OUT);
    ASSERT_OK(clone.wait_one(ZX_VMO_ZERO_CHILDREN, zx::time::infinite_past(), &o));

    clone.reset();

    ASSERT_OK(vmo.wait_one(ZX_VMO_ZERO_CHILDREN, zx::time::infinite_past(), &o));
}

// Tests that child count and zero child signals for when there are many children. Tests
// with closing the children both in the order they were created and the reverse order.
void ManyChildrenTestHelper(bool reverse_close) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    static constexpr uint32_t kCloneCount = 5;
    zx::vmo clones[kCloneCount];

    for (unsigned i = 0; i < kCloneCount; i++) {
        ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, clones + i));
        ASSERT_EQ(VmoNumChildren(vmo), i + 1);
    }

    if (reverse_close) {
        for (unsigned i = kCloneCount - 1; i != UINT32_MAX; i--) {
            clones[i].reset();
            ASSERT_EQ(VmoNumChildren(vmo), i);
        }
    } else {
        for (unsigned i = 0; i < kCloneCount; i++) {
            clones[i].reset();
            ASSERT_EQ(VmoNumChildren(vmo), kCloneCount - (i + 1));
        }
    }

    zx_signals_t o;
    ASSERT_OK(vmo.wait_one(ZX_VMO_ZERO_CHILDREN, zx::time::infinite_past(), &o));
}

TEST(VmoClone2TestCase, ManyChildren) {
    bool kForwardClose = false;
    ASSERT_NO_FATAL_FAILURES(ManyChildrenTestHelper(kForwardClose));
}

TEST(VmoClone2TestCase, ManyChildrenRevClose) {
    bool kReverseClose = true;
    ASSERT_NO_FATAL_FAILURES(ManyChildrenTestHelper(kReverseClose));
}

// Creates a collection of clones and writes to their mappings in every permutation order
// to make sure that no order results in a bad read.
TEST(VmoClone2TestCase, ManyCloneMapping) {
    constexpr uint32_t kNumElts = 4;

    auto test_fn = [](uint32_t perm[]) -> void {
        zx::vmo vmos[kNumElts];
        ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, vmos));

        constexpr uint32_t kOriginalData = 0xdeadbeef;
        constexpr uint32_t kNewData = 0xc0ffee;

        ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[0], kOriginalData));

        ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 1));
        ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 2));
        ASSERT_OK(vmos[1].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 3));

        Mapping mappings[kNumElts] = {};

        // Map the vmos and make sure they're all correct.
        for (unsigned i = 0; i < kNumElts; i++) {
            ASSERT_OK(mappings[i].Init(vmos[i], ZX_PAGE_SIZE));
            ASSERT_EQ(*mappings[i].ptr(), kOriginalData);
        }

        // Write to the pages in the order specified by |perm| and validate.
        bool written[kNumElts] = {};
        for (unsigned i = 0; i < kNumElts; i++) {
            uint32_t cur_idx = perm[i];
            *mappings[cur_idx].ptr() = kNewData;
            written[cur_idx] = true;

            for (unsigned j = 0; j < kNumElts; j++) {
                ASSERT_EQ(written[j] ? kNewData : kOriginalData, *mappings[j].ptr());
            }
        }
    };

    ASSERT_NO_FATAL_FAILURES(CallPermutations(test_fn, kNumElts));
}

// Tests that a chain of clones where some have offsets works.
TEST(VmoClone2TestCase, ManyCloneOffset) {
    zx::vmo vmo;
    zx::vmo clone1;
    zx::vmo clone2;

    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 1));

    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone1));
    ASSERT_OK(clone1.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, ZX_PAGE_SIZE, ZX_PAGE_SIZE,
                                  &clone2));

    VmoWrite(clone1, 1);

    clone1.reset();

    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, 1));
}

// Tests that a chain of clones where some have offsets doesn't mess up
// the page migration logic.
TEST_F(VmoClone2TestCase, ManyCloneMappingOffset) {
    uint64_t original = KmemVmoMemUsage();

    zx::vmo vmos[4];
    ASSERT_OK(zx::vmo::create(2 * ZX_PAGE_SIZE, 0, vmos));


    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[0], 1));

    ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, vmos + 1));
    ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, ZX_PAGE_SIZE, ZX_PAGE_SIZE,
                                   vmos + 2));
    ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, vmos + 3));

    Mapping mappings[4] = {};

    // Map the vmos and make sure they're all correct.
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_OK(mappings[i].Init(vmos[i], ZX_PAGE_SIZE));
        if (i != 2) {
            ASSERT_EQ(*mappings[i].ptr(), 1);
        }
    }

    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[3], 2));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[1], 3));

    ASSERT_EQ(*mappings[1].ptr(), 3);
    ASSERT_EQ(*mappings[3].ptr(), 2);
    ASSERT_EQ(*mappings[0].ptr(), 1);

    if (original) {
        ASSERT_EQ(original + 3 * ZX_PAGE_SIZE, KmemVmoMemUsage());
    }
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_EQ(VmoCommittedBytes(vmos[i]), (i != 2) * ZX_PAGE_SIZE);
    }
}

// Tests the correctness and memory consumption of a chain of progressive clones, and
// ensures that memory is properly discarded by closing/resizing the vmos.
struct ProgressiveCloneDiscardTests : public KmemStatsFixture {
    static void ProgressiveCloneDiscardTest(bool close) {
        const uint64_t original = KmemVmoMemUsage();

        constexpr uint64_t kNumClones = 6;
        zx::vmo vmos[kNumClones];
        ASSERT_NO_FATAL_FAILURES(InitPageTaggedVmo(kNumClones, vmos));

        ASSERT_EQ(VmoCommittedBytes(vmos[0]), kNumClones * ZX_PAGE_SIZE);
        if (original) {
            ASSERT_EQ(original + kNumClones * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }

        // Repeatedly clone the vmo while simultaniously changing it. Then check the total memory
        // consumption. This must consume less pages than manually duplicating the vmo, but the
        // precise amount consumed and the amount blamed to each vmo is implementation dependent.
        // Furthermore, the amount blamed should match the amount allocated.
        for (unsigned i = 1; i < kNumClones; i++) {
            ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2 | ZX_VMO_CHILD_RESIZABLE,
                                           0, kNumClones * ZX_PAGE_SIZE, vmos + i));
            ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[i], kNumClones + 2, i * ZX_PAGE_SIZE));
        }
        constexpr uint64_t kImplTotalPages = (kNumClones * (kNumClones + 1)) / 2;
        static_assert(kImplTotalPages <= kNumClones * kNumClones);
        for (unsigned i = 0; i < kNumClones; i++) {
            ASSERT_EQ(VmoCommittedBytes(vmos[i]), (kNumClones - i) * ZX_PAGE_SIZE);
        }
        if (original) {
            ASSERT_EQ(original + kImplTotalPages * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }

        // Check that the vmos have the right content.
        for (unsigned i = 0; i < kNumClones; i++) {
            for (unsigned j = 0; j < kNumClones; j++) {
                uint32_t expected = (i != 0 && j == i) ? kNumClones + 2 : j + 1;
                ASSERT_NO_FATAL_FAILURES(VmoCheck(vmos[i], expected, j * ZX_PAGE_SIZE));
            }
        }

        // Close the original vmo and check for correctness.
        if (close) {
            vmos[0].reset();
        } else {
            ASSERT_OK(vmos[0].set_size(0));
        }

        for (unsigned i = 1; i < kNumClones; i++) {
            for (unsigned j = 0; j < kNumClones; j++) {
                ASSERT_NO_FATAL_FAILURES(
                    VmoCheck(vmos[i], j == i ? kNumClones + 2 : j + 1, j * ZX_PAGE_SIZE));
            }
        }

        // Check that some memory was freed and that all allocated memory is accounted for. The total
        // amount retained is implementation dependent, but it must be less than manually copying
        // the vmo. The amount blamed to each vmo does not need to be the same for both version
        // of this test.
        constexpr uint64_t kImplRemainingPages = kImplTotalPages - 1;
        static_assert(kImplRemainingPages <= kNumClones * (kNumClones - 1));
        uint64_t observed = 0;
        for (unsigned i = 1; i < kNumClones; i++) {
            observed += VmoCommittedBytes(vmos[i]);
        }
        if (original) {
            ASSERT_EQ(original + observed, KmemVmoMemUsage());
        }
        ASSERT_EQ(observed, kImplRemainingPages * ZX_PAGE_SIZE);

        // Close all but the last two vmos. The total amount of memory consumed by the two remaining
        // vmos is *not* implementation dependent.
        for (unsigned i = 1; i < kNumClones - 2; i++) {
            if (close) {
                vmos[i].reset();
            } else {
                ASSERT_OK(vmos[i].set_size(0));
            }
        }

        for (unsigned i = kNumClones - 2; i < kNumClones; i++) {
            for (unsigned j = 0; j < kNumClones; j++) {
                ASSERT_NO_FATAL_FAILURES(
                    VmoCheck(vmos[i], j == i ? kNumClones + 2 : j + 1, j * ZX_PAGE_SIZE));
            }
        }
        if (original) {
            ASSERT_EQ(original + (kNumClones + 2) * ZX_PAGE_SIZE, KmemVmoMemUsage());
        }
    }
};

TEST_F(ProgressiveCloneDiscardTests, ProgressiveCloneClose) {
    constexpr bool kClose = true;
    ASSERT_NO_FATAL_FAILURES(ProgressiveCloneDiscardTest(kClose));
}

TEST_F(ProgressiveCloneDiscardTests, ProgressiveCloneTruncate) {
    constexpr bool kTruncate = false;
    ASSERT_NO_FATAL_FAILURES(ProgressiveCloneDiscardTest(kTruncate));
}

// Tests that a contiguous VMO remains contiguous even after writes to its clones.
TEST_F(VmoClone2TestCase, ContiguousVmo) {
    if (!RootResource()) {
        printf("Root resource not available, skipping\n");
        return;
    }

    zx::iommu iommu;
    zx::bti bti;
    zx_iommu_desc_dummy_t desc;
    ASSERT_OK(zx::iommu::create(RootResource(), ZX_IOMMU_TYPE_DUMMY,
                              &desc, sizeof(desc), &iommu));
    ASSERT_OK(zx::bti::create(iommu, 0, 0xdeadbeef, &bti));

    zx::vmo vmos[4];
    ASSERT_OK(zx::vmo::create_contiguous(bti, 4 * ZX_PAGE_SIZE, 0, vmos));

    // Tag each page.
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[0], i + 1, i * ZX_PAGE_SIZE));
    }

    // Create two clones of the original VMO and one clone of one of those clones.
    ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 4 * ZX_PAGE_SIZE, vmos + 1));
    ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 4 * ZX_PAGE_SIZE, vmos + 2));
    ASSERT_OK(vmos[1].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 4 * ZX_PAGE_SIZE, vmos + 3));

    // Write to one page in each different VMO.
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[i], 5, i * ZX_PAGE_SIZE));
    }

    // Verify that the data is correct in each VMO.
    for (unsigned i = 0; i < 4; i++) {
        for (unsigned j = 0; j < 4; j++) {
            ASSERT_NO_FATAL_FAILURES(VmoCheck(vmos[i], i == j ? 5 : j + 1, j * ZX_PAGE_SIZE));
        }
    }

    ASSERT_NO_FATAL_FAILURES(CheckContigState<4>(bti, vmos[0]));
}

// Tests that closing the clone of a contiguous VMO doesn't cause problems with contiguity.
TEST_F(VmoClone2TestCase, ContiguousVmoCloseChild) {
    if (!RootResource()) {
        printf("Root resource not available, skipping\n");
        return;
    }

    zx::iommu iommu;
    zx::bti bti;
    zx_iommu_desc_dummy_t desc;
    ASSERT_OK(zx::iommu::create(RootResource(), ZX_IOMMU_TYPE_DUMMY,
                              &desc, sizeof(desc), &iommu));
    ASSERT_OK(zx::bti::create(iommu, 0, 0xdeadbeef, &bti));

    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create_contiguous(bti, 2 * ZX_PAGE_SIZE, 0, &vmo));

    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 1, 0));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 2, ZX_PAGE_SIZE));

    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 2 * ZX_PAGE_SIZE, &clone));

    // Write to one page in the contig VMO so that one page is forked and one page isn't forked.
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 3, 0));

    // Reset the original VMO and check that things got properly merged into the child.
    clone.reset();

    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, 3, 0));
    ASSERT_NO_FATAL_FAILURES(VmoCheck(vmo, 2, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(CheckContigState<2>(bti, vmo));
}

// Tests that pages are properly become 'non-contiguous' after closing a contiguous VMO
// with a child.
TEST_F(VmoClone2TestCase, ContiguousVmoCloseOriginal) {
    if (!RootResource()) {
        printf("Root resource not available, skipping\n");
        return;
    }

    const uint64_t original = KmemVmoMemUsage();

    zx::iommu iommu;
    zx::bti bti;
    zx_iommu_desc_dummy_t desc;
    ASSERT_OK(zx::iommu::create(RootResource(), ZX_IOMMU_TYPE_DUMMY,
                              &desc, sizeof(desc), &iommu));
    ASSERT_OK(zx::bti::create(iommu, 0, 0xdeadbeef, &bti));

    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create_contiguous(bti, 3 * ZX_PAGE_SIZE, 0, &vmo));

    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 1, 0));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 2, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmo, 3, 2 * ZX_PAGE_SIZE));

    // Create the clone so that there is a page before and after it.
    zx::vmo clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, ZX_PAGE_SIZE, ZX_PAGE_SIZE, &clone));

    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 2));

    vmo.reset();

    ASSERT_NO_FATAL_FAILURES(VmoCheck(clone, 2));
    ASSERT_EQ(VmoCommittedBytes(clone), ZX_PAGE_SIZE);
    ASSERT_EQ(original + ZX_PAGE_SIZE, KmemVmoMemUsage());
}

TEST_F(VmoCloneResizeTests, ContiguousVmoResizeChild) {
    ASSERT_NO_FATAL_FAILURES(ResizeTest(Contiguity::Contig, ResizeTarget::Child));
}

TEST_F(VmoCloneResizeTests, ContiguousVmoResizeOriginal) {
    ASSERT_NO_FATAL_FAILURES(ResizeTest(Contiguity::Contig, ResizeTarget::Parent));
}

// Tests partial clones of contiguous vmos.
TEST_F(VmoClone2TestCase, ContiguousVmoPartialClone) {
    if (!RootResource()) {
        printf("Root resource not available, skipping\n");
        return;
    }

    zx::iommu iommu;
    zx::bti bti;
    zx_iommu_desc_dummy_t desc;
    ASSERT_OK(zx::iommu::create(RootResource(), ZX_IOMMU_TYPE_DUMMY,
                              &desc, sizeof(desc), &iommu));
    ASSERT_OK(zx::bti::create(iommu, 0, 0xdeadbeef, &bti));

    zx::vmo vmos[4];
    ASSERT_OK(zx::vmo::create_contiguous(bti, 3 * ZX_PAGE_SIZE, 0, vmos));

    // Tag each page.
    for (unsigned i = 0; i < 3; i++) {
        ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[0], i + 1, i * ZX_PAGE_SIZE));
    }

    // Create two clones of the original VMO and one clone of one of those clones.
    ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 1));
    ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, vmos + 2));
    ASSERT_OK(vmos[0].create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, 4 * ZX_PAGE_SIZE, vmos + 3));

    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[0], 5, ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[3], 6, ZX_PAGE_SIZE));

    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[3], 6, 2 * ZX_PAGE_SIZE));
    ASSERT_NO_FATAL_FAILURES(VmoWrite(vmos[0], 5, 2 * ZX_PAGE_SIZE));

    // Verify that the data is correct in each VMO.
    for (unsigned i = 0; i < 4; i++) {
        ASSERT_NO_FATAL_FAILURES(VmoCheck(vmos[i], 1, 0 * ZX_PAGE_SIZE));
        if (i == 0 || i == 3) {
            uint32_t target_val = i == 0 ? 5 : 6;
            ASSERT_NO_FATAL_FAILURES(VmoCheck(vmos[i], target_val, 1 * ZX_PAGE_SIZE));
            ASSERT_NO_FATAL_FAILURES(VmoCheck(vmos[i], target_val, 2 * ZX_PAGE_SIZE));
        }
    }

    ASSERT_NO_FATAL_FAILURES(CheckContigState<3>(bti, vmos[0]));
}

// Tests that clones based on physical vmos can't be created.
TEST_F(VmoClone2TestCase, NoPhysical) {
    if (!RootResource()) {
        printf("Root resource not available, skipping\n");
        return;
    }

    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create_physical(RootResource(), 0, ZX_PAGE_SIZE, &vmo));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone),
              ZX_ERR_NOT_SUPPORTED);
}

// Tests that clones based on pager vmos can't be created.
TEST(VmoClone2TestCase, NoPager) {
    zx::pager pager;
    ASSERT_OK(zx::pager::create(0, &pager));

    zx::port port;
    ASSERT_OK(zx::port::create(0, &port));

    zx::vmo vmo;
    ASSERT_OK(pager.create_vmo(0, port, 0, ZX_PAGE_SIZE, &vmo));

    zx::vmo uni_clone;
    ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE, 0, ZX_PAGE_SIZE, &uni_clone));

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone),
                               ZX_ERR_NOT_SUPPORTED);
    ASSERT_EQ(uni_clone.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2,
                                     0, ZX_PAGE_SIZE, &clone), ZX_ERR_NOT_SUPPORTED);
}

// Tests that clones of uncached memory can't be created.
TEST(VmoClone2TestCase, Uncached) {
    zx::vmo vmo;
    ASSERT_OK(zx::vmo::create(ZX_PAGE_SIZE, 0, &vmo));

    ASSERT_OK(vmo.set_cache_policy(ZX_CACHE_POLICY_UNCACHED));

    Mapping vmo_mapping;
    ASSERT_OK(vmo_mapping.Init(vmo, ZX_PAGE_SIZE));

    static constexpr uint32_t kOriginalData = 0xdeadbeef;
    *vmo_mapping.ptr() = kOriginalData;

    zx::vmo clone;
    ASSERT_EQ(vmo.create_child(ZX_VMO_CHILD_COPY_ON_WRITE2, 0, ZX_PAGE_SIZE, &clone),
              ZX_ERR_BAD_STATE);

    ASSERT_EQ(*vmo_mapping.ptr(), kOriginalData);
}

} // namespace
