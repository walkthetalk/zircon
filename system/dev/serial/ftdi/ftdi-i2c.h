// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_SERIAL_FTDI_FTDI_I2C_H_
#define ZIRCON_SYSTEM_DEV_SERIAL_FTDI_FTDI_I2C_H_

#include <ddktl/device.h>
#include <ddktl/protocol/i2cimpl.h>
#include <fuchsia/hardware/ftdi/c/fidl.h>
#include <threads.h>

#include <vector>

#include "ftdi-mpsse.h"

namespace ftdi_mpsse {

class FtdiI2c;
using DeviceType = ddk::Device<FtdiI2c, ddk::Unbindable>;

// This class represents a single I2C bus created from 3 pins of an FTDI device.
// It implements the standard I2cImpl driver. It is created with metadata that will
// allow other I2C devices that exist on the bus to bind.
class FtdiI2c : public DeviceType, public ddk::I2cImplProtocol<FtdiI2c, ddk::base_protocol> {
 public:
  struct I2cLayout {
    uint32_t scl;
    uint32_t sda_out;
    uint32_t sda_in;
  };
  struct I2cDevice {
    uint32_t address;
    uint32_t vid;
    uint32_t pid;
    uint32_t did;
  };
  FtdiI2c(zx_device_t* parent, I2cLayout layout, std::vector<I2cDevice> i2c_devices)
      : DeviceType(parent),
        pin_layout_(layout),
        mpsse_(parent),
        i2c_devices_(std::move(i2c_devices)) {}

  static zx_status_t Create(zx_device_t* device, const fuchsia_hardware_ftdi_I2cBusLayout* layout,
                            const fuchsia_hardware_ftdi_I2cDevice* i2c_dev);
  zx_status_t Bind();
  void DdkUnbind();
  void DdkRelease() { delete this; }

  uint32_t I2cImplGetBusCount() { return 1; }

  zx_status_t I2cImplGetMaxTransferSize(uint32_t bus_id, size_t* out_size) {
    *out_size = kFtdiI2cMaxTransferSize;
    return ZX_OK;
  }

  // Sets the bitrate for the i2c bus in KHz units.
  zx_status_t I2cImplSetBitrate(uint32_t bus_id, uint32_t bitrate) { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t I2cImplTransact(uint32_t bus_id, const i2c_impl_op_t* op_list, size_t op_count);

  zx_status_t Ping(uint8_t bus_address);
  zx_status_t Write(uint8_t bus_address, std::vector<uint8_t> data);
  zx_status_t Enable();

 private:
  static constexpr int kFtdiI2cMaxTransferSize = 0x1000;
  static constexpr uint8_t kI2cWriteCommandByte1 = 0x11;
  static constexpr uint8_t kI2cWriteCommandByte2 = 0x00;
  static constexpr uint8_t kI2cWriteCommandByte3 = 0x00;
  static constexpr uint8_t kI2cReadAckCommandByte1 = 0x22;
  static constexpr uint8_t kI2cReadAckCommandByte2 = 0x00;
  // Every full write requires 49 additional bytes. These are for the start and end I2C
  // sequence commands.
  static constexpr uint8_t kI2cNumCommandBytesPerFullWrite = 49;
  // We need to write 12 bytes for every written byte. There are 3 prefix command bytes, a 6
  // byte command to reset GPIO pins, and a 2 byte suffix command for reading the ACK bit.
  static constexpr uint8_t kI2cNumCommandBytesPerWriteByte = 12;
  static constexpr uint8_t kI2cCommandFinishTransaction = 0x87;
  static constexpr uint8_t kFtdiCommandDriveZeroMode = 0x9E;

  zx_status_t WriteIdleToBuf(size_t index, std::vector<uint8_t>* buffer, size_t* bytes_written);
  zx_status_t WriteTransactionStartToBuf(size_t index, std::vector<uint8_t>* buffer,
                                         size_t* bytes_written);
  zx_status_t WriteTransactionEndToBuf(size_t index, std::vector<uint8_t>* buffer,
                                       size_t* bytes_written);

  thrd_t enable_thread_;

  I2cLayout pin_layout_;
  Mpsse mpsse_;
  std::vector<I2cDevice> i2c_devices_;
};

}  // namespace ftdi_mpsse

#endif  // ZIRCON_SYSTEM_DEV_SERIAL_FTDI_FTDI_I2C_H_
