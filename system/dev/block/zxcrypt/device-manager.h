// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_DEV_BLOCK_ZXCRYPT_DEVICE_MANAGER_H_
#define ZIRCON_SYSTEM_DEV_BLOCK_ZXCRYPT_DEVICE_MANAGER_H_

#include <ddk/device.h>
#include <ddktl/device.h>
#include <fbl/macros.h>
#include <fbl/mutex.h>
#include <stddef.h>
#include <stdint.h>
#include <zircon/compiler.h>
#include <zircon/types.h>
#include <zxcrypt/volume.h>

namespace zxcrypt {

// |zxcrypt::DeviceManager| is a "wrapper" driver for zxcrypt volumes.  Each block device with valid
// zxcrypt metadata will result in a wrapper being created, but the wrapper cannot perform any block
// operations.  To perform block operations, |Unseal| must first be called with a valid key and
// slot, which will cause an unsealed |zxcrypt::Device| to be added to the device tree.
class DeviceManager;
using DeviceManagerType = ddk::Device<DeviceManager, ddk::Unbindable, ddk::Messageable>;
class DeviceManager final : public DeviceManagerType {
 public:
  explicit DeviceManager(zx_device_t* parent) : DeviceManagerType(parent), state_(kBinding) {}
  ~DeviceManager() = default;
  DISALLOW_COPY_ASSIGN_AND_MOVE(DeviceManager);

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  // Adds the device
  zx_status_t Bind();

  // ddk::Device methods; see ddktl/device.h
  void DdkUnbind() __TA_EXCLUDES(mtx_);
  void DdkRelease();

  // ddk::Messageable methods
  zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) __TA_EXCLUDES(mtx_);

  // Unseals the zxcrypt volume and adds it as a |zxcrypt::Device| to the device tree.
  zx_status_t Unseal(const uint8_t* ikm, size_t ikm_len, key_slot_t slot) __TA_EXCLUDES(mtx_);

  // Removes the unsealed |zxcrypt::Device|, if present.
  zx_status_t Seal() __TA_EXCLUDES(mtx_);

  // Calls |Unseal| with a fixed key.
  // TODO(security): ZX-3257.  This stopgap should be removed when the zxcrypt FIDL interface is
  // available.
  void AutoUnseal() __TA_EXCLUDES(mtx_);

 private:
  // Represents the state of this device.
  // TODO(security): ZX-3257. When |AutoUnseal| is removed, this can be reduced to a simple
  // boolean indicating un/sealed.
  enum State {
    kBinding,
    kSealed,
    kUnsealed,
    kUnbinding,
    kRemoved,
  };

  // Unseals the zxcrypt volume and adds it as a |zxcrypt::Device| to the device tree.
  // TODO(security): ZX-3257. When |AutoUnseal| is removed, this can be merged into |Unseal|.
  zx_status_t UnsealLocked(const uint8_t* ikm, size_t ikm_len, key_slot_t slot) __TA_REQUIRES(mtx_);

  // Used to ensure calls to |Unseal|, |Seal|,|Unbind|, an |AutoUnseal| are exclusive to each
  // other, and protects access to |state_|.
  // TODO(security): ZX-3257. Update comment when |AutoUnseal|, |state_| are removed.
  fbl::Mutex mtx_;

  // TODO(security): ZX-3257. See commnt on |State| above.
  State state_ __TA_GUARDED(mtx_);
};

}  // namespace zxcrypt

#endif  // ZIRCON_SYSTEM_DEV_BLOCK_ZXCRYPT_DEVICE_MANAGER_H_
