// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "slice-extent.h"

#include <fbl/unique_ptr.h>
#include <zircon/assert.h>

namespace fvm {

fbl::unique_ptr<SliceExtent> SliceExtent::Split(size_t vslice) {
    ZX_DEBUG_ASSERT(start() <= vslice);
    ZX_DEBUG_ASSERT(vslice < end());

    fbl::unique_ptr<SliceExtent> new_extent(new SliceExtent(vslice + 1));
    new_extent->pslices_.reserve(end() - vslice);

    for (size_t vs = vslice + 1; vs < end(); vs++) {
        new_extent->push_back(at(vs));
    }
    while (!empty() && vslice + 1 != end()) {
        pop_back();
    }
    return new_extent;
}

void SliceExtent::Merge(const SliceExtent& other) {
    ZX_DEBUG_ASSERT(end() == other.start());
    pslices_.reserve(other.size());

    for (size_t vs = other.start(); vs < other.end(); vs++) {
        push_back(other.at(vs));
    }
}

} // namespace fvm
