// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform-bus.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>
#include <fbl/unique_ptr.h>
#include <fuchsia/boot/c/fidl.h>
#include <fuchsia/sysinfo/c/fidl.h>
#include <zircon/boot/driver-config.h>
#include <zircon/process.h>
#include <zircon/syscalls/iommu.h>

#include "cpu-trace.h"
#include "platform-composite-device.h"

namespace platform_bus {

zx_status_t PlatformBus::IommuGetBti(uint32_t iommu_index, uint32_t bti_id,
                                     zx::bti* out_bti) {
    if (iommu_index != 0) {
        return ZX_ERR_OUT_OF_RANGE;
    }
    return zx::bti::create(iommu_handle_, 0, bti_id, out_bti);
}

zx_status_t PlatformBus::PBusRegisterProtocol(uint32_t proto_id, const void* protocol,
                                              size_t protocol_size) {
    if (!protocol || protocol_size < sizeof(ddk::AnyProtocol)) {
        return ZX_ERR_INVALID_ARGS;
    }

    switch (proto_id) {
    case ZX_PROTOCOL_GPIO_IMPL: {
        gpio_ = ddk::GpioImplProtocolClient(static_cast<const gpio_impl_protocol_t*>(protocol));
        break;
    }
    case ZX_PROTOCOL_CLOCK_IMPL: {
        clk_ = ddk::ClockImplProtocolClient(static_cast<const clock_impl_protocol_t*>(protocol));
        break;
    }
    case ZX_PROTOCOL_POWER_IMPL: {
        power_ = ddk::PowerImplProtocolClient(static_cast<const power_impl_protocol_t*>(protocol));
        break;
    }
    case ZX_PROTOCOL_IOMMU: {
        iommu_ = ddk::IommuProtocolClient(static_cast<const iommu_protocol_t*>(protocol));
        break;
    }
    case ZX_PROTOCOL_SYSMEM: {
        sysmem_ = ddk::SysmemProtocolClient(static_cast<const sysmem_protocol_t*>(protocol));
        break;
    }
    case ZX_PROTOCOL_AMLOGIC_CANVAS: {
        canvas_ = ddk::AmlogicCanvasProtocolClient(
                                        static_cast<const amlogic_canvas_protocol_t*>(protocol));
        break;
    }
    default:
        return ZX_ERR_NOT_SUPPORTED;
    }

    fbl::AutoLock lock(&proto_completion_mutex_);
    sync_completion_signal(&proto_completion_);
    return ZX_OK;
}

zx_status_t PlatformBus::PBusDeviceAdd(const pbus_dev_t* pdev) {
    if (!pdev->name) {
        return ZX_ERR_INVALID_ARGS;
    }

    zx_device_t* parent_dev;
    if (pdev->vid == PDEV_VID_GENERIC && pdev->pid == PDEV_PID_GENERIC &&
        pdev->did == PDEV_DID_KPCI) {
        // Add PCI root at top level.
        parent_dev = parent();
    } else {
        parent_dev = zxdev();
    }

    fbl::unique_ptr<platform_bus::PlatformDevice> dev;
    auto status = PlatformDevice::Create(pdev, parent_dev, this, &dev);
    if (status != ZX_OK) {
        return status;
    }

    status = dev->Start();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the device.
    __UNUSED auto* dummy = dev.release();
    return ZX_OK;
}

zx_status_t PlatformBus::PBusProtocolDeviceAdd(uint32_t proto_id, const pbus_dev_t* pdev) {
    if (!pdev->name) {
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::unique_ptr<platform_bus::ProtocolDevice> dev;
    auto status = ProtocolDevice::Create(pdev, zxdev(), this, &dev);
    if (status != ZX_OK) {
        return status;
    }

    // Protocol devices run in our devhost.
    status = dev->Start();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the device.
    __UNUSED auto* dummy = dev.release();

    // Wait for protocol implementation driver to register its protocol.
    ddk::AnyProtocol dummy_proto;

    proto_completion_mutex_.Acquire();
    while (DdkGetProtocol(proto_id, &dummy_proto) == ZX_ERR_NOT_SUPPORTED) {
        sync_completion_reset(&proto_completion_);
        proto_completion_mutex_.Release();
        zx_status_t status = sync_completion_wait(&proto_completion_, ZX_SEC(10));
        if (status != ZX_OK) {
            zxlogf(ERROR, "%s sync_completion_wait(protocol %08x) failed: %d\n", __FUNCTION__,
                   proto_id, status);
            return status;
        }
        proto_completion_mutex_.Acquire();
    }
    proto_completion_mutex_.Release();
    return ZX_OK;
}

zx_status_t PlatformBus::PBusGetBoardInfo(pdev_board_info_t* out_info) {
    memcpy(out_info, &board_info_, sizeof(board_info_));
    return ZX_OK;
}

zx_status_t PlatformBus::PBusSetBoardInfo(const pbus_board_info_t* info) {
    board_info_.board_revision = info->board_revision;
    return ZX_OK;
}

zx_status_t PlatformBus::PBusRegisterSysSuspendCallback(const pbus_sys_suspend_t* suspend_cbin) {
    suspend_cb_ = *suspend_cbin;
    return ZX_OK;
}

zx_status_t PlatformBus::PBusCompositeDeviceAdd(const pbus_dev_t* pdev,
                                                const device_component_t* components_list,
                                                size_t components_count,
                                                uint32_t coresident_device_index) {
    if (!pdev || !pdev->name) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (coresident_device_index == 0) {
        zxlogf(ERROR, "%s: coresident_device_index cannot be zero\n", __func__);
        return ZX_ERR_INVALID_ARGS;
    }

    fbl::unique_ptr<platform_bus::CompositeDevice> dev;
    auto status = CompositeDevice::Create(pdev, zxdev(), this, &dev);
    if (status != ZX_OK) {
        return status;
    }

    status = dev->Start();
    if (status != ZX_OK) {
        return status;
    }

    // devmgr is now in charge of the device.
    __UNUSED auto* dummy = dev.release();

    device_component_t components[components_count + 1];
    memcpy(&components[1], components_list, components_count * sizeof(components[1]));

    constexpr zx_bind_inst_t root_match[] = {
        BI_MATCH(),
    };
    const zx_bind_inst_t pdev_match[]  = {
        BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_PDEV),
        BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, pdev->vid),
        BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, pdev->pid),
        BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, pdev->did),
    };
    const device_component_part_t pdev_component[] = {
        { countof(root_match), root_match },
        { countof(pdev_match), pdev_match },
    };

    components[0].parts_count = fbl::count_of(pdev_component);
    components[0].parts = pdev_component;

    const zx_device_prop_t props[] = {
        { BIND_PLATFORM_DEV_VID, 0, pdev->vid },
        { BIND_PLATFORM_DEV_PID, 0, pdev->pid },
        { BIND_PLATFORM_DEV_DID, 0, pdev->did },
    };

    return DdkAddComposite(pdev->name, props, fbl::count_of(props), components,
                           components_count + 1, coresident_device_index);
}

zx_status_t PlatformBus::DdkGetProtocol(uint32_t proto_id, void* out) {
    switch (proto_id) {
    case ZX_PROTOCOL_PBUS: {
        auto proto = static_cast<pbus_protocol_t*>(out);
        proto->ctx = this;
        proto->ops = &pbus_protocol_ops_;
        return ZX_OK;
    }
    case ZX_PROTOCOL_GPIO_IMPL:
        if (gpio_) {
            gpio_->GetProto(static_cast<gpio_impl_protocol_t*>(out));
            return ZX_OK;
        }
        break;
    case ZX_PROTOCOL_POWER_IMPL:
        if (power_) {
            power_->GetProto(static_cast<power_impl_protocol_t*>(out));
            return ZX_OK;
        }
        break;
    case ZX_PROTOCOL_CLOCK_IMPL:
        if (clk_) {
            clk_->GetProto(static_cast<clock_impl_protocol_t*>(out));
            return ZX_OK;
        }
        break;
    case ZX_PROTOCOL_SYSMEM:
        if (sysmem_) {
            sysmem_->GetProto(static_cast<sysmem_protocol_t*>(out));
            return ZX_OK;
        }
        break;
    case ZX_PROTOCOL_AMLOGIC_CANVAS:
        if (canvas_) {
            canvas_->GetProto(static_cast<amlogic_canvas_protocol_t*>(out));
            return ZX_OK;
        }
        break;
    case ZX_PROTOCOL_IOMMU:
        if (iommu_) {
            iommu_->GetProto(static_cast<iommu_protocol_t*>(out));
        } else {
            // return default implementation
            auto proto = static_cast<iommu_protocol_t*>(out);
            proto->ctx = this;
            proto->ops = &iommu_protocol_ops_;
            return ZX_OK;
        }
        break;
    }

    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t PlatformBus::GetBootItem(uint32_t type, uint32_t extra, zx::vmo* vmo,
                                     uint32_t* length) {
    return fuchsia_boot_ItemsGet(items_svc_.get(), type, extra, vmo->reset_and_get_address(),
                                 length);
}

zx_status_t PlatformBus::GetBootItem(uint32_t type, uint32_t extra, fbl::Array<uint8_t>* out) {
    zx::vmo vmo;
    uint32_t length;
    zx_status_t status = GetBootItem(type, extra, &vmo, &length);
    if (status != ZX_OK) {
        return status;
    }
    if (vmo.is_valid()) {
        fbl::Array<uint8_t> data(new uint8_t[length], length);
        status = vmo.read(data.get(), 0, data.size());
        if (status != ZX_OK) {
            return status;
        }
        *out = std::move(data);
    }
    return ZX_OK;
}

void PlatformBus::DdkRelease() {
    delete this;
}

typedef struct {
    void* pbus_instance;
} sysdev_suspend_t;

static zx_status_t sys_device_suspend(void* ctx, uint32_t flags) {
    auto* p = reinterpret_cast<sysdev_suspend_t*>(ctx);
    auto* pbus = reinterpret_cast<class PlatformBus*>(p->pbus_instance);

    if (pbus != nullptr) {
        pbus_sys_suspend_t suspend_cb = pbus->suspend_cb();
        if (suspend_cb.callback != nullptr) {
            return suspend_cb.callback(suspend_cb.ctx, flags);
        }
    }
    return ZX_ERR_NOT_SUPPORTED;
}

static void sys_device_release(void* ctx) {
    auto* p = reinterpret_cast<sysdev_suspend_t*>(ctx);
    delete p;
}

// cpu-trace provides access to the cpu's tracing and performance counters.
// As such the "device" is the cpu itself.
static zx_status_t InitCpuTrace(zx_device_t* parent, zx_handle_t dummy_iommu_handle) {
    zx_handle_t cpu_trace_bti;
    zx_status_t status = zx_bti_create(dummy_iommu_handle, 0, CPU_TRACE_BTI_ID, &cpu_trace_bti);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform-bus: error %d in bti_create(cpu_trace_bti)\n", status);
        return status;
    }

    status = publish_cpu_trace(cpu_trace_bti, parent);
    if (status != ZX_OK) {
        // This is not fatal.
        zxlogf(INFO, "publish_cpu_trace returned %d\n", status);
    }
    return status;
}

static zx_protocol_device_t sys_device_proto = []() {
    zx_protocol_device_t result;

    result.version = DEVICE_OPS_VERSION;
    result.suspend = sys_device_suspend;
    result.release = sys_device_release;
    return result;
}();

zx_status_t PlatformBus::Create(zx_device_t* parent, const char* name, zx::channel items_svc) {
    // This creates the "sys" device.
    sys_device_proto.version = DEVICE_OPS_VERSION;

    // The suspend op needs to get access to the PBus instance, to be able to
    // callback the ACPI suspend hook. Introducing a level of indirection here
    // to allow us to update the PBus instance in the device context after creating
    // the device.
    fbl::AllocChecker ac;
    fbl::unique_ptr<uint8_t[]> ptr(new (&ac) uint8_t[sizeof(sysdev_suspend_t)]);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    auto* suspend_buf = reinterpret_cast<sysdev_suspend_t*>(ptr.get());
    suspend_buf->pbus_instance = nullptr;

    device_add_args_t args = {};
    args.version = DEVICE_ADD_ARGS_VERSION;
    args.name = "sys";
    args.ops = &sys_device_proto;
    args.flags = DEVICE_ADD_NON_BINDABLE;
    args.ctx = suspend_buf;

    // Create /dev/sys.
    zx_device_t* sys_root;
    auto status = device_add(parent, &args, &sys_root);
    if (status != ZX_OK) {
        return status;
    } else {
        __UNUSED auto* dummy = ptr.release();
    }

    // Add child of sys for the board driver to bind to.
    fbl::unique_ptr<platform_bus::PlatformBus> bus(
        new (&ac) platform_bus::PlatformBus(sys_root, std::move(items_svc)));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    suspend_buf->pbus_instance = bus.get();

    status = bus->Init();
    if (status != ZX_OK) {
        return status;
    }

    // Create /dev/sys/cpu-trace.
    // But only do so if we have an iommu handle. Normally we do, but tests
    // may create us without a root resource, and thus without the iommu
    // handle.
    if (bus->iommu_handle_.is_valid()) {
        status = InitCpuTrace(sys_root, bus->iommu_handle_.get());
        if (status != ZX_OK) {
            // This is not fatal. Error message already printed.
        }
    }

    // devmgr is now in charge of the device.
    __UNUSED auto* dummy = bus.release();
    return ZX_OK;
}

PlatformBus::PlatformBus(zx_device_t* parent, zx::channel items_svc)
    : PlatformBusType(parent), items_svc_(std::move(items_svc)) {
    sync_completion_reset(&proto_completion_);
}

zx_status_t PlatformBus::Init() {
    zx_status_t status;
    // Set up a dummy IOMMU protocol to use in the case where our board driver
    // does not set a real one.
    zx_iommu_desc_dummy_t desc;
    // Please do not use get_root_resource() in new code. See ZX-1467.
    zx::unowned_resource root_resource(get_root_resource());
    if (root_resource->is_valid()) {
        status = zx::iommu::create(*root_resource, ZX_IOMMU_TYPE_DUMMY, &desc, sizeof(desc),
                                   &iommu_handle_);
        if (status != ZX_OK) {
            return status;
        }
    }

    // Read kernel driver.
    zx::vmo vmo;
    uint32_t length;
    uint8_t interrupt_controller_type = fuchsia_sysinfo_InterruptControllerType_UNKNOWN;
#if __x86_64__
    interrupt_controller_type = fuchsia_sysinfo_InterruptControllerType_APIC;
#else
    status = GetBootItem(ZBI_TYPE_KERNEL_DRIVER, KDRV_ARM_GIC_V2, &vmo, &length);
    if (status != ZX_OK) {
        return status;
    }
    if (vmo.is_valid()) {
        interrupt_controller_type = fuchsia_sysinfo_InterruptControllerType_GIC_V2;
    }
    status = GetBootItem(ZBI_TYPE_KERNEL_DRIVER, KDRV_ARM_GIC_V3, &vmo, &length);
    if (status != ZX_OK) {
        return status;
    }
    if (vmo.is_valid()) {
        interrupt_controller_type = fuchsia_sysinfo_InterruptControllerType_GIC_V3;
    }
#endif
    // Publish interrupt controller type to sysinfo driver
    status = device_publish_metadata(
        parent(), "/dev/misc/sysinfo", DEVICE_METADATA_INTERRUPT_CONTROLLER_TYPE,
        &interrupt_controller_type, sizeof(interrupt_controller_type));
    if (status != ZX_OK) {
        zxlogf(ERROR, "device_publish_metadata(interrupt_controller_type) failed: %d\n", status);
        return status;
    }

    // Read platform ID.
    status = GetBootItem(ZBI_TYPE_PLATFORM_ID, 0, &vmo, &length);
    if (status != ZX_OK) {
        return status;
    }
    if (vmo.is_valid()) {
        zbi_platform_id_t platform_id;
        status = vmo.read(&platform_id, 0, sizeof(platform_id));
        if (status != ZX_OK) {
            return status;
        }
        zxlogf(INFO, "platform bus: VID: %u PID: %u board: \"%s\"\n", platform_id.vid,
               platform_id.pid, platform_id.board_name);
        board_info_.vid = platform_id.vid;
        board_info_.pid = platform_id.pid;
        memcpy(board_info_.board_name, platform_id.board_name, sizeof(board_info_.board_name));
        // Publish board name to sysinfo driver
        status = device_publish_metadata(parent(), "/dev/misc/sysinfo", DEVICE_METADATA_BOARD_NAME,
                                         platform_id.board_name, sizeof(platform_id.board_name));
        if (status != ZX_OK) {
            zxlogf(ERROR, "device_publish_metadata(board_name) failed: %d\n", status);
            return status;
        }
    } else {
#if __x86_64__
        // For x86_64, we might not find the ZBI_TYPE_PLATFORM_ID, old bootloaders
        // won't support this, for example. If this is the case, cons up the VID/PID here
        // to allow the acpi board driver to load and bind.
        board_info_.vid = PDEV_VID_INTEL;
        board_info_.pid = PDEV_PID_X86;
        strncpy(board_info_.board_name, "x86_64", sizeof(board_info_.board_name));
        // Publish board name to sysinfo driver
        status = device_publish_metadata(parent(), "/dev/misc/sysinfo", DEVICE_METADATA_BOARD_NAME,
                                         "x86_64", sizeof("x86_64"));
        if (status != ZX_OK) {
            zxlogf(ERROR, "device_publish_metadata(board_name) failed: %d\n", status);
            return status;
        }
#else
        zxlogf(ERROR, "platform_bus: ZBI_TYPE_PLATFORM_ID not found\n");
        return ZX_ERR_INTERNAL;
#endif
    }
    // This is optionally set later by the board driver.
    board_info_.board_revision = 0;

    // Then we attach the platform-bus device below it.
    zx_device_prop_t props[] = {
        {BIND_PLATFORM_DEV_VID, 0, board_info_.vid},
        {BIND_PLATFORM_DEV_PID, 0, board_info_.pid},
    };
    status = DdkAdd("platform", DEVICE_ADD_INVISIBLE, props, fbl::count_of(props));
    if (status != ZX_OK) {
        return status;
    }
    fbl::Array<uint8_t> board_data;
    status = GetBootItem(ZBI_TYPE_DRV_BOARD_PRIVATE, 0, &board_data);
    if (status != ZX_OK) {
        return status;
    }
    if (board_data) {
        status = DdkAddMetadata(DEVICE_METADATA_BOARD_PRIVATE, board_data.get(), board_data.size());
        if (status != ZX_OK) {
            return status;
        }
    }
    DdkMakeVisible();
    return ZX_OK;
}

zx_status_t platform_bus_create(void* ctx, zx_device_t* parent, const char* name,
                                const char* args, zx_handle_t handle) {
    return platform_bus::PlatformBus::Create(parent, name, zx::channel(handle));
}

static constexpr zx_driver_ops_t driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.create = platform_bus_create;
    return ops;
}();

} // namespace platform_bus

ZIRCON_DRIVER_BEGIN(platform_bus, platform_bus::driver_ops, "zircon", "0.1", 1)
    // devmgr loads us directly, so we need no binding information here
    BI_ABORT_IF_AUTOBIND,
ZIRCON_DRIVER_END(platform_bus)
