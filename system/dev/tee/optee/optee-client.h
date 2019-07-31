// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/intrusive_hash_table.h>
#include <fuchsia/tee/c/fidl.h>
#include <lib/zx/channel.h>

#include "optee-controller.h"

namespace optee {

class OpteeClient;
using OpteeClientBase = ddk::Device<OpteeClient, ddk::Closable, ddk::Messageable>;
using OpteeClientProtocol = ddk::EmptyProtocol<ZX_PROTOCOL_TEE>;

// The Optee driver allows for simultaneous access from different processes. The OpteeClient object
// is a distinct device instance for each client connection. This allows for per-instance state to
// be managed together. For example, if a client closes the device, OpteeClient can free all of the
// allocated shared memory buffers and sessions that were created by that client without interfering
// with other active clients.

class OpteeClient : public OpteeClientBase,
                    public OpteeClientProtocol,
                    public fbl::DoublyLinkedListable<OpteeClient*> {
public:
    explicit OpteeClient(OpteeController* controller, zx::channel service_provider_channel)
        : OpteeClientBase(controller->zxdev()), controller_(controller),
          service_provider_channel_(std::move(service_provider_channel)) {}

    OpteeClient(const OpteeClient&) = delete;
    OpteeClient& operator=(const OpteeClient&) = delete;

    zx_status_t DdkClose(uint32_t flags);
    void DdkRelease();
    zx_status_t DdkMessage(fidl_msg_t* msg, fidl_txn_t* txn);

    // If the Controller is unbound, we need to notify all clients that the device is no longer
    // available. The Controller will invoke this function so that any subsequent calls on the
    // client will notify the caller that the peer has closed.
    void MarkForClosing() { needs_to_close_ = true; }

    // FIDL Handlers
    zx_status_t GetOsInfo(fidl_txn_t* txn) const;
    zx_status_t OpenSession(const fuchsia_tee_Uuid* trusted_app,
                            const fuchsia_tee_ParameterSet* parameter_set,
                            fidl_txn_t* txn);
    zx_status_t InvokeCommand(uint32_t session_id, uint32_t command_id,
                              const fuchsia_tee_ParameterSet* parameter_set,
                              fidl_txn_t* txn);
    zx_status_t CloseSession(uint32_t session_id, fidl_txn_t* txn);

private:
    using SharedMemoryList = fbl::DoublyLinkedList<fbl::unique_ptr<SharedMemory>>;

    zx_status_t CloseSession(uint32_t session_id);

    // Attempts to allocate a block of SharedMemory from a designated memory pool.
    //
    // On success:
    //  * Tracks the allocated memory block in the allocated_shared_memory_ list.
    //  * Gives the physical address of the memory block in out_phys_addr
    //  * Gives an identifier for the memory block in out_mem_id. This identifier will later be
    //    used to free the memory block.
    //
    // On failure:
    //  * Sets the physical address of the memory block to 0.
    //  * Sets the identifier of the memory block to 0.
    template <typename SharedMemoryPoolTraits>
    zx_status_t AllocateSharedMemory(size_t size,
                                     SharedMemoryPool<SharedMemoryPoolTraits>* memory_pool,
                                     zx_paddr_t* out_phys_addr,
                                     uint64_t* out_mem_id);

    // Frees a block of SharedMemory that was previously allocated by the driver.
    //
    // Parameters:
    //  * mem_id:   The identifier for the memory block to free, given at allocation time.
    //
    // Returns:
    //  * ZX_OK:             Successfully freed the memory.
    //  * ZX_ERR_NOT_FOUND:  Could not find a block corresponding to the identifier given.
    zx_status_t FreeSharedMemory(uint64_t mem_id);

    // Attempts to find a previously allocated block of memory.
    //
    // Returns:
    //  * If the block was found, an iterator object pointing to the SharedMemory block.
    //  * Otherwise, an iterator object pointing to the end of allocated_shared_memory_.
    SharedMemoryList::iterator FindSharedMemory(uint64_t mem_id);

    // Attempts to get a slice of `SharedMemory` representing an OP-TEE memory reference.
    //
    // Parameters:
    //  * mem_iter:   The `SharedMemoryList::iterator` object pointing to the `SharedMemory`.
    //                This may point to the end of `allocated_shared_memory_`.
    //  * base_paddr: The starting base physical address of the slice.
    //  * size:       The size of the slice.
    //
    // Returns:
    //  * If `mem_iter` is valid and the slice bounds are valid, an initialized `std::optional` with
    //    the `SharedMemoryView`.
    //  * Otherwise, an uninitialized `std::optional`.
    std::optional<SharedMemoryView> GetMemoryReference(SharedMemoryList::iterator mem_iter,
                                                       zx_paddr_t base_paddr,
                                                       size_t size);

    // Requests the root storage channel from the `ServiceProvider` and caches it in
    // `root_storage_channel_`.
    //
    // Subsequent calls to the function will return the cached channel.
    //
    // Returns:
    //  * ZX_OK:                The operation was successful.
    //  * ZX_ERR_UNAVAILABLE:   The current client does not have access to a `ServiceProvider`.
    //  * `zx_status_t` codes from `zx::channel::create` or requesting the `ServiceProvider` over
    //    FIDL.
    zx_status_t GetRootStorageChannel(zx::unowned_channel* out_root_channel);

    // Requests a connection to the storage directory pointed to by the path.
    //
    // Parameters:
    //  * path:                 The path of the directory, relative to the root storage directory.
    //  * create:               Flag specifying whether to create directories if they don't exist.
    //  * out_storage_channel:  Where to output the `fuchsia.io.Directory` client end channel.
    zx_status_t GetStorageDirectory(std::filesystem::path path, bool create,
                                    zx::channel* out_storage_channel);

    // Tracks a new file system object associated with the current client.
    //
    // This occurs when the trusted world creates or opens a file system object.
    //
    // Parameters:
    //  * io_node_channel:  The channel to the `fuchsia.io.Node` file system object.
    //
    // Returns:
    //  * The identifier for the trusted world to refer to the object.
    __WARN_UNUSED_RESULT uint64_t TrackFileSystemObject(zx::channel io_node_channel);

    // Gets the channel to the file system object associated with the given identifier.
    //
    // Parameters:
    //  * identifier: The identifier to find the file system object by.
    //
    // Returns:
    //  * A `std::optional` containing a reference to the `zx::channel`, if it was found.
    std::optional<zx::unowned_channel>
    GetFileSystemObjectChannel(uint64_t identifier);

    // Untracks a file system object associated with the current client.
    //
    // This occurs when the trusted world closes a previously open file system object.
    //
    // Parameters:
    //  * identifier:  The identifier to refer to the object.
    //
    // Returns:
    //  * Whether a file system object associated with the identifier was untracked.
    bool UntrackFileSystemObject(uint64_t identifier);

    //
    // OP-TEE RPC Function Handlers
    //
    // The section below outlines the functions that are used to parse and fulfill RPC commands from
    // the OP-TEE secure world.
    //
    // There are two main "types" of functions defined and can be identified by their naming
    // convention:
    //  * "HandleRpc" functions handle the first layer of commands. These are basic, fundamental
    //    commands used for critical tasks like setting up shared memory, notifying the normal world
    //    of interrupts, and accessing the second layer of commands.
    //  * "HandleRpcCommand" functions handle the second layer of commands. These are more advanced
    //    commands, like loading trusted applications and accessing the file system. These make up
    //    the bulk of RPC commands once a session is open.
    //      * HandleRpcCommand is actually a specific command in the first layer that can be invoked
    //        once initial shared memory is set up for the command message.
    //
    // Because these RPCs are the primary channel through which the normal and secure worlds mediate
    // shared resources, it is important that handlers in the normal world are resilient to errors
    // from the trusted world. While we don't expect that the trusted world is actively malicious in
    // any way, we do want handlers to be cautious against buggy or unexpected behaviors, as we do
    // not want errors propagating into the normal world (especially with resources like memory).

    // Identifies and dispatches the first layer of RPC command requests.
    zx_status_t HandleRpc(const RpcFunctionArgs& args, RpcFunctionResult* out_result);
    zx_status_t HandleRpcAllocateMemory(const RpcFunctionAllocateMemoryArgs& args,
                                        RpcFunctionAllocateMemoryResult* out_result);
    zx_status_t HandleRpcFreeMemory(const RpcFunctionFreeMemoryArgs& args,
                                    RpcFunctionFreeMemoryResult* out_result);

    // Identifies and dispatches the second layer of RPC command requests.
    //
    // This dispatcher is actually a specific command in the first layer of RPC requests.
    zx_status_t HandleRpcCommand(const RpcFunctionExecuteCommandsArgs& args,
                                 RpcFunctionExecuteCommandsResult* out_result);
    zx_status_t HandleRpcCommandLoadTa(LoadTaRpcMessage* message);
    zx_status_t HandleRpcCommandGetTime(GetTimeRpcMessage* message);
    zx_status_t HandleRpcCommandAllocateMemory(AllocateMemoryRpcMessage* message);
    zx_status_t HandleRpcCommandFreeMemory(FreeMemoryRpcMessage* message);

    // Move in the FileSystemRpcMessage since it'll be moved into a sub-type in this function.
    zx_status_t HandleRpcCommandFileSystem(FileSystemRpcMessage&& message);
    zx_status_t HandleRpcCommandFileSystemOpenFile(OpenFileFileSystemRpcMessage* message);
    zx_status_t HandleRpcCommandFileSystemCreateFile(CreateFileFileSystemRpcMessage* message);
    zx_status_t HandleRpcCommandFileSystemCloseFile(CloseFileFileSystemRpcMessage* message);
    zx_status_t HandleRpcCommandFileSystemReadFile(ReadFileFileSystemRpcMessage* message);
    zx_status_t HandleRpcCommandFileSystemWriteFile(WriteFileFileSystemRpcMessage* message);
    zx_status_t HandleRpcCommandFileSystemTruncateFile(TruncateFileFileSystemRpcMessage* message);
    zx_status_t HandleRpcCommandFileSystemRemoveFile(RemoveFileFileSystemRpcMessage* message);
    zx_status_t HandleRpcCommandFileSystemRenameFile(RenameFileFileSystemRpcMessage* message);
    static fuchsia_tee_Device_ops_t kFidlOps;

    OpteeController* controller_;
    bool needs_to_close_ = false;
    SharedMemoryList allocated_shared_memory_;
    std::atomic<uint64_t> next_file_system_object_id_{1};

    std::unordered_map<uint64_t, zx::channel> open_file_system_objects_;
    std::unordered_set<uint32_t> open_sessions_;

    // The client end of a channel to the `fuchsia.tee.manager.ServiceProvider` protocol.
    // This may be an invalid channel, which indicates the client has no service provider support.
    zx::channel service_provider_channel_;

    // A lazily-initialized, cached channel to the root storage channel.
    // This may be an invalid channel, which indicates it has not been initialized yet.
    zx::channel root_storage_channel_;
};

} // namespace optee
