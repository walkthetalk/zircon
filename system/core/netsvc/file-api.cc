// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "file-api.h"

#include <errno.h>
#include <fcntl.h>

#include <fbl/unique_fd.h>
#include <lib/fdio/fdio.h>
#include <zircon/boot/netboot.h>

#include "board-name.h"
#include "netboot.h"

namespace netsvc {
namespace {

size_t NB_IMAGE_PREFIX_LEN() {
    return strlen(NB_IMAGE_PREFIX);
}
size_t NB_FILENAME_PREFIX_LEN() {
    return strlen(NB_FILENAME_PREFIX);
}

} // namespace

FileApi::FileApi(bool is_zedboot, std::unique_ptr<NetCopyInterface> netcp, zx::channel sysinfo,
                 PaverInterface* paver)
    : is_zedboot_(is_zedboot), sysinfo_(std::move(sysinfo)), netcp_(std::move(netcp)),
      paver_(paver) {
    ZX_ASSERT(paver_ != nullptr);

    if (!sysinfo_) {
        constexpr char kSysInfoPath[] = "/dev/misc/sysinfo";
        fbl::unique_fd sysinfo_fd(open(kSysInfoPath, O_RDWR));
        if (sysinfo_fd) {
            fdio_get_service_handle(sysinfo_fd.release(), sysinfo_.reset_and_get_address());
        }
    }
}

ssize_t FileApi::OpenRead(const char* filename) {
    // Make sure all in-progress paving operations have completed
    if (paver_->InProgress() == true) {
        return TFTP_ERR_SHOULD_WAIT;
    }
    if (paver_->exit_code() != ZX_OK) {
        fprintf(stderr, "paver exited with error: %d\n", paver_->exit_code());
        paver_->reset_exit_code();
        return TFTP_ERR_IO;
    }

    is_write_ = false;
    strncpy(filename_, filename, PATH_MAX);
    filename_[PATH_MAX] = '\0';
    netboot_file_ = NULL;
    size_t file_size;
    if (netcp_->Open(filename, O_RDONLY, &file_size) == 0) {
        return static_cast<ssize_t>(file_size);
    }
    return TFTP_ERR_NOT_FOUND;
}

tftp_status FileApi::OpenWrite(const char* filename, size_t size) {
    // Make sure all in-progress paving operations have completed
    if (paver_->InProgress() == true) {
        return TFTP_ERR_SHOULD_WAIT;
    }
    if (paver_->exit_code() != ZX_OK) {
        fprintf(stderr, "paver exited with error: %d\n", paver_->exit_code());
        paver_->reset_exit_code();
        return TFTP_ERR_IO;
    }

    is_write_ = true;
    strncpy(filename_, filename, PATH_MAX);
    filename_[PATH_MAX] = '\0';

    if (is_zedboot_ && !strncmp(filename_, NB_FILENAME_PREFIX, NB_FILENAME_PREFIX_LEN())) {
        type_ = NetfileType::kNetboot;
        netboot_file_ = netboot_get_buffer(filename_, size);
        if (netboot_file_ != NULL) {
            return TFTP_NO_ERROR;
        }
    } else if (is_zedboot_ && !strcmp(filename_, NB_BOARD_NAME_FILENAME)) {
        printf("netsvc: Running board name validation\n");
        type_ = NetfileType::kBoardName;
        return TFTP_NO_ERROR;
    } else if (is_zedboot_ && !strncmp(filename_, NB_IMAGE_PREFIX, NB_IMAGE_PREFIX_LEN())) {
        type_ = NetfileType::kPaver;
        tftp_status status = paver_->OpenWrite(filename_, size);
        if (status != TFTP_NO_ERROR) {
            filename_[0] = '\0';
        }
        return status;
    } else {
        type_ = NetfileType::kNetCopy;
        if (netcp_->Open(filename_, O_WRONLY, NULL) == 0) {
            return TFTP_NO_ERROR;
        }
    }
    return TFTP_ERR_INVALID_ARGS;
}

tftp_status FileApi::Read(void* data, size_t* length, off_t offset) {
    if (length == NULL) {
        return TFTP_ERR_INVALID_ARGS;
    }
    ssize_t read_len = netcp_->Read(data, offset, *length);
    if (read_len < 0) {
        return TFTP_ERR_IO;
    }
    *length = static_cast<size_t>(read_len);
    return TFTP_NO_ERROR;
}

tftp_status FileApi::Write(const void* data, size_t* length, off_t offset) {
    if (length == NULL) {
        return TFTP_ERR_INVALID_ARGS;
    }
    switch (type_) {
    case NetfileType::kNetboot: {
        nbfile* nb_file = netboot_file_;
        if ((static_cast<size_t>(offset) > nb_file->size) || (offset + *length) > nb_file->size) {
            return TFTP_ERR_INVALID_ARGS;
        }
        memcpy(nb_file->data + offset, data, *length);
        nb_file->offset = offset + *length;
        return TFTP_NO_ERROR;
    }
    case NetfileType::kPaver:
        return paver_->Write(data, length, offset);

    case NetfileType::kBoardName: {
        tftp_status status = CheckBoardName(sysinfo_, reinterpret_cast<const char*>(data), *length)
                                 ? TFTP_NO_ERROR
                                 : TFTP_ERR_BAD_STATE;
        if (status == TFTP_NO_ERROR) {
            printf("netsvc: Board name validation passed\n");
        } else {
            printf("netsvc: Board name validation failed\n");
        }
        return status;
    }
    case NetfileType::kNetCopy: {
        ssize_t write_result =
            netcp_->Write(reinterpret_cast<const char*>(data), offset, *length);
        if (static_cast<size_t>(write_result) == *length) {
            return TFTP_NO_ERROR;
        }
        if (write_result == -EBADF) {
            return TFTP_ERR_BAD_STATE;
        }
        return TFTP_ERR_IO;
    }
    default:
        return ZX_ERR_BAD_STATE;
    }

    return TFTP_ERR_BAD_STATE;
}

void FileApi::Close() {
    if (type_ == NetfileType::kNetCopy) {
        netcp_->Close();
    } else if (type_ == NetfileType::kPaver) {
        paver_->Close();
    }
    type_ = NetfileType::kUnknown;
}

void FileApi::Abort() {
    if (is_write_ && type_ == NetfileType::kNetCopy) {
        netcp_->AbortWrite();
    }
    Close();
}

} // namespace netsvc
