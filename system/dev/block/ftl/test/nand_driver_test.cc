// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "nand_driver.h"

#include <memory>

#include <fbl/array.h>
#include <ddk/driver.h>
#include <ddktl/protocol/nand.h>
#include <ddktl/protocol/badblock.h>
#include <zxtest/zxtest.h>

namespace {

constexpr uint32_t kRealPageSize = 1024;
constexpr uint32_t kRealOobSize = 8;
constexpr uint32_t kRealBlockSize = 4;
constexpr uint32_t kPageSize = kRealPageSize * 2;
constexpr uint32_t kOobSize = kRealOobSize * 2;
constexpr uint32_t kBlockSize = kRealBlockSize / 2;
constexpr uint32_t kNumBlocks = 3;
constexpr uint32_t kEccBits = 12;

// Fake for the nand protocol.
class FakeNand : public ddk::NandProtocol<FakeNand> {
  public:
    FakeNand() : proto_({&nand_protocol_ops_, this}) {
        info_.page_size = kRealPageSize;
        info_.oob_size = kRealOobSize;
        info_.pages_per_block = kRealBlockSize;
        info_.num_blocks = kNumBlocks;
        info_.ecc_bits = kEccBits;
    }

    nand_protocol_t* proto() { return &proto_; }
    nand_operation_t& operation() { return operation_; }

    void set_result(zx_status_t result) { result_ = result; }
    void set_ecc_bits(uint32_t ecc_bits) { ecc_bits_ = ecc_bits; }

    // Nand protocol:
    void NandQuery(fuchsia_hardware_nand_Info* out_info, size_t* out_nand_op_size) {
        *out_info = info_;
        *out_nand_op_size = sizeof(operation_);
    }

    void NandQueue(nand_operation_t* operation, nand_queue_callback callback, void* cookie) {
        operation_ = *operation;
        if (operation->rw.command == NAND_OP_READ) {
            uint8_t data = 'd';
            uint64_t vmo_addr = operation->rw.offset_data_vmo * kRealPageSize;
            zx_vmo_write(operation->rw.data_vmo, &data, vmo_addr, sizeof(data));

            data = 'o';
            vmo_addr = operation->rw.offset_oob_vmo * kRealPageSize;
            zx_vmo_write(operation->rw.oob_vmo, &data, vmo_addr, sizeof(data));
            operation->rw.corrected_bit_flips = ecc_bits_;
        } else if (operation->rw.command == NAND_OP_WRITE) {
            uint8_t data;
            uint64_t vmo_addr = operation->rw.offset_data_vmo * kRealPageSize;
            zx_vmo_read(operation->rw.data_vmo, &data, vmo_addr, sizeof(data));
            if (data != 'd' && result_ == ZX_OK) {
                result_ = ZX_ERR_IO;
            }

            vmo_addr = operation->rw.offset_oob_vmo * kRealPageSize;
            zx_vmo_read(operation->rw.oob_vmo, &data, vmo_addr, sizeof(data));
            if (data != 'o' && result_ == ZX_OK) {
                result_ = ZX_ERR_IO;
            }
        }
        callback(cookie, result_, operation);
    }

    zx_status_t NandGetFactoryBadBlockList(uint32_t* out_bad_blocks_list, size_t bad_blocks_count,
                                           size_t* out_bad_blocks_actual) {
      return ZX_ERR_BAD_STATE;
    }

  private:
    nand_protocol_t proto_;
    fuchsia_hardware_nand_Info info_ = {};
    nand_operation_t operation_ = {};
    zx_status_t result_ = ZX_OK;
    uint32_t ecc_bits_ = 0;
};

// Fake for the bad block protocol.
class FakeBadBlock : public ddk::BadBlockProtocol<FakeBadBlock> {
  public:
    FakeBadBlock() : proto_({&bad_block_protocol_ops_, this}) {}

    bad_block_protocol_t* proto() { return &proto_; }
    void set_result(zx_status_t result) { result_ = result; }

    // Bad block protocol:
    zx_status_t BadBlockGetBadBlockList(uint32_t* out_bad_blocks_list, size_t bad_blocks_count,
                                        size_t* out_bad_blocks_actual) {
        *out_bad_blocks_actual = 0;
        if (!bad_blocks_count) {
            *out_bad_blocks_actual = 1;
        } else if (bad_blocks_count == 1) {
            ZX_ASSERT(out_bad_blocks_list);
            *out_bad_blocks_list = 1;  // Second block is bad.
            *out_bad_blocks_actual = 1;
        }
        return result_;
    }

    zx_status_t BadBlockMarkBlockBad(uint32_t block) {
        return ZX_ERR_BAD_STATE;
    }

  private:
    bad_block_protocol_t proto_;
    zx_status_t result_ = ZX_OK;
};

class NandDriverTest : public zxtest::Test {
  public:
    nand_protocol_t* nand_proto() { return nand_proto_.proto(); }
    bad_block_protocol_t* bad_block_proto() { return bad_block_proto_.proto(); }
    nand_operation_t& nand_operation() { return nand_proto_.operation(); }
    FakeNand* nand() { return &nand_proto_; }
    FakeBadBlock* bad_block() { return &bad_block_proto_; }

  private:
    FakeNand nand_proto_;
    FakeBadBlock bad_block_proto_;
};

TEST_F(NandDriverTest, TrivialLifetime) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
}

TEST_F(NandDriverTest, Init) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());
}

TEST_F(NandDriverTest, InitFailure) {
    bad_block()->set_result(ZX_ERR_BAD_STATE);
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_NE(nullptr, driver->Init());
}

TEST_F(NandDriverTest, Read) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    fbl::Array<uint8_t> data(new uint8_t[kPageSize * 2], kPageSize * 2);
    fbl::Array<uint8_t> oob(new uint8_t[kOobSize * 2], kOobSize * 2);

    ASSERT_EQ(ftl::kNdmOk, driver->NandRead(5, 2, data.get(), oob.get()));

    nand_operation_t& operation = nand_operation();
    EXPECT_EQ(NAND_OP_READ, operation.command);
    EXPECT_EQ(2 * 2, operation.rw.length);
    EXPECT_EQ(5 * 2, operation.rw.offset_nand);
    EXPECT_EQ(0, operation.rw.offset_data_vmo);
    EXPECT_EQ(2 * 2, operation.rw.offset_oob_vmo);
    EXPECT_EQ('d', data[0]);
    EXPECT_EQ('o', oob[0]);
}

TEST_F(NandDriverTest, ReadFailure) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    fbl::Array<uint8_t> data(new uint8_t[kPageSize * 2], kPageSize * 2);
    fbl::Array<uint8_t> oob(new uint8_t[kOobSize * 2], kOobSize * 2);

    nand()->set_result(ZX_ERR_BAD_STATE);
    ASSERT_EQ(ftl::kNdmFatalError, driver->NandRead(5, 2, data.get(), oob.get()));
}

TEST_F(NandDriverTest, ReadEccUnsafe) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    fbl::Array<uint8_t> data(new uint8_t[kPageSize * 2], kPageSize * 2);
    fbl::Array<uint8_t> oob(new uint8_t[kOobSize * 2], kOobSize * 2);

    nand()->set_ecc_bits(kEccBits / 2 + 1);
    ASSERT_EQ(ftl::kNdmUnsafeEcc, driver->NandRead(5, 2, data.get(), oob.get()));
}

TEST_F(NandDriverTest, ReadEccFailure) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    fbl::Array<uint8_t> data(new uint8_t[kPageSize * 2], kPageSize * 2);
    fbl::Array<uint8_t> oob(new uint8_t[kOobSize * 2], kOobSize * 2);

    nand()->set_ecc_bits(kEccBits + 1);
    ASSERT_EQ(ftl::kNdmUncorrectableEcc, driver->NandRead(5, 2, data.get(), oob.get()));
}

TEST_F(NandDriverTest, Write) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    fbl::Array<uint8_t> data(new uint8_t[kPageSize * 2], kPageSize * 2);
    fbl::Array<uint8_t> oob(new uint8_t[kOobSize * 2], kOobSize * 2);
    memset(data.get(), 'd', data.size());
    memset(oob.get(), 'o', oob.size());

    ASSERT_EQ(ftl::kNdmOk, driver->NandWrite(5, 2, data.get(), oob.get()));

    nand_operation_t& operation = nand_operation();
    EXPECT_EQ(NAND_OP_WRITE, operation.command);
    EXPECT_EQ(2 * 2, operation.rw.length);
    EXPECT_EQ(5 * 2, operation.rw.offset_nand);
    EXPECT_EQ(0, operation.rw.offset_data_vmo);
    EXPECT_EQ(2 * 2, operation.rw.offset_oob_vmo);
}

TEST_F(NandDriverTest, WriteFailure) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    fbl::Array<uint8_t> data(new uint8_t[kPageSize * 2], kPageSize * 2);
    fbl::Array<uint8_t> oob(new uint8_t[kOobSize * 2], kOobSize * 2);
    memset(data.get(), 'd', data.size());
    memset(oob.get(), 'e', oob.size());  // Unexpected value.
    nand()->set_result(ZX_ERR_BAD_STATE);

    ASSERT_EQ(ftl::kNdmFatalError, driver->NandWrite(5, 2, data.get(), oob.get()));
}

TEST_F(NandDriverTest, WriteFailureBadBlock) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    fbl::Array<uint8_t> data(new uint8_t[kPageSize * 2], kPageSize * 2);
    fbl::Array<uint8_t> oob(new uint8_t[kOobSize * 2], kOobSize * 2);
    memset(data.get(), 'd', data.size());
    memset(oob.get(), 'e', oob.size());  // Unexpected value.

    ASSERT_EQ(ftl::kNdmError, driver->NandWrite(5, 2, data.get(), oob.get()));
}

TEST_F(NandDriverTest, Erase) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    ASSERT_EQ(ftl::kNdmOk, driver->NandErase(5 * kBlockSize));

    nand_operation_t& operation = nand_operation();
    EXPECT_EQ(NAND_OP_ERASE, operation.command);
    EXPECT_EQ(1, operation.erase.num_blocks);
    EXPECT_EQ(5, operation.erase.first_block);
}

TEST_F(NandDriverTest, EraseFailure) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    nand()->set_result(ZX_ERR_BAD_STATE);
    ASSERT_EQ(ftl::kNdmFatalError, driver->NandErase(5 * kBlockSize));
}

TEST_F(NandDriverTest, EraseFailureBadBlock) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    nand()->set_result(ZX_ERR_IO);
    ASSERT_EQ(ftl::kNdmError, driver->NandErase(5 * kBlockSize));
}

TEST_F(NandDriverTest, IsBadBlock) {
    auto driver = ftl::NandDriver::Create(nand_proto(), bad_block_proto());
    ASSERT_EQ(nullptr, driver->Init());

    ASSERT_FALSE(driver->IsBadBlock(0));
    ASSERT_TRUE(driver->IsBadBlock(1 * kBlockSize));
    ASSERT_FALSE(driver->IsBadBlock(2 * kBlockSize));
}

}  // namespace
