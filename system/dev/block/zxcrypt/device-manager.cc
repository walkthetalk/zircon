// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device-manager.h"

#include <crypto/secret.h>
#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <ddktl/device.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_call.h>
#include <fbl/auto_lock.h>
#include <fbl/macros.h>
#include <fbl/unique_ptr.h>
#include <fuchsia/hardware/zxcrypt/c/fidl.h>
#include <threads.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zxcrypt/ddk-volume.h>
#include <zxcrypt/volume.h>

#include "device-info.h"
#include "device.h"

namespace zxcrypt {

zx_status_t DeviceManager::Create(void* ctx, zx_device_t* parent) {
  zx_status_t rc;
  fbl::AllocChecker ac;

  auto manager = fbl::make_unique_checked<DeviceManager>(&ac, parent);
  if (!ac.check()) {
    zxlogf(ERROR, "failed to allocate %zu bytes\n", sizeof(DeviceManager));
    return ZX_ERR_NO_MEMORY;
  }

  if ((rc = manager->Bind()) != ZX_OK) {
    zxlogf(ERROR, "failed to bind: %s\n", zx_status_get_string(rc));
    return rc;
  }

  // devmgr is now in charge of the memory for |manager|.
  __UNUSED auto* owned_by_devmgr_now = manager.release();

  return ZX_OK;
}

zx_status_t DeviceManager::Bind() {
  zx_status_t rc;
  fbl::AutoLock lock(&mtx_);

  if ((rc = DdkAdd("zxcrypt")) != ZX_OK) {
    zxlogf(ERROR, "failed to add device: %s\n", zx_status_get_string(rc));
    state_ = kRemoved;
    return rc;
  }

  state_ = kSealed;
  return ZX_OK;
}

void DeviceManager::DdkUnbind() {
  fbl::AutoLock lock(&mtx_);
  if (state_ == kBinding) {
    state_ = kUnbinding;
  } else if (state_ == kSealed || state_ == kUnsealed) {
    state_ = kRemoved;
    DdkRemove();
  }
}

void DeviceManager::DdkRelease() { delete this; }

zx_status_t Unseal(void* ctx, const uint8_t* key_data, const size_t key_count, uint8_t slot,
                   fidl_txn_t* txn) {
  DeviceManager* device = reinterpret_cast<DeviceManager*>(ctx);
  key_slot_t key_slot = static_cast<key_slot_t>(slot);  // widens
  zx_status_t status = device->Unseal(key_data, key_count, key_slot);
  return fuchsia_hardware_zxcrypt_DeviceManagerUnseal_reply(txn, status);
}

zx_status_t Seal(void* ctx, fidl_txn_t* txn) {
  DeviceManager* device = reinterpret_cast<DeviceManager*>(ctx);
  zx_status_t status = device->Seal();
  return fuchsia_hardware_zxcrypt_DeviceManagerSeal_reply(txn, status);
}

static fuchsia_hardware_zxcrypt_DeviceManager_ops_t fidl_ops = {.Unseal = Unseal, .Seal = Seal};

zx_status_t DeviceManager::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
  return fuchsia_hardware_zxcrypt_DeviceManager_dispatch(this, txn, msg, &fidl_ops);
}

zx_status_t DeviceManager::Unseal(const uint8_t* ikm, size_t ikm_len, key_slot_t slot) {
  fbl::AutoLock lock(&mtx_);
  if (state_ != kSealed) {
    zxlogf(ERROR, "can't unseal zxcrypt, state=%d\n", state_);
    return ZX_ERR_BAD_STATE;
  }
  return UnsealLocked(ikm, ikm_len, slot);
}

zx_status_t DeviceManager::Seal() {
  zx_status_t rc;
  fbl::AutoLock lock(&mtx_);

  if (state_ != kUnsealed) {
    zxlogf(ERROR, "can't seal zxcrypt, state=%d\n", state_);
    return ZX_ERR_BAD_STATE;
  }
  if ((rc = device_rebind(zxdev())) != ZX_OK) {
    zxlogf(ERROR, "failed to rebind zxcrypt: %s\n", zx_status_get_string(rc));
    return rc;
  }

  state_ = kSealed;
  return ZX_OK;
}

zx_status_t DeviceManager::UnsealLocked(const uint8_t* ikm, size_t ikm_len, key_slot_t slot) {
  zx_status_t rc;

  // Unseal the zxcrypt volume.
  crypto::Secret key;
  uint8_t* buf;
  if ((rc = key.Allocate(ikm_len, &buf)) != ZX_OK) {
    zxlogf(ERROR, "failed to allocate %zu-byte key: %s\n", ikm_len, zx_status_get_string(rc));
    return rc;
  }
  memcpy(buf, ikm, key.len());
  fbl::unique_ptr<DdkVolume> volume;
  if ((rc = DdkVolume::Unlock(parent(), key, slot, &volume)) != ZX_OK) {
    zxlogf(ERROR, "failed to unseal volume: %s\n", zx_status_get_string(rc));
    return rc;
  }

  // Get the parent device's configuration details.
  DeviceInfo info(parent(), *volume);
  if (!info.IsValid()) {
    zxlogf(ERROR, "failed to get valid device info\n");
    return ZX_ERR_BAD_STATE;
  }
  // Reserve space for shadow I/O transactions
  if ((rc = info.Reserve(Volume::kBufferSize)) != ZX_OK) {
    zxlogf(ERROR, "failed to reserve buffer for I/O: %s\n", zx_status_get_string(rc));
    return rc;
  }

  // Create the unsealed device
  fbl::AllocChecker ac;
  auto device = fbl::make_unique_checked<zxcrypt::Device>(&ac, zxdev(), std::move(info));
  if (!ac.check()) {
    zxlogf(ERROR, "failed to allocate %zu bytes\n", sizeof(zxcrypt::Device));
    return ZX_ERR_NO_MEMORY;
  }
  if ((rc = device->Init(*volume)) != ZX_OK) {
    zxlogf(ERROR, "failed to initialize device: %s\n", zx_status_get_string(rc));
    return rc;
  }
  if ((rc = device->DdkAdd("unsealed")) != ZX_OK) {
    zxlogf(ERROR, "failed to add device: %s\n", zx_status_get_string(rc));
    return rc;
  }

  // devmgr is now in charge of the memory for |device|
  __UNUSED auto owned_by_devmgr_now = device.release();
  state_ = kUnsealed;
  return ZX_OK;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = DeviceManager::Create;
  return ops;
}();

}  // namespace zxcrypt

// clang-format off
ZIRCON_DRIVER_BEGIN(zxcrypt, zxcrypt::driver_ops, "zircon", "0.1", 2)
    BI_ABORT_IF_AUTOBIND,
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_BLOCK),
ZIRCON_DRIVER_END(zxcrypt)
    // clang-format on
