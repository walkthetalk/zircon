// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <lib/zx/vmo.h>
#include <limits.h>
#include <zxtest/zxtest.h>

namespace {

TEST(VmoSliceTestCase, WriteThrough) {
  // Create parent VMO with 4 pages.
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(PAGE_SIZE * 4, 0, &vmo));

  // Write to our first two pages.
  uint32_t val = 42;
  EXPECT_OK(vmo.write(&val, 0, sizeof(val)));
  EXPECT_OK(vmo.write(&val, PAGE_SIZE, sizeof(val)));

  // Create a child that can see the middle two pages.
  zx::vmo slice_vmo;
  ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_SLICE, PAGE_SIZE, PAGE_SIZE * 2, &slice_vmo));

  // The first page in the slice should have the contents we wrote to the parent earlier.
  EXPECT_OK(slice_vmo.read(&val, 0, sizeof(val)));
  EXPECT_EQ(val, 42);

  // Write to the two pages in the slice. The second page is the third page in the parent and
  // was never written to or allocated previously. After this the parent should contain
  // [42, 84, 84, unallocated]
  val = 84;
  EXPECT_OK(slice_vmo.write(&val, 0, sizeof(val)));
  EXPECT_OK(slice_vmo.write(&val, PAGE_SIZE, sizeof(val)));

  EXPECT_OK(vmo.read(&val, 0, sizeof(val)));
  EXPECT_EQ(val, 42);
  EXPECT_OK(vmo.read(&val, PAGE_SIZE, sizeof(val)));
  EXPECT_EQ(val, 84);
  EXPECT_OK(vmo.read(&val, PAGE_SIZE * 2, sizeof(val)));
  EXPECT_EQ(val, 84);
  EXPECT_OK(vmo.read(&val, PAGE_SIZE * 3, sizeof(val)));
  EXPECT_EQ(val, 0);
}

TEST(VmoSliceTestCase, DecommitParent) {
  // Create parent VMO and put some data in it.
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(PAGE_SIZE, 0, &vmo));

  uint8_t val = 42;
  EXPECT_OK(vmo.write(&val, 0, sizeof(val)));

  // Create the child and check we can see what we wrote in the parent.
  zx::vmo slice_vmo;
  ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_SLICE, 0, PAGE_SIZE, &slice_vmo));

  EXPECT_OK(slice_vmo.read(&val, 0, sizeof(val)));
  EXPECT_EQ(val, 42);

  // Decommit from the parent should cause the slice to see fresh zero pages.
  EXPECT_OK(vmo.op_range(ZX_VMO_OP_DECOMMIT, 0, PAGE_SIZE, nullptr, 0));

  EXPECT_OK(slice_vmo.read(&val, 0, sizeof(val)));
  EXPECT_EQ(val, 0);
}

TEST(VmoSliceTestCase, Nested) {
  // Create parent.
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(PAGE_SIZE * 2, 0, &vmo));

  // Put something in the first page.
  uint32_t val = 42;
  EXPECT_OK(vmo.write(&val, 0, sizeof(val)));

  // Create a child that can see both pages.
  zx::vmo slice_vmo;
  ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_SLICE, 0, PAGE_SIZE * 2, &slice_vmo));

  // Create a child of the child.
  zx::vmo slice_slice_vmo;
  ASSERT_OK(slice_vmo.create_child(ZX_VMO_CHILD_SLICE, 0, PAGE_SIZE * 2, &slice_slice_vmo));

  // Check the child of the child sees parent data.
  EXPECT_OK(slice_slice_vmo.read(&val, 0, sizeof(val)));
  EXPECT_EQ(val, 42);

  // Write to child of child and check parent updates.
  val = 84;
  EXPECT_OK(slice_slice_vmo.write(&val, 0, sizeof(val)));
  EXPECT_OK(slice_slice_vmo.write(&val, PAGE_SIZE, sizeof(val)));

  EXPECT_OK(vmo.read(&val, 0, sizeof(val)));
  EXPECT_EQ(val, 84);
  EXPECT_OK(vmo.read(&val, PAGE_SIZE, sizeof(val)));
  EXPECT_EQ(val, 84);
}

TEST(VmoSliceTestCase, NonSlice) {
  // Create parent.
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(PAGE_SIZE * 2, ZX_VMO_RESIZABLE, &vmo));

  // Creating children that are not strict slices should fail.
  zx::vmo slice_vmo;
  EXPECT_EQ(ZX_ERR_INVALID_ARGS,
            vmo.create_child(ZX_VMO_CHILD_SLICE, 0, PAGE_SIZE * 3, &slice_vmo));
  EXPECT_EQ(ZX_ERR_INVALID_ARGS,
            vmo.create_child(ZX_VMO_CHILD_SLICE, PAGE_SIZE, PAGE_SIZE * 2, &slice_vmo));
  EXPECT_EQ(ZX_ERR_INVALID_ARGS,
            vmo.create_child(ZX_VMO_CHILD_SLICE, PAGE_SIZE * 2, PAGE_SIZE, &slice_vmo));
  EXPECT_EQ(ZX_ERR_OUT_OF_RANGE,
            vmo.create_child(ZX_VMO_CHILD_SLICE, 0, UINT64_MAX, &slice_vmo));
  const uint64_t nearly_int_max = UINT64_MAX - PAGE_SIZE + 1;
  EXPECT_EQ(ZX_ERR_OUT_OF_RANGE,
            vmo.create_child(ZX_VMO_CHILD_SLICE, 0, nearly_int_max, &slice_vmo));
  EXPECT_EQ(ZX_ERR_INVALID_ARGS,
            vmo.create_child(ZX_VMO_CHILD_SLICE, nearly_int_max, PAGE_SIZE, &slice_vmo));
  EXPECT_EQ(ZX_ERR_OUT_OF_RANGE,
            vmo.create_child(ZX_VMO_CHILD_SLICE, nearly_int_max, nearly_int_max, &slice_vmo));
  EXPECT_EQ(ZX_ERR_OUT_OF_RANGE,
            vmo.create_child(ZX_VMO_CHILD_SLICE, nearly_int_max, UINT64_MAX, &slice_vmo));

}

TEST(VmoSliceTestCase, NonResizable) {
  // Create a resizable parent.
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(PAGE_SIZE, ZX_VMO_RESIZABLE, &vmo));

  // Any slice creation should fail.
  zx::vmo slice_vmo;
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, vmo.create_child(ZX_VMO_CHILD_SLICE, 0, PAGE_SIZE, &slice_vmo));
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, vmo.create_child(ZX_VMO_CHILD_SLICE | ZX_VMO_CHILD_RESIZABLE, 0,
                                                  PAGE_SIZE, &slice_vmo));

  // Switch to a correctly non-resizable parent.
  vmo.reset();
  ASSERT_OK(zx::vmo::create(PAGE_SIZE, 0, &vmo));

  // A resizable slice should fail.
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, vmo.create_child(ZX_VMO_CHILD_SLICE | ZX_VMO_CHILD_RESIZABLE, 0,
                                                  PAGE_SIZE, &slice_vmo));
}

TEST(VmoSliceTestCase, CommitChild) {
  // Create parent VMO.
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(PAGE_SIZE, 0, &vmo));

  // Create a child and commit it.
  zx::vmo slice_vmo;
  ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_SLICE, 0, PAGE_SIZE, &slice_vmo));
  EXPECT_OK(slice_vmo.op_range(ZX_VMO_OP_COMMIT, 0, PAGE_SIZE, nullptr, 0));

  // Now write to the child and verify the parent reads the same.
  uint8_t val = 42;
  EXPECT_OK(slice_vmo.write(&val, 0, sizeof(val)));
  EXPECT_OK(vmo.read(&val, 0, sizeof(val)));
  EXPECT_EQ(val, 42);
}

TEST(VmoSliceTestCase, DecommitChild) {
  // Create parent VMO.
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(PAGE_SIZE, 0, &vmo));

  // Write to the parent to commit some pages.
  uint8_t val = 42;
  EXPECT_OK(vmo.write(&val, 0, sizeof(val)));

  // Create a child and decommit.
  zx::vmo slice_vmo;
  ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_SLICE, 0, PAGE_SIZE, &slice_vmo));
  EXPECT_OK(slice_vmo.op_range(ZX_VMO_OP_DECOMMIT, 0, PAGE_SIZE, nullptr, 0));

  // Reading from the parent should result in fresh zeros.
  // Now write to the child and verify the parent reads the same.
  EXPECT_OK(vmo.read(&val, 0, sizeof(val)));
  EXPECT_EQ(val, 0);
}

TEST(VmoSliceTestCase, ZeroSized) {
  // Create parent VMO.
  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(PAGE_SIZE, 0, &vmo));

  // Create some zero sized children.
  zx::vmo slice_vmo1;
  ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_SLICE, 0, 0, &slice_vmo1));
  zx::vmo slice_vmo2;
  ASSERT_OK(vmo.create_child(ZX_VMO_CHILD_SLICE, PAGE_SIZE, 0, &slice_vmo2));

  // Reading and writing should fail.
  uint8_t val = 42;
  EXPECT_EQ(slice_vmo1.read(&val, 0, 1), ZX_ERR_OUT_OF_RANGE);
  EXPECT_EQ(slice_vmo2.read(&val, 0, 1), ZX_ERR_OUT_OF_RANGE);
  EXPECT_EQ(slice_vmo1.write(&val, 0, 1), ZX_ERR_OUT_OF_RANGE);
  EXPECT_EQ(slice_vmo2.write(&val, 0, 1), ZX_ERR_OUT_OF_RANGE);
}

}  // namespace
