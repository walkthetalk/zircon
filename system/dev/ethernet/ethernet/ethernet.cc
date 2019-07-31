// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ethernet.h"
#include <type_traits>

namespace eth {

TransmitInfo* EthDev0::NetbufToTransmitInfo(ethernet_netbuf_t* netbuf) {
    // NOTE: Alignment is guaranteed by the static_asserts for alignment and padding of the
    // TransmitInfo structure, combined with the value of transmit_buffer_size_.
    return reinterpret_cast<TransmitInfo*>(reinterpret_cast<uintptr_t>(netbuf) + info_.netbuf_size);
}

ethernet_netbuf_t* EthDev0::TransmitInfoToNetbuf(TransmitInfo* transmit_info) {
    return reinterpret_cast<ethernet_netbuf_t*>(reinterpret_cast<uintptr_t>(transmit_info) -
                                              info_.netbuf_size);
}

zx_status_t EthDev::PromiscHelperLogicLocked(bool req_on, uint32_t state_bit,
                                             uint32_t param_id, int32_t* requesters_count) {
    if (state_bit == 0 || state_bit & (state_bit - 1)) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (!req_on == !(state_ & state_bit)) {
        return ZX_OK; // Duplicate request
    }

    zx_status_t status = ZX_OK;
    if (req_on) {
        (*requesters_count)++;
        state_ |= state_bit;
        if (*requesters_count == 1) {
            status = edev0_->mac_.SetParam(param_id, true, nullptr, 0);
            if (status != ZX_OK) {
                (*requesters_count)--;
                state_ &= ~state_bit;
            }
        }
    } else {
        (*requesters_count)--;
        state_ &= ~state_bit;
        if (*requesters_count == 0) {
            status = edev0_->mac_.SetParam(param_id, false, nullptr, 0);
            if (status != ZX_OK) {
                (*requesters_count)++;
                state_ |= state_bit;
            }
        }
    }
    return status;
}

zx_status_t EthDev::SetPromiscLocked(bool req_on) {
    return PromiscHelperLogicLocked(req_on, kStatePromiscuous,
                                    ETHERNET_SETPARAM_PROMISC,
                                    &edev0_->promisc_requesters_);
}

zx_status_t EthDev::SetMulticastPromiscLocked(bool req_on) {
    return PromiscHelperLogicLocked(req_on, kStateMulticastPromiscuous,
                                    ETHERNET_SETPARAM_MULTICAST_PROMISC,
                                    &edev0_->multicast_promisc_requesters_);
}

zx_status_t EthDev::RebuildMulticastFilterLocked() {
    uint8_t multicast[kMulticastListLimit][ETH_MAC_SIZE];
    uint32_t n_multicast = 0;

    for (auto& edev_i : edev0_->list_active_) {
        for (uint32_t i = 0; i < edev_i.num_multicast_; i++) {
            if (n_multicast == kMulticastListLimit) {
                return edev0_->mac_.SetParam(ETHERNET_SETPARAM_MULTICAST_FILTER,
                                             ETHERNET_MULTICAST_FILTER_OVERFLOW, nullptr, 0);
            }
            memcpy(multicast[n_multicast], edev_i.multicast_[i], ETH_MAC_SIZE);
            n_multicast++;
        }
    }
    return edev0_->mac_.SetParam(ETHERNET_SETPARAM_MULTICAST_FILTER, n_multicast, multicast,
                                 n_multicast * ETH_MAC_SIZE);
}

int EthDev::MulticastAddressIndex(const uint8_t* mac) {
    for (uint32_t i = 0; i < num_multicast_; i++) {
        if (!memcmp(multicast_[i], mac, ETH_MAC_SIZE)) {
            return i;
        }
    }
    return -1;
}

zx_status_t EthDev::AddMulticastAddressLocked(const uint8_t* mac) {
    if (!(mac[0] & 1)) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (MulticastAddressIndex(mac) != -1) {
        return ZX_OK;
    }
    if (num_multicast_ < kMulticastListLimit) {
        memcpy(multicast_[num_multicast_], mac, ETH_MAC_SIZE);
        num_multicast_++;
        return RebuildMulticastFilterLocked();
    } else {
        return edev0_->mac_.SetParam(ETHERNET_SETPARAM_MULTICAST_FILTER,
                                     ETHERNET_MULTICAST_FILTER_OVERFLOW, nullptr, 0);
    }
    return ZX_OK;
}

zx_status_t EthDev::DelMulticastAddressLocked(const uint8_t* mac) {
    int ix = MulticastAddressIndex(mac);
    if (ix == -1) {
        // We may have overflowed the list and not remember an address. Nothing will go wrong if
        // they try to stop listening to an address they never added.
        return ZX_OK;
    }
    num_multicast_--;
    memcpy(&multicast_[ix], &multicast_[num_multicast_], ETH_MAC_SIZE);
    return RebuildMulticastFilterLocked();
}

// The thread safety analysis cannot reason through the aliasing of
// edev0 and edev->edev0__, so disable it.
zx_status_t EthDev::TestClearMulticastPromiscLocked() TA_NO_THREAD_SAFETY_ANALYSIS {
    zx_status_t status = ZX_OK;
    for (auto& edev_i : edev0_->list_active_) {
        if ((status = edev_i.SetMulticastPromiscLocked(false)) != ZX_OK) {
            return status;
        }
    }
    return status;
}

void EthDev::RecvLocked(const void* data, size_t len, uint32_t extra) {
    zx_status_t status;
    size_t count;

    if (receive_fifo_entry_count_ == 0) {
        status = receive_fifo_.read(sizeof(receive_fifo_entries_[0]), receive_fifo_entries_,
                                    countof(receive_fifo_entries_), &count);
        if (status != ZX_OK) {
            if (status == ZX_ERR_SHOULD_WAIT) {
                fail_receive_read_ += 1;
                if (fail_receive_read_ == 1 || (fail_receive_read_ % kFailureReportRate) == 0) {
                    // TODO(bbosak): Printing this warning
                    // can result in more dropped packets.
                    // Find a better way to log this.
                    zxlogf(WARN,
                           "eth [%s]: warning: no rx buffers available, frame dropped "
                           "(%u time%s)\n",
                           name_, fail_receive_read_, fail_receive_read_ > 1 ? "s" : "");
                }
            } else {
                // Fatal, should force teardown
                zxlogf(ERROR, "eth [%s]: rx fifo read failed %d\n", name_, status);
            }
            return;
        }
        receive_fifo_entry_count_ = count;
    }

    eth_fifo_entry_t* e = &receive_fifo_entries_[--receive_fifo_entry_count_];
    if ((e->offset >= io_buffer_.size()) || ((e->length > (io_buffer_.size() - e->offset)))) {
        // Invalid offset/length. Report error. Drop packet
        e->length = 0;
        e->flags = ETH_FIFO_INVALID;
    } else if (len > e->length) {
        e->length = 0;
        e->flags = ETH_FIFO_INVALID;
    } else {
        // Packet fits. Deliver it
        memcpy(reinterpret_cast<uint8_t*>(io_buffer_.start()) + e->offset, data, len);
        e->length = static_cast<uint16_t>(len);
        e->flags = static_cast<uint16_t>(ETH_FIFO_RX_OK | extra);
    }

    if ((status = receive_fifo_.write(sizeof(*e), e, 1, nullptr)) < 0) {
        if (status == ZX_ERR_SHOULD_WAIT) {
            if ((fail_receive_write_++ % kFailureReportRate) == 0) {
                zxlogf(ERROR, "eth [%s]: no rx_fifo space available (%u times)\n",
                       name_, fail_receive_write_);
            }
        } else {
            // Fatal, should force teardown.
            zxlogf(ERROR, "eth [%s]: rx_fifo write failed %d\n", name_, status);
        }
        return;
    }
}

int EthDev::TransmitFifoWrite(eth_fifo_entry_t* entries,
                              size_t count) {
    zx_status_t status;
    size_t actual;
    // Writing should never fail, or fail to write all entries.
    status = transmit_fifo_.write(sizeof(eth_fifo_entry_t), entries,
                                  count, &actual);
    if (status < 0) {
        zxlogf(ERROR, "eth [%s]: tx_fifo write failed %d\n", name_, status);
        return -1;
    }
    if (actual != count) {
        zxlogf(ERROR, "eth [%s]: tx_fifo: only wrote %zu of %zu!\n", name_, actual, count);
        return -1;
    }
    return 0;
}

// Borrows a TX buffer from the pool. Logs and returns nullptr if none is available.
TransmitInfo* EthDev::GetTransmitInfo() {
    fbl::AutoLock lock(&lock_);
    TransmitInfo* transmit_info = list_remove_head_type(&free_transmit_buffers_,
                                                        TransmitInfo, node);
    if (transmit_info == nullptr) {
        zxlogf(ERROR, "eth [%s]: transmit_info pool empty\n", name_);
    }
    new (transmit_info) TransmitInfo();
    transmit_info->edev = fbl::RefPtr<EthDev>(this);
    return transmit_info;
}

// Returns a TX buffer to the pool.
void EthDev::PutTransmitInfo(TransmitInfo* transmit_info) {
    // Call the destructor on TransmitInfo since we are effectively "freeing" the
    // TransmitInfo structure. This needs to be done manually, since it is an inline structure.
    transmit_info->~TransmitInfo();
    fbl::AutoLock lock(&lock_);
    list_add_head(&free_transmit_buffers_, &transmit_info->node);
}

void EthDev0::SetStatus(uint32_t status) {
    zxlogf(TRACE, "eth: status() %08x\n", status);

    fbl::AutoLock lock(&ethdev_lock_);
    static_assert(ETHERNET_STATUS_ONLINE == fuchsia_hardware_ethernet_DEVICE_STATUS_ONLINE, "");
    status_ = status;

    static_assert(fuchsia_hardware_ethernet_SIGNAL_STATUS == ZX_USER_SIGNAL_0, "");
    for (auto& edev : list_active_) {
        edev.receive_fifo_.signal_peer(0, fuchsia_hardware_ethernet_SIGNAL_STATUS);
    }
}

// The thread safety analysis cannot reason through the aliasing of
// edev0 and edev->edev0_, so disable it.
// TODO: I think if this arrives at the wrong time during teardown we
// can deadlock with the ethermac device.
void EthDev0::Recv(const void* data, size_t len, uint32_t flags) TA_NO_THREAD_SAFETY_ANALYSIS {
    if (!data || !len) {
        return;
    }
    fbl::AutoLock lock(&ethdev_lock_);
    for (auto& edev : list_active_) {
        edev.RecvLocked(data, len, 0);
    }
}

void EthDev0::CompleteTx(ethernet_netbuf_t* netbuf, zx_status_t status) {
    if (!netbuf) {
        return;
    }
    TransmitInfo* transmit_info = NetbufToTransmitInfo(netbuf);
    auto edev = transmit_info->edev;
    eth_fifo_entry_t entry = {
        .offset = static_cast<uint32_t>(reinterpret_cast<const char*>(netbuf->data_buffer) -
                                        reinterpret_cast<const char*>(edev->io_buffer_.start())),
        .length = static_cast<uint16_t>(netbuf->data_size),
        .flags = static_cast<uint16_t>(status == ZX_OK ? ETH_FIFO_TX_OK : 0),
        .cookie = transmit_info->fifo_cookie};

    // Now that we've copied all pertinent data from the netbuf, return it to the free list so
    // it is available immediately for the next request.
    edev->PutTransmitInfo(transmit_info);

    // Send the entry back to the client.
    edev->TransmitFifoWrite(&entry, 1);
    edev->ethernet_response_count_++;
}

ethernet_ifc_protocol_ops_t ethernet_ifc = {
    .status =
        [](void* cookie, uint32_t status) {
            reinterpret_cast<EthDev0*>(cookie)->SetStatus(status);
        },
    .recv =
        [](void* cookie, const void* data, size_t len, uint32_t flags) {
            reinterpret_cast<EthDev0*>(cookie)->Recv(data, len, flags);
        },
    .complete_tx =
        [](void* cookie, ethernet_netbuf_t* netbuf, zx_status_t status) {
            reinterpret_cast<EthDev0*>(cookie)->CompleteTx(netbuf, status);
        },
};

// The thread safety analysis cannot reason through the aliasing of
// edev0 and edev->edev0_, so disable it.
void EthDev0::TransmitEcho(const void* data, size_t len) TA_NO_THREAD_SAFETY_ANALYSIS {
    fbl::AutoLock lock(&ethdev_lock_);
    for (auto& edev : list_active_) {
        if (edev.state_ & EthDev::kStateTransmissionListen) {
            edev.RecvLocked(data, len, ETH_FIFO_RX_TX);
        }
    }
}

zx_status_t EthDev::TransmitListenLocked(bool yes) {
    // Update our state.
    if (yes) {
        state_ |= kStateTransmissionListen;
    } else {
        state_ &= (~kStateTransmissionListen);
    }

    // Determine global state.
    yes = false;
    for (auto& edev_i : edev0_->list_active_) {
        if (edev_i.state_ & kStateTransmissionListen) {
            yes = true;
        }
    }

    // Set everyone's echo flag based on global state.
    for (auto& edev_i : edev0_->list_active_) {
        if (yes) {
            edev_i.state_ |= kStateTransmissionLoopback;
        } else {
            edev_i.state_ &= (~kStateTransmissionLoopback);
        }
    }

    return ZX_OK;
}

// The array of entries is invalidated after the call.
int EthDev::Send(eth_fifo_entry_t* entries, size_t count) {
    TransmitInfo* transmit_info = nullptr;
    // The entries that we can't send back to the fifo immediately are filtered
    // out in-place using a classic algorithm a-la "std::remove_if".
    // Once the loop finishes, the first 'to_write' entries in the array
    // will be written back to the fifo. The rest will be written later by
    // the eth0_complete_tx callback.
    uint32_t to_write = 0;
    for (eth_fifo_entry_t* e = entries; count > 0; e++) {
        if ((e->offset > io_buffer_.size()) || ((e->length > (io_buffer_.size() - e->offset)))) {
            e->flags = ETH_FIFO_INVALID;
            entries[to_write++] = *e;
        } else {
            zx_status_t status;
            if (transmit_info == nullptr) {
                transmit_info = GetTransmitInfo();
                if (transmit_info == nullptr) {
                    return -1;
                }
            }
            uint32_t opts = count > 1 ? ETHERNET_TX_OPT_MORE : 0u;
            if (opts) {
                zxlogf(SPEW, "setting OPT_MORE (%lu packets to go)\n", count);
            }
            ethernet_netbuf_t* netbuf = edev0_->TransmitInfoToNetbuf(transmit_info);
            netbuf->data_buffer = reinterpret_cast<char*>(io_buffer_.start()) + e->offset;
            if (edev0_->info_.features & ETHERNET_FEATURE_DMA) {
                netbuf->phys = paddr_map_[e->offset / PAGE_SIZE] +
                               (e->offset & kPageMask);
            }
            netbuf->data_size = e->length;
            transmit_info->fifo_cookie = e->cookie;
            status = edev0_->mac_.QueueTx(opts, netbuf);
            if (state_ & kStateTransmissionLoopback) {
                edev0_->TransmitEcho(
                    reinterpret_cast<char*>(io_buffer_.start()) + e->offset, e->length);
            }
            if (status != ZX_ERR_SHOULD_WAIT) {
                // Transmission completed. To avoid extra mutex locking/unlocking,
                // we don't return the buffer to the pool immediately, but reuse
                // it on the next iteration of the loop.
                e->flags = status == ZX_OK ? ETH_FIFO_TX_OK : 0;
                entries[to_write++] = *e;
            } else {
                // The ownership of the TX buffer is transferred to mac_.QueueTx().
                // We can't reuse it, so clear the pointer.
                transmit_info = nullptr;
                ethernet_request_count_++;
            }
        }
        count--;
    }
    if (transmit_info) {
        PutTransmitInfo(transmit_info);
    }
    if (to_write) {
        TransmitFifoWrite(entries, to_write);
    }
    return 0;
}

int EthDev::TransmitThread() {
    eth_fifo_entry_t entries[kFifoDepth / 2];
    zx_status_t status;
    size_t count;

    for (;;) {
        if ((status = transmit_fifo_.read(sizeof(entries[0]), entries,
                                          countof(entries), &count)) < 0) {
            if (status == ZX_ERR_SHOULD_WAIT) {
                zx_signals_t observed;
                if ((status = transmit_fifo_.wait_one(ZX_FIFO_READABLE |
                                                          ZX_FIFO_PEER_CLOSED |
                                                          kSignalFifoTerminate,
                                                      zx::time::infinite(),
                                                      &observed)) < 0) {
                    zxlogf(ERROR, "eth [%s]: tx_fifo: error waiting: %d\n", name_, status);
                    break;
                }
                if (observed & kSignalFifoTerminate)
                    break;
                continue;
            } else {
                zxlogf(ERROR, "eth [%s]: tx_fifo: cannot read: %d\n", name_, status);
                break;
            }
        }
        if (Send(entries, count)) {
            break;
        }
    }

    zxlogf(INFO, "eth [%s]: tx_thread: exit: %d\n", name_, status);
    return 0;
}

zx_status_t EthDev::GetFifosLocked(struct fuchsia_hardware_ethernet_Fifos* fifos) {
    zx_status_t status;
    struct fuchsia_hardware_ethernet_Fifos temp_fifo;
    zx::fifo transmit_fifo;
    zx::fifo receive_fifo;
    if ((status = zx_fifo_create(kFifoDepth, kFifoEntrySize, 0,
                                 &temp_fifo.tx, transmit_fifo.reset_and_get_address())) < 0) {
        zxlogf(ERROR, "eth_create  [%s]: failed to create tx fifo: %d\n", name_, status);
        return status;
    }
    if ((status = zx_fifo_create(kFifoDepth, kFifoEntrySize, 0,
                                 &temp_fifo.rx, receive_fifo.reset_and_get_address())) < 0) {
        zxlogf(ERROR, "eth_create  [%s]: failed to create rx fifo: %d\n", name_, status);
        zx_handle_close(temp_fifo.tx);
        return status;
    }

    *fifos = temp_fifo;
    transmit_fifo_ = std::move(transmit_fifo);
    receive_fifo_ = std::move(receive_fifo);
    transmit_fifo_depth_ = kFifoDepth;
    receive_fifo_depth_ = kFifoDepth;
    fifos->tx_depth = kFifoDepth;
    fifos->rx_depth = kFifoDepth;

    return ZX_OK;
}

zx_status_t EthDev::SetIObufLocked(zx_handle_t vmo) {
    if (io_vmo_.is_valid() || io_buffer_.start() != nullptr) {
        return ZX_ERR_ALREADY_BOUND;
    }

    size_t size;
    zx_status_t status;
    zx::vmo io_vmo = zx::vmo(vmo);
    fzl::VmoMapper io_buffer;
    fbl::unique_ptr<zx_paddr_t[]> paddr_map = nullptr;
    zx::pmt pmt;

    if ((status = io_vmo.get_size(&size)) < 0) {
        zxlogf(ERROR, "eth [%s]: could not get io_buf size: %d\n", name_, status);
        return status;
    }

    if ((status = io_buffer.Map(io_vmo, 0, size,
                                ZX_VM_PERM_READ | ZX_VM_PERM_WRITE |
                                    ZX_VM_REQUIRE_NON_RESIZABLE,
                                NULL)) < 0) {
        zxlogf(ERROR, "eth [%s]: could not map io_buf: %d\n", name_, status);
        return status;
    }

    // If the driver indicates that it will be doing DMA to/from the vmo,
    // We pin the memory and cache the physical address list.
    if (edev0_->info_.features & ETHERNET_FEATURE_DMA) {
        fbl::AllocChecker ac;
        size_t pages = ROUNDUP(size, PAGE_SIZE) / PAGE_SIZE;
        paddr_map = fbl::unique_ptr<zx_paddr_t[]>(new (&ac) zx_paddr_t[pages]);
        if (!ac.check()) {
            status = ZX_ERR_NO_MEMORY;
            return status;
        }
        zx::bti bti = zx::bti();
        edev0_->mac_.GetBti(&bti);
        if (!bti.is_valid()) {
            status = ZX_ERR_INTERNAL;
            zxlogf(ERROR, "eth [%s]: ethernet_impl_get_bti return invalid handle\n", name_);
            return status;
        }
        if ((status = bti.pin(ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE,
                              io_vmo, 0, size, paddr_map.get(), pages, &pmt)) != ZX_OK) {
            zxlogf(ERROR, "eth [%s]: bti_pin failed, can't pin vmo: %d\n",
                   name_, status);
            return status;
        }
    }

    io_vmo_ = std::move(io_vmo);
    paddr_map_ = std::move(paddr_map);
    io_buffer_ = std::move(io_buffer);
    pmt_ = std::move(pmt);

    return ZX_OK;
}

// The thread safety analysis cannot reason through the aliasing of
// edev0 and edev->edev0_, so disable it.
zx_status_t EthDev::StartLocked() TA_NO_THREAD_SAFETY_ANALYSIS {
    // Cannot start unless tx/rx rings are configured.
    if ((!io_vmo_.is_valid()) ||
        (!transmit_fifo_.is_valid()) ||
        (!receive_fifo_.is_valid())) {
        return ZX_ERR_BAD_STATE;
    }

    if (state_ & kStateRunning) {
        return ZX_OK;
    }

    if (!(state_ & kStateTransmitThreadCreated)) {
        int r = thrd_create_with_name(
            &transmit_thread_, [](void* arg) -> int {
                return static_cast<EthDev*>(arg)->TransmitThread();
            },
            this, "eth-tx-thread");
        if (r != thrd_success) {
            zxlogf(ERROR, "eth [%s]: failed to start tx thread: %d\n", name_, r);
            return ZX_ERR_INTERNAL;
        }
        state_ |= kStateTransmitThreadCreated;
    }

    zx_status_t status;
    if (edev0_->list_active_.is_empty()) {
        // Release the lock to allow other device operations in callback routine.
        // Re-acquire lock afterwards.
        edev0_->ethdev_lock_.Release();
        status = edev0_->mac_.Start(edev0_, &ethernet_ifc);
        edev0_->ethdev_lock_.Acquire();
        // Check whether unbind was called while we were unlocked.
        if (state_ & kStateDead) {
            status = ZX_ERR_BAD_STATE;
        }
    } else {
        status = ZX_OK;
    }

    if (status == ZX_OK) {
        state_ |= kStateRunning;
        edev0_->list_idle_.erase(*this);
        edev0_->list_active_.push_back(fbl::WrapRefPtr(this));
        // Trigger the status signal so the client will query the status at the start.
        receive_fifo_.signal_peer(0, fuchsia_hardware_ethernet_SIGNAL_STATUS);
    } else {
        zxlogf(ERROR, "eth [%s]: failed to start mac: %d\n", name_, status);
    }

    return status;
}

// The thread safety analysis cannot reason through the aliasing of
// edev0 and edev->edev0_, so disable it.
zx_status_t EthDev::StopLocked() TA_NO_THREAD_SAFETY_ANALYSIS {
    if (state_ & kStateRunning) {
        state_ &= (~kStateRunning);
        edev0_->list_active_.erase(*this);
        edev0_->list_idle_.push_back(fbl::WrapRefPtr(this));
        // The next three lines clean up promisc, multicast-promisc, and multicast-filter, in case
        // this ethdev had any state set. Ignore failures, which may come from drivers not
        // supporting the feature. (TODO: check failure codes).
        SetPromiscLocked(false);
        SetMulticastPromiscLocked(false);
        RebuildMulticastFilterLocked();
        if (edev0_->list_active_.is_empty()) {
            if (!(state_ & kStateDead)) {
                // Release the lock to allow other device operations in callback routine.
                // Re-acquire lock afterwards.
                edev0_->ethdev_lock_.Release();
                edev0_->mac_.Stop();
                edev0_->ethdev_lock_.Acquire();
            }
        }
    }

    return ZX_OK;
}

zx_status_t EthDev::SetClientNameLocked(const void* in_buf, size_t in_len) {
    if (in_len >= sizeof(name_)) {
        in_len = sizeof(name_) - 1;
    }
    memcpy(name_, in_buf, in_len);
    name_[in_len] = '\0';
    return ZX_OK;
}

zx_status_t EthDev::GetStatusLocked(void* out_buf, size_t out_len,
                                    size_t* out_actual) {
    if (out_len < sizeof(uint32_t)) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (!receive_fifo_.is_valid()) {
        return ZX_ERR_BAD_STATE;
    }
    if (receive_fifo_.signal_peer(fuchsia_hardware_ethernet_SIGNAL_STATUS, 0) != ZX_OK) {
        return ZX_ERR_INTERNAL;
    }

    uint32_t* status = reinterpret_cast<uint32_t*>(out_buf);
    *status = edev0_->status_;
    *out_actual = sizeof(*status);
    return ZX_OK;
}

#define REPLY(x) fuchsia_hardware_ethernet_Device##x##_reply

zx_status_t EthDev::MsgGetInfoLocked(fidl_txn_t* txn) {
    fuchsia_hardware_ethernet_Info info = {};
    memcpy(info.mac.octets, edev0_->info_.mac, ETH_MAC_SIZE);
    if (edev0_->info_.features & ETHERNET_FEATURE_WLAN) {
        info.features |= fuchsia_hardware_ethernet_INFO_FEATURE_WLAN;
    }
    if (edev0_->info_.features & ETHERNET_FEATURE_SYNTH) {
        info.features |= fuchsia_hardware_ethernet_INFO_FEATURE_SYNTH;
    }
    info.mtu = edev0_->info_.mtu;
    return REPLY(GetInfo)(txn, &info);
}

zx_status_t EthDev::MsgGetFifosLocked(fidl_txn_t* txn) {
    fuchsia_hardware_ethernet_Fifos fifos;
    return REPLY(GetFifos)(txn, GetFifosLocked(&fifos), &fifos);
}

zx_status_t EthDev::MsgSetIOBufferLocked(zx_handle_t h, fidl_txn_t* txn) {
    return REPLY(SetIOBuffer)(txn, SetIObufLocked(h));
}

zx_status_t EthDev::MsgStartLocked(fidl_txn_t* txn) {
    return REPLY(Start)(txn, StartLocked());
}

zx_status_t EthDev::MsgStopLocked(fidl_txn_t* txn) {
    StopLocked();
    return REPLY(Stop)(txn);
}

zx_status_t EthDev::MsgListenStartLocked(fidl_txn_t* txn) {
    return REPLY(ListenStart)(txn, TransmitListenLocked(true));
}

zx_status_t EthDev::MsgListenStopLocked(fidl_txn_t* txn) {
    TransmitListenLocked(false);
    return REPLY(ListenStop)(txn);
}

zx_status_t EthDev::MsgSetClientNameLocked(const char* buf, size_t len,
                                           fidl_txn_t* txn) {
    return REPLY(SetClientName)(txn, SetClientNameLocked(buf, len));
}

zx_status_t EthDev::MsgGetStatusLocked(fidl_txn_t* txn) {
    if (!receive_fifo_.is_valid()) {
        return ZX_ERR_BAD_STATE;
    }

    if (receive_fifo_.signal_peer(fuchsia_hardware_ethernet_SIGNAL_STATUS, 0) != ZX_OK) {
        return ZX_ERR_INTERNAL;
    }

    return REPLY(GetStatus)(txn, edev0_->status_);
}

zx_status_t EthDev::MsgSetPromiscLocked(bool enabled, fidl_txn_t* txn) {
    return REPLY(SetPromiscuousMode)(txn, SetPromiscLocked(enabled));
}

zx_status_t EthDev::MsgConfigMulticastAddMacLocked(const fuchsia_hardware_ethernet_MacAddress* mac,
                                                   fidl_txn_t* txn) {
    zx_status_t status = AddMulticastAddressLocked(mac->octets);
    return REPLY(ConfigMulticastAddMac)(txn, status);
}

zx_status_t
EthDev::MsgConfigMulticastDeleteMacLocked(const fuchsia_hardware_ethernet_MacAddress* mac,
                                          fidl_txn_t* txn) {
    zx_status_t status = DelMulticastAddressLocked(mac->octets);
    return REPLY(ConfigMulticastDeleteMac)(txn, status);
}

zx_status_t EthDev::MsgConfigMulticastSetPromiscuousModeLocked(bool enabled,
                                                               fidl_txn_t* txn) {
    zx_status_t status = SetMulticastPromiscLocked(enabled);
    return REPLY(ConfigMulticastSetPromiscuousMode)(txn, status);
}

zx_status_t EthDev::MsgConfigMulticastTestFilterLocked(fidl_txn_t* txn) {
    zxlogf(INFO,
           "MULTICAST_TEST_FILTER invoked. Turning multicast-promisc off unconditionally.\n");
    zx_status_t status = TestClearMulticastPromiscLocked();
    return REPLY(ConfigMulticastTestFilter)(txn, status);
}

zx_status_t EthDev::MsgDumpRegistersLocked(fidl_txn_t* txn) {
    zx_status_t status = edev0_->mac_.SetParam(ETHERNET_SETPARAM_DUMP_REGS, 0, nullptr, 0);
    return REPLY(DumpRegisters)(txn, status);
}

#undef REPLY

static const fuchsia_hardware_ethernet_Device_ops_t* FIDLOps() {
    using Binder = fidl::Binder<EthDev>;

    static fuchsia_hardware_ethernet_Device_ops_t kOps = {
        .GetInfo =
            Binder::BindMember<&EthDev::MsgGetInfoLocked>,
        .GetFifos =
            Binder::BindMember<&EthDev::MsgGetFifosLocked>,
        .SetIOBuffer =
            Binder::BindMember<&EthDev::MsgSetIOBufferLocked>,
        .Start =
            Binder::BindMember<&EthDev::MsgStartLocked>,
        .Stop =
            Binder::BindMember<&EthDev::MsgStopLocked>,
        .ListenStart =
            Binder::BindMember<&EthDev::MsgListenStartLocked>,
        .ListenStop =
            Binder::BindMember<&EthDev::MsgListenStopLocked>,
        .SetClientName =
            Binder::BindMember<&EthDev::MsgSetClientNameLocked>,
        .GetStatus =
            Binder::BindMember<&EthDev::MsgGetStatusLocked>,
        .SetPromiscuousMode =
            Binder::BindMember<&EthDev::MsgSetPromiscLocked>,
        .ConfigMulticastAddMac =
            Binder::BindMember<&EthDev::MsgConfigMulticastAddMacLocked>,
        .ConfigMulticastDeleteMac =
            Binder::BindMember<&EthDev::MsgConfigMulticastDeleteMacLocked>,
        .ConfigMulticastSetPromiscuousMode =
            Binder::BindMember<&EthDev::MsgConfigMulticastSetPromiscuousModeLocked>,
        .ConfigMulticastTestFilter =
            Binder::BindMember<&EthDev::MsgConfigMulticastTestFilterLocked>,
        .DumpRegisters =
            Binder::BindMember<&EthDev::MsgDumpRegistersLocked>,
    };
    return &kOps;
}

zx_status_t EthDev::DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn) {
    fbl::AutoLock lock(&edev0_->ethdev_lock_);
    if (state_ & kStateDead) {
        return ZX_ERR_BAD_STATE;
    }
    zx_status_t status = fuchsia_hardware_ethernet_Device_dispatch(this, txn, msg, FIDLOps());
    return status;
}

// Kill transmit thread, release buffers, etc
// called from unbind and close.
void EthDev::KillLocked() {
    if (state_ & kStateDead) {
        return;
    }

    //Ensure that all requests to ethmac were completed.
    ZX_DEBUG_ASSERT(ethernet_request_count_ == ethernet_response_count_);

    zxlogf(TRACE, "eth [%s]: kill: tearing down%s\n",
           name_, (state_ & kStateTransmitThreadCreated) ? " tx thread" : "");
    SetPromiscLocked(false);

    // Make sure any future ioctls or other ops will fail.
    state_ |= kStateDead;

    // Try to convince clients to close us.
    if (receive_fifo_.is_valid()) {
        receive_fifo_.reset();
    }
    if (transmit_fifo_.is_valid()) {
        // Ask the Transmit thread to exit.
        transmit_fifo_.signal(0, kSignalFifoTerminate);
    }

    if (io_vmo_.is_valid()) {
        io_vmo_.reset();
    }

    if (state_ & kStateTransmitThreadCreated) {
        state_ &= (~kStateTransmitThreadCreated);
        int ret;
        thrd_join(transmit_thread_, &ret);
        zxlogf(TRACE, "eth [%s]: kill: tx thread exited\n", name_);
    }

    if (transmit_fifo_.is_valid()) {
        transmit_fifo_.reset();
    }

    io_buffer_.Unmap();

    if (paddr_map_ != nullptr) {
        if (pmt_.unpin() != ZX_OK) {
            zxlogf(ERROR, "eth [%s]: cannot unpin vmo?!\n", name_);
        }
        paddr_map_ = nullptr;
        pmt_.reset();
    }
    zxlogf(TRACE, "eth [%s]: all resources released\n", name_);
}

void EthDev::StopAndKill() {
    fbl::AutoLock lock(&edev0_->ethdev_lock_);
    StopLocked();
    SetPromiscLocked(false);
    if (transmit_fifo_.is_valid()) {
        // Ask the Transmit thread to exit.
        transmit_fifo_.signal(0, kSignalFifoTerminate);
    }
    if (state_ & kStateTransmitThreadCreated) {
        state_ &= (~kStateTransmitThreadCreated);
        int ret;
        thrd_join(transmit_thread_, &ret);
        zxlogf(TRACE, "eth [%s]: kill: tx thread exited\n", name_);
    }
    // Check if it is part of the idle list and remove.
    // It will not be part of active list as StopLocked() would have moved it to Idle.
    if (InContainer()) {
        edev0_->list_idle_.erase(*this);
    }
}

void EthDev::DdkRelease() {
    // Release the device (and wait for completion)!
    if (Release()) {
        delete this;
    } else {
        // TODO (ZX-3934): It is not presently safe to block here.
        // So we cannot satisfy the assumptions of the DDK.
        // If we block here, we will deadlock the entire system
        // due to the virtual bus's control channel being controlled via FIDL.
        // as well as its need to issue lifecycle events to the main event loop
        // in order to remove the bus during shutdown.
        // Uncomment the lines below when we can do so safely.
        // sync_completion_t completion;
        // completion_ = &completion;
        // sync_completion_wait(&completion, ZX_TIME_INFINITE);
    }
}

EthDev::~EthDev() {
    if (transmit_fifo_.is_valid()) {
        // Ask the Transmit thread to exit.
        transmit_fifo_.signal(0, kSignalFifoTerminate);
    }
    if (state_ & kStateTransmitThreadCreated) {
        state_ &= (~kStateTransmitThreadCreated);
        int ret;
        thrd_join(transmit_thread_, &ret);
        zxlogf(TRACE, "eth [%s]: kill: tx thread exited\n", name_);
    }
    // sync_completion_signal(completion_);
}

zx_status_t EthDev::DdkOpen(zx_device_t** out, uint32_t flags) {
    {
        fbl::AutoLock lock(&lock_);
        open_count_++;
    }
    if (out) {
        *out = nullptr;
    }
    return ZX_OK;
}

zx_status_t EthDev::DdkClose(uint32_t flags) {
    bool destroy = false;
    {
        fbl::AutoLock lock(&lock_);
        open_count_--;
        if (open_count_ == 0) {
            destroy = true;
        }
    }

    if (!destroy) {
        return ZX_OK;
    }

    // No more users. Can stop the thread and kill the instance.
    StopAndKill();

    return ZX_OK;
}

zx_status_t EthDev::AddDevice(zx_device_t** out) {
    zx_status_t status;

    transmit_buffer_size_ =
        ROUNDUP(sizeof(TransmitInfo) + edev0_->info_.netbuf_size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    // Ensure that we can meet alignment requirement of TransmitInfo in this allocation,
    // and that sufficient padding exists between elements in the struct to guarantee safe
    // accesses of this array.
    static_assert(std::alignment_of_v<TransmitInfo> <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    static_assert(std::alignment_of_v<TransmitInfo> <= sizeof(ethernet_netbuf_t));
    fbl::AllocChecker ac;
    fbl::unique_ptr<uint8_t[]> all_transmit_buffers =
        fbl::unique_ptr<uint8_t[]>(new (&ac) uint8_t[kFifoDepth * transmit_buffer_size_]());
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    list_initialize(&free_transmit_buffers_);
    for (size_t ndx = 0; ndx < kFifoDepth; ndx++) {
        ethernet_netbuf_t* netbuf = (ethernet_netbuf_t*)((uintptr_t)all_transmit_buffers.get() +
                                                     (transmit_buffer_size_ * ndx));
        TransmitInfo* transmit_info = edev0_->NetbufToTransmitInfo(netbuf);
        list_add_tail(&free_transmit_buffers_, &transmit_info->node);
    }

    if ((status = DdkAdd("ethernet", DEVICE_ADD_INSTANCE, nullptr, 0,
                         ZX_PROTOCOL_ETHERNET)) < 0) {
        list_initialize(&free_transmit_buffers_);
        return status;
    }
    if (out) {
        *out = zxdev_;
    }
    all_transmit_buffers_ = std::move(all_transmit_buffers);

    {
        fbl::AutoLock lock(&edev0_->ethdev_lock_);
        edev0_->list_idle_.push_back(fbl::RefPtr(this));
    }

    return ZX_OK;
}

zx_status_t EthDev0::DdkOpen(zx_device_t** out, uint32_t flags) {
    fbl::AllocChecker ac;
    auto edev = fbl::MakeRefCountedChecked<EthDev>(&ac, this->zxdev_, this);
    // Hold a second reference to the device to prevent a use-after-free
    // in the case where DdkRelease is called immediately after AddDevice.
    fbl::RefPtr<EthDev> dev_ref_2 = edev;
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    // Add a reference for the devhost handle.
    // This will be removed in DdkRelease.
    zx_status_t status;
    if ((status = edev->AddDevice(out)) < 0) {
        return status;
    }

    __UNUSED auto dev = edev.leak_ref();
    return ZX_OK;
}

// The thread safety analysis cannot reason through the aliasing of
// edev0 and edev->edev0_, so disable it.
void EthDev0::DestroyAllEthDev() TA_NO_THREAD_SAFETY_ANALYSIS {
    fbl::AutoLock lock(&ethdev_lock_);
    for (auto itr = list_active_.begin(); itr != list_active_.end();) {
        auto& eth = *itr;
        itr++;
        eth.StopLocked();
    }

    for (auto itr = list_idle_.begin(); itr != list_idle_.end();) {
        auto& eth = *itr;
        itr++;
        eth.KillLocked();
        list_idle_.erase(eth);
    }
}

void EthDev0::DdkUnbind() {
    // Tear down shared memory, fifos, and threads
    // to encourage any open instances to close.
    DestroyAllEthDev();
    // This will trigger DdkCLose() and DdkRelease() of all EthDev.
    DdkRemove();
}

void EthDev0::DdkRelease() {
    // All ethdev devices must have been removed.
    {
        fbl::AutoLock lock(&ethdev_lock_);
        ZX_DEBUG_ASSERT(list_active_.is_empty());
        ZX_DEBUG_ASSERT(list_idle_.is_empty());
    }
    delete this;
}

zx_status_t EthDev0::AddDevice() {
    zx_status_t status;
    ethernet_impl_protocol_ops_t* ops;
    ethernet_impl_protocol_t proto;

    if (!mac_.is_valid()) {
        zxlogf(ERROR, "eth: bind: no ethermac protocol\n");
        return ZX_ERR_INTERNAL;
    }

    mac_.GetProto(&proto);
    ops = proto.ops;
    if (ops->query == nullptr || ops->stop == nullptr || ops->start == nullptr ||
        ops->queue_tx == nullptr || ops->set_param == nullptr) {
        zxlogf(ERROR, "eth: bind: device '%s': incomplete ethermac protocol\n",
               device_get_name(parent_));
        return ZX_ERR_NOT_SUPPORTED;
    }

    if ((status = mac_.Query(0, &info_)) < 0) {
        zxlogf(ERROR, "eth: bind: ethermac query failed: %d\n", status);
        return status;
    }

    if ((info_.features & ETHERNET_FEATURE_DMA) &&
        (ops->get_bti == nullptr)) {
        zxlogf(ERROR, "eth: bind: device '%s': does not implement ops->get_bti()\n",
               device_get_name(parent_));
        return ZX_ERR_NOT_SUPPORTED;
    }

    if (info_.netbuf_size < sizeof(ethernet_netbuf_t)) {
        zxlogf(ERROR, "eth: bind: device '%s': invalid buffer size %ld\n",
               device_get_name(parent_), info_.netbuf_size);
        return ZX_ERR_NOT_SUPPORTED;
    }
    info_.netbuf_size = ROUNDUP(info_.netbuf_size, 8);

    if ((status = DdkAdd("ethernet", 0, nullptr, 0,
                         ZX_PROTOCOL_ETHERNET)) < 0) {
        return status;
    }

    return ZX_OK;
}

zx_status_t EthDev0::EthBind(void* ctx, zx_device_t* dev) {
    fbl::AllocChecker ac;
    auto edev0 = fbl::make_unique_checked<eth::EthDev0>(&ac, dev);

    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    zx_status_t status;
    if ((status = edev0->AddDevice()) != ZX_OK) {
        return status;
    }

    // On successful Add, Devmgr takes ownership (relinquished on DdkRelease),
    // so transfer our ownership to a local var, and let it go out of scope.
    auto __UNUSED temp_ref = edev0.release();

    return ZX_OK;
}

} // namespace eth

static constexpr zx_driver_ops_t eth_driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = &eth::EthDev0::EthBind;
    ops.release = [](void* ctx) {
        // We don't support unloading. Assert if this ever
        // happens. In order to properly support unloading,
        // we need a way to inform the DDK when all of our
        // resources have been freed, so it can safely
        // unload the driver. This mechanism does not currently
        // exist.
        ZX_ASSERT(false);
    };
    return ops;
}();

// clang-format off
ZIRCON_DRIVER_BEGIN(ethernet, eth_driver_ops, "zircon", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_ETHERNET_IMPL)
ZIRCON_DRIVER_END(ethernet)
// clang-format on
