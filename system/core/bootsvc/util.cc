// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util.h"

#include <ctype.h>
#include <fbl/algorithm.h>
#include <fs/connection.h>
#include <safemath/checked_math.h>
#include <string.h>
#include <zircon/assert.h>
#include <zircon/boot/image.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/status.h>

namespace {

// Returns true for boot item types that should be stored.
bool StoreItem(uint32_t type) {
  switch (type) {
    case ZBI_TYPE_CRASHLOG:
    case ZBI_TYPE_KERNEL_DRIVER:
    case ZBI_TYPE_PLATFORM_ID:
    case ZBI_TYPE_STORAGE_BOOTFS_FACTORY:
    case ZBI_TYPE_STORAGE_RAMDISK:
      return true;
    default:
      return ZBI_TYPE_DRV_METADATA(type);
  }
}

// Discards the boot item from the boot image VMO.
void DiscardItem(zx::vmo* vmo, uint32_t begin_in, uint32_t end_in) {
  uint64_t begin = fbl::round_up(begin_in, static_cast<uint32_t>(PAGE_SIZE));
  uint64_t end = fbl::round_down(end_in, static_cast<uint32_t>(PAGE_SIZE));
  if (begin < end) {
    zx_status_t status = vmo->op_range(ZX_VMO_OP_DECOMMIT, begin, end - begin, nullptr, 0);
    ZX_ASSERT_MSG(status == ZX_OK, "Discarding boot item failed: %s\n",
                  zx_status_get_string(status));
    printf("bootsvc: Decommitted BOOTDATA VMO from %#lx to %#lx\n", begin, end);
  }
}

bootsvc::ItemKey CreateItemKey(uint32_t type, uint32_t extra) {
  switch (type) {
    case ZBI_TYPE_STORAGE_RAMDISK:
      // If this is for a ramdisk, set the extra value to zero.
      return bootsvc::ItemKey{.type = type, .extra = 0};
    default:
      // Otherwise, store the extra value.
      return bootsvc::ItemKey{.type = type, .extra = extra};
  }
}

bootsvc::ItemValue CreateItemValue(uint32_t type, uint32_t off, uint32_t len) {
  switch (type) {
    case ZBI_TYPE_STORAGE_RAMDISK:
      // If this is for a ramdisk, capture the ZBI header.
      len = safemath::CheckAdd(len, sizeof(zbi_header_t)).ValueOrDie<uint32_t>();
      break;
    default:
      // Otherwise, adjust the offset to skip the ZBI header.
      off = safemath::CheckAdd(off, sizeof(zbi_header_t)).ValueOrDie<uint32_t>();
  }
  return bootsvc::ItemValue{.offset = off, .length = len};
}

zx_status_t ProcessFactoryItem(const zx::vmo& vmo, uint32_t offset, uint32_t length,
                               bootsvc::FactoryItemValue* out_factory_item) {
  offset = safemath::CheckAdd(offset, sizeof(zbi_header_t)).ValueOrDie<uint32_t>();

  zx::vmo payload;
  zx_status_t status = zx::vmo::create(length, 0, &payload);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to create payload vmo: %s\n", zx_status_get_string(status));
    return status;
  }

  auto buffer = std::make_unique<uint8_t[]>(length);
  status = vmo.read(buffer.get(), offset, length);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to read input vmo: %s\n", zx_status_get_string(status));
    return status;
  }

  status = payload.write(buffer.get(), 0, length);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to write payload vmo: %s\n", zx_status_get_string(status));
    return status;
  }

  // Wipe the factory item from the original VMO.
  memset(buffer.get(), 0, length);
  status = vmo.write(buffer.get(), offset, length);
  if (status != ZX_OK) {
    printf("bootsvc: Failed to wipe input vmo: %s\n", zx_status_get_string(status));
    return status;
  }

  *out_factory_item = bootsvc::FactoryItemValue{.vmo = std::move(payload), .length = length};
  return ZX_OK;
}

}  // namespace

namespace bootsvc {

const char* const kLastPanicFilePath = "log/last-panic.txt";

zx_status_t RetrieveBootImage(zx::vmo* out_vmo, ItemMap* out_map, FactoryItemMap* out_factory_map) {
  // Validate boot image VMO provided by startup handle.
  zx::vmo vmo(zx_take_startup_handle(PA_HND(PA_VMO_BOOTDATA, 0)));
  zbi_header_t header;
  zx_status_t status = vmo.read(&header, 0, sizeof(header));
  if (status != ZX_OK) {
    printf("bootsvc: Failed to read ZBI image header: %s\n", zx_status_get_string(status));
    return status;
  } else if (header.type != ZBI_TYPE_CONTAINER || header.extra != ZBI_CONTAINER_MAGIC ||
             header.magic != ZBI_ITEM_MAGIC || !(header.flags & ZBI_FLAG_VERSION)) {
    printf("bootsvc: Invalid ZBI image header\n");
    return ZX_ERR_IO_DATA_INTEGRITY;
  }

  // Used to discard pages from the boot image VMO.
  uint32_t discard_begin = 0;
  uint32_t discard_end = 0;

  // Read boot items from the boot image VMO.
  ItemMap map;
  FactoryItemMap factory_map;

  uint32_t off = sizeof(header);
  uint32_t len = header.length;
  while (len > sizeof(header)) {
    status = vmo.read(&header, off, sizeof(header));
    if (status != ZX_OK) {
      printf("bootsvc: Failed to read ZBI item header: %s\n", zx_status_get_string(status));
      return status;
    } else if (header.type == ZBI_CONTAINER_MAGIC || header.magic != ZBI_ITEM_MAGIC) {
      printf("bootsvc: Invalid ZBI item header\n");
      return ZX_ERR_IO_DATA_INTEGRITY;
    }
    uint32_t item_len = ZBI_ALIGN(header.length + static_cast<uint32_t>(sizeof(zbi_header_t)));
    uint32_t next_off = safemath::CheckAdd(off, item_len).ValueOrDie();
    if (item_len > len) {
      printf("bootsvc: ZBI item too large (%u > %u)\n", item_len, len);
      return ZX_ERR_IO_DATA_INTEGRITY;
    } else if (StoreItem(header.type)) {
      if (header.type == ZBI_TYPE_STORAGE_BOOTFS_FACTORY) {
        FactoryItemValue factory_item;

        status = ProcessFactoryItem(vmo, off, header.length, &factory_item);
        if (status != ZX_OK) {
          printf("bootsvc: Failed to process factory item: %s\n", zx_status_get_string(status));
          return status;
        }

        factory_map.emplace(header.extra, std::move(factory_item));
      } else {
        map.emplace(CreateItemKey(header.type, header.extra),
                    CreateItemValue(header.type, off, header.length));
      }
      DiscardItem(&vmo, discard_begin, discard_end);
      discard_begin = next_off;
    } else {
      discard_end = next_off;
    }
    off = next_off;
    len = safemath::CheckSub(len, item_len).ValueOrDie();
  }

  DiscardItem(&vmo, discard_begin, discard_end);
  *out_vmo = std::move(vmo);
  *out_map = std::move(map);
  *out_factory_map = std::move(factory_map);
  return ZX_OK;
}

zx_status_t ParseBootArgs(std::string_view str, fbl::Vector<char>* buf) {
  buf->reserve(buf->size() + str.size());
  for (auto it = str.begin(); it != str.end(); it++) {
    // Skip any leading whitespace.
    if (isspace(*it)) {
      continue;
    }
    // Is the line a comment or a zero-length name?
    bool is_comment = *it == '#' || *it == '=';
    // Append line, if it is not a comment.
    for (; it != str.end(); it++) {
      if (*it == '\n') {
        // We've reached the end of the line.
        break;
      } else if (is_comment) {
        // Skip this character, as it is part of a comment.
        continue;
      } else if (isspace(*it)) {
        // It is invalid to have a space within an argument.
        return ZX_ERR_INVALID_ARGS;
      } else {
        buf->push_back(*it);
      }
    }
    if (!is_comment) {
      buf->push_back(0);
    }
  }
  return ZX_OK;
}

zx_status_t CreateVnodeConnection(fs::Vfs* vfs, fbl::RefPtr<fs::Vnode> vnode, zx::channel* out) {
  zx::channel local, remote;
  zx_status_t status = zx::channel::create(0, &local, &remote);
  if (status != ZX_OK) {
    return status;
  }

  auto conn = std::make_unique<fs::Connection>(
      vfs, vnode, std::move(local),
      ZX_FS_FLAG_DIRECTORY | ZX_FS_RIGHT_READABLE | ZX_FS_RIGHT_WRITABLE);
  status = vfs->ServeConnection(std::move(conn));
  if (status != ZX_OK) {
    return status;
  }
  *out = std::move(remote);
  return ZX_OK;
}

fbl::Vector<fbl::String> SplitString(fbl::String input, char delimiter) {
  fbl::Vector<fbl::String> result;

  // No fbl::String::find, do it ourselves.
  const char* start = input.begin();
  for (auto end = start; end != input.end(); start = end + 1) {
    end = start;
    while (end != input.end() && *end != delimiter) {
      ++end;
    }
    result.push_back(fbl::String(start, end - start));
  }
  return result;
}

}  // namespace bootsvc
