// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <fbl/ref_ptr.h>
#include <fbl/unique_ptr.h>
#include <fbl/macros.h>
#include <lib/zx/bti.h>
#include <lib/zx/pmt.h>
#include <lib/zx/vmo.h>
#include <zircon/types.h>

#include <utility>

namespace fzl {

class PinnedVmo {
  public:
    struct Region {
        zx_paddr_t phys_addr;
        uint64_t   size;
    };

    PinnedVmo() = default;
    ~PinnedVmo() { Unpin(); }
    DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(PinnedVmo);

    // Move support
    PinnedVmo(PinnedVmo&& other) {
        *this = std::move(other);
    }

    PinnedVmo& operator=(PinnedVmo&& other) {
        pmt_ = std::move(other.pmt_);
        regions_ = std::move(other.regions_);
        region_count_ = other.region_count_;
        other.region_count_ = 0;
        return *this;
    }

    zx_status_t Pin(const zx::vmo& vmo, const zx::bti& bti, uint32_t options);
    void Unpin();

    uint32_t region_count() const { return region_count_; }
    const Region& region(uint32_t ndx) const {
        ZX_DEBUG_ASSERT(ndx < region_count_);
        ZX_DEBUG_ASSERT(regions_ != nullptr);
        return regions_[ndx];
    }

  private:
    void UnpinInternal();

    zx::pmt pmt_;
    fbl::unique_ptr<Region[]> regions_;
    uint32_t region_count_ = 0;
};

}  // namespace fzl
