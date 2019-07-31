// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_CORE_DEVMGR_DEVHOST_ZX_DEVICE_H_
#define ZIRCON_SYSTEM_CORE_DEVMGR_DEVHOST_ZX_DEVICE_H_

#include <ddk/device.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/intrusive_wavl_tree.h>
#include <fbl/mutex.h>
#include <fbl/recycler.h>
#include <fbl/ref_counted_upgradeable.h>
#include <fbl/ref_ptr.h>
#include <fbl/vector.h>
#include <fs/handler.h>
#include <lib/zx/channel.h>
#include <lib/zx/eventpair.h>
#include <zircon/compiler.h>
#include <zircon/thread_annotations.h>

#include <atomic>

namespace devmgr {

class CompositeDevice;
class DeviceControllerConnection;
struct ProxyIostate;

}  // namespace devmgr

#define DEV_MAGIC 'MDEV'

// This needs to be a struct, not a class, to match the public definition
struct zx_device : fbl::RefCountedUpgradeable<zx_device>, fbl::Recyclable<zx_device> {
  ~zx_device() = default;

  zx_device(const zx_device&) = delete;
  zx_device& operator=(const zx_device&) = delete;

  static zx_status_t Create(fbl::RefPtr<zx_device>* out_dev);

  zx_status_t OpenOp(zx_device_t** dev_out, uint32_t flags) {
    return Dispatch(ops->open, ZX_OK, dev_out, flags);
  }

  zx_status_t CloseOp(uint32_t flags) { return Dispatch(ops->close, ZX_OK, flags); }

  void UnbindOp() { Dispatch(ops->unbind); }

  void ReleaseOp() { Dispatch(ops->release); }

  zx_status_t SuspendOp(uint32_t flags) {
    return Dispatch(ops->suspend, ZX_ERR_NOT_SUPPORTED, flags);
  }

  zx_status_t ResumeOp(uint32_t flags) {
    return Dispatch(ops->resume, ZX_ERR_NOT_SUPPORTED, flags);
  }

  zx_status_t ReadOp(void* buf, size_t count, zx_off_t off, size_t* actual) {
    return Dispatch(ops->read, ZX_ERR_NOT_SUPPORTED, buf, count, off, actual);
  }

  zx_status_t WriteOp(const void* buf, size_t count, zx_off_t off, size_t* actual) {
    return Dispatch(ops->write, ZX_ERR_NOT_SUPPORTED, buf, count, off, actual);
  }

  zx_off_t GetSizeOp() { return Dispatch(ops->get_size, 0lu); }

  zx_status_t IoctlOp(uint32_t op, const void* in_buf, size_t in_len, void* out_buf, size_t out_len,
                      size_t* out_actual) {
    return Dispatch(ops->ioctl, ZX_ERR_NOT_SUPPORTED, op, in_buf, in_len, out_buf, out_len,
                    out_actual);
  }

  zx_status_t MessageOp(fidl_msg_t* msg, fidl_txn_t* txn) {
    return Dispatch(ops->message, ZX_ERR_NOT_SUPPORTED, msg, txn);
  }

  void PushBindConn(const fs::FidlConnection& conn);
  bool PopBindConn(fs::FidlConnection* conn);

  void PushTestCompatibilityConn(const fs::FidlConnection& conn);
  bool PopTestCompatibilityConn(fs::FidlConnection* conn);
  // Check if this devhost has a device with the given ID, and if so returns a
  // reference to it.
  static fbl::RefPtr<zx_device> GetDeviceFromLocalId(uint64_t local_id);

  uint64_t local_id() const { return local_id_; }
  void set_local_id(uint64_t id);

  uintptr_t magic = DEV_MAGIC;

  const zx_protocol_device_t* ops = nullptr;

  // reserved for driver use; will not be touched by devmgr
  void* ctx = nullptr;

  uint32_t flags = 0;

  zx::eventpair event;
  zx::eventpair local_event;
  // The RPC channel is owned by |conn|
  zx::unowned_channel rpc;

  // most devices implement a single
  // protocol beyond the base device protocol
  uint32_t protocol_id = 0;
  void* protocol_ops = nullptr;

  // driver that has published this device
  zx_driver_t* driver = nullptr;

  // parent in the device tree
  fbl::RefPtr<zx_device_t> parent;

  // for the parent's device_list
  fbl::DoublyLinkedListNodeState<zx_device*> node;
  struct Node {
    static fbl::DoublyLinkedListNodeState<zx_device*>& node_state(zx_device& obj) {
      return obj.node;
    }
  };

  // list of this device's children in the device tree
  fbl::DoublyLinkedList<zx_device*, Node> children;

  // list node for the defer_device_list
  fbl::DoublyLinkedListNodeState<zx_device*> defer;
  struct DeferNode {
    static fbl::DoublyLinkedListNodeState<zx_device*>& node_state(zx_device& obj) {
      return obj.defer;
    }
  };

  // This is an atomic so that the connection's async loop can inspect this
  // value to determine if an expected shutdown is happening.  See comments in
  // devhost_remove().
  std::atomic<devmgr::DeviceControllerConnection*> conn = nullptr;

  fbl::Mutex proxy_ios_lock;
  devmgr::ProxyIostate* proxy_ios TA_GUARDED(proxy_ios_lock) = nullptr;

  char name[ZX_DEVICE_NAME_MAX + 1] = {};

  // Trait structures for the local ID map
  struct LocalIdNode {
    static fbl::WAVLTreeNodeState<fbl::RefPtr<zx_device>>& node_state(zx_device& obj) {
      return obj.local_id_node_;
    }
  };
  struct LocalIdKeyTraits {
    static uint64_t GetKey(const zx_device& obj) { return obj.local_id_; }
    static bool LessThan(const uint64_t& key1, const uint64_t& key2) { return key1 < key2; }
    static bool EqualTo(const uint64_t& key1, const uint64_t& key2) { return key1 == key2; }
  };

  void set_composite(fbl::RefPtr<devmgr::CompositeDevice> composite);
  fbl::RefPtr<devmgr::CompositeDevice> take_composite();

 private:
  zx_device() = default;

  friend class fbl::Recyclable<zx_device_t>;
  void fbl_recycle();

  // Templates that dispatch the protocol operations if they were set.
  // If they were not set, the second paramater is returned to the caller
  // (usually ZX_ERR_NOT_SUPPORTED)
  template <typename RetType, typename... ArgTypes>
  RetType Dispatch(RetType (*op)(void* ctx, ArgTypes...), RetType fallback, ArgTypes... args) {
    return op ? (*op)(ctx, args...) : fallback;
  }

  template <typename... ArgTypes>
  void Dispatch(void (*op)(void* ctx, ArgTypes...), ArgTypes... args) {
    if (op) {
      (*op)(ctx, args...);
    }
  }

  // If this device is a component of a composite, this points to the
  // composite control structure.
  fbl::RefPtr<devmgr::CompositeDevice> composite_;

  fbl::WAVLTreeNodeState<fbl::RefPtr<zx_device>> local_id_node_;

  // Identifier assigned by devmgr that can be used to assemble composite
  // devices.
  uint64_t local_id_ = 0;

  fbl::Mutex bind_conn_lock_;
  fbl::Vector<fs::FidlConnection> bind_conn_ TA_GUARDED(bind_conn_lock_);

  // The connection associated with fuchsia.device.Controller/RunCompatibilityTests
  fbl::Mutex test_compatibility_conn_lock_;
  fbl::Vector<fs::FidlConnection> test_compatibility_conn_
                                        TA_GUARDED(test_compatibility_conn_lock_);
};

// zx_device_t objects must be created or initialized by the driver manager's
// device_create() function.  Drivers MAY NOT touch any
// fields in the zx_device_t, except for the protocol_id and protocol_ops
// fields which it may fill out after init and before device_add() is called,
// and the ctx field which may be used to store driver-specific data.

// clang-format off

#define DEV_FLAG_DEAD                  0x00000001  // being deleted
#define DEV_FLAG_VERY_DEAD             0x00000002  // safe for ref0 and release()
#define DEV_FLAG_UNBINDABLE            0x00000004  // nobody may bind to this device
#define DEV_FLAG_BUSY                  0x00000010  // device being created
#define DEV_FLAG_INSTANCE              0x00000020  // this device was created-on-open
#define DEV_FLAG_MULTI_BIND            0x00000080  // this device accepts many children
#define DEV_FLAG_ADDED                 0x00000100  // device_add() has been called for this device
#define DEV_FLAG_INVISIBLE             0x00000200  // device not visible via devfs
#define DEV_FLAG_UNBOUND               0x00000400  // informed that it should self-delete asap
#define DEV_FLAG_WANTS_REBIND          0x00000800  // when last child goes, rebind this device
#define DEV_FLAG_ALLOW_MULTI_COMPOSITE 0x00001000 // can be part of multiple composite devices
// clang-format on

zx_status_t device_bind(const fbl::RefPtr<zx_device_t>& dev, const char* drv_libname);
zx_status_t device_unbind(const fbl::RefPtr<zx_device_t>& dev);
zx_status_t device_run_compatibility_tests(const fbl::RefPtr<zx_device_t>& dev,
                                           int64_t hook_wait_time);
zx_status_t device_open(const fbl::RefPtr<zx_device_t>& dev, fbl::RefPtr<zx_device_t>* out,
                        uint32_t flags);
// Note that device_close() is intended to consume a reference (logically, the
// one created by device_open).
zx_status_t device_close(fbl::RefPtr<zx_device_t> dev, uint32_t flags);

#endif  // ZIRCON_SYSTEM_CORE_DEVMGR_DEVHOST_ZX_DEVICE_H_
