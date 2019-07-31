// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fuchsia/hardware/block/c/fidl.h>
#include <gpt/cros.h>
#include <gpt/gpt.h>
#include <gpt/guid.h>
#include <lib/fzl/fdio.h>
#include <zircon/device/block.h>
#include <zircon/status.h>
#include <zircon/syscalls.h> // for zx_cprng_draw

using gpt::GptDevice;
using gpt::KnownGuid;

namespace {

const char* bin_name;
bool confirm_writes = true;

zx_status_t ReadPartitionIndex(const char* arg, uint32_t* idx) {
    char* end;
    unsigned long lidx = strtoul(arg, &end, 10);
    if (*end != 0 || lidx > UINT32_MAX || lidx >= gpt::kPartitionCount) {
        return ZX_ERR_INVALID_ARGS;
    }

    *idx = static_cast<uint32_t>(lidx);
    return ZX_OK;
}

int status_to_retcode(zx_status_t ret) {
    return ret == ZX_OK ? 0 : 1;
}

int Usage(zx_status_t ret) {
    printf("Usage:\n");
    printf("Note that for all these commands, [<dev>] is the device containing the GPT.\n");
    printf("Although using a GPT will split your device into small partitions, [<dev>] \n");
    printf("should always refer to the containing device, NOT block devices representing\n");
    printf("the partitions themselves.\n\n");
    printf("> %s dump [<dev>]\n", bin_name);
    printf("  View the properties of the selected device\n");
    printf("> %s Init [<dev>]\n", bin_name);
    printf("  Initialize the block device with a GPT\n");
    printf("> %s repartition <dev> [[<label> <type> <size>], ...]\n", bin_name);
    printf("  Destructively repartition the device with the given layout\n");
    printf("    e.g.\n");
    printf("    %s repartition /dev/class/block-core/000", bin_name);
    printf(" esp efi-system 100m sys system 5g blob fuchsia-blob 50%% data cros-data 50%%\n");
    printf("> %s add <start block> <end block> <name> [<dev>]\n", bin_name);
    printf("  Add a partition to the device (and create a GPT if one does not exist)\n");
    printf("  Range of blocks is INCLUSIVE (both start and end). Full device range\n");
    printf("  may be queried using '%s dump'\n", bin_name);
    printf("> %s edit <n> <type type_guid>|<id id_guid> [<dev>]\n", bin_name);
    printf("  Edit the GUID of the nth partition on the device\n");
    printf("> %s edit_cros <n> [-T <tries>] [-S <successful>] [-P <priority] <dev>\n", bin_name);
    printf("  Edit the GUID of the nth partition on the device\n");
    printf("> %s adjust <n> <start block> <end block> [<dev>]\n", bin_name);
    printf("  Move or resize the nth partition on the device\n");
    printf("> %s remove <n> [<dev>]\n", bin_name);
    printf("  Remove the nth partition from the device\n");
    printf("> %s visible <n> true|false [<dev>]\n", bin_name);
    printf("  Set the visibility of the nth partition on the device\n");
    printf("\n");
    printf("Known partition types are:\n");
    for (KnownGuid::const_iterator i = KnownGuid::begin(); i != KnownGuid::end(); i++) {
        printf("        %s\n", i->name());
    }
    printf("The option --live-dangerously may be passed in front of any command\n");
    printf("to skip the write confirmation prompt.\n");

    return status_to_retcode(ret);
}

int CGetC(void) {
    uint8_t ch;
    for (;;) {
        ssize_t r = read(0, &ch, 1);
        if (r < 0)
            return static_cast<int>(r);
        if (r == 1)
            return ch;
    }
}

char* CrosFlagsToCString(char* dst, size_t dst_len, uint64_t flags) {
    uint32_t priority = gpt_cros_attr_get_priority(flags);
    uint32_t tries = gpt_cros_attr_get_tries(flags);
    bool successful = gpt_cros_attr_get_successful(flags);
    snprintf(dst, dst_len, "priority=%u tries=%u successful=%u", priority, tries, successful);
    dst[dst_len - 1] = 0;
    return dst;
}

char* FlagsToCString(char* dst, size_t dst_len, const uint8_t* guid, uint64_t flags) {
    if (gpt_cros_is_kernel_guid(guid, sizeof(gpt::guid_t))) {
        return CrosFlagsToCString(dst, dst_len, flags);
    } else {
        snprintf(dst, dst_len, "0x%016" PRIx64, flags);
    }
    dst[dst_len - 1] = 0;
    return dst;
}

fbl::unique_ptr<GptDevice> Init(const char* dev) {
    fbl::unique_fd fd(open(dev, O_RDWR));
    if (!fd.is_valid()) {
        fprintf(stderr, "error opening %s\n", dev);
        return nullptr;
    }

    fuchsia_hardware_block_BlockInfo info;
    fzl::UnownedFdioCaller disk_caller(fd.get());
    zx_status_t status;
    zx_status_t io_status = fuchsia_hardware_block_BlockGetInfo(disk_caller.borrow_channel(),
                                                                &status, &info);
    if (io_status != ZX_OK || status != ZX_OK) {
        fprintf(stderr, "gpt: error getting block info\n");
        return nullptr;
    }

    printf("blocksize=0x%X blocks=%" PRIu64 "\n", info.block_size, info.block_count);

    fbl::unique_ptr<GptDevice> gpt;
    status = GptDevice::Create(fd.get(), info.block_size, info.block_count, &gpt);
    if (status != ZX_OK) {
        fprintf(stderr, "error initializing GPT\n");
        return nullptr;
    }

    return gpt;
}

constexpr void SetXY(unsigned yes, const char** X, const char** Y) {
    if (yes) {
        *X = "\033[7m";
        *Y = "\033[0m";
    } else {
        *X = "";
        *Y = "";
    }
}

void Dump(const GptDevice* gpt, int* count) {
    if (!gpt->Valid()) {
        return;
    }
    const gpt_partition_t* p;
    char name[gpt::kGuidStrLength];
    char guid[gpt::kGuidStrLength];
    char id[gpt::kGuidStrLength];
    char flags_str[256];
    const char* X;
    const char* Y;
    uint32_t i;
    for (i = 0; i < gpt::kPartitionCount; i++) {
        p = gpt->GetPartition(i);
        if (p == nullptr)
            break;
        memset(name, 0, gpt::kGuidStrLength);
        unsigned diff;
        ZX_ASSERT(gpt->GetDiffs(i, &diff) == ZX_OK);
        SetXY(diff & gpt::kGptDiffName, &X, &Y);
        printf("Partition %d: %s%s%s\n", i, X,
               utf16_to_cstring(name, (const uint16_t*)p->name, gpt::kGuidStrLength - 1), Y);
        SetXY(diff & (gpt::kGptDiffFirst | gpt::kGptDiffLast), &X, &Y);
        printf("    Start: %s%" PRIu64 "%s, End: %s%" PRIu64 "%s (%" PRIu64 " blocks)\n", X,
               p->first, Y, X, p->last, Y, p->last - p->first + 1);
        SetXY(diff & gpt::kGptDiffGuid, &X, &Y);
        uint8_to_guid_string(guid, (const uint8_t*)p->guid);
        printf("    id:   %s%s%s\n", X, guid, Y);
        SetXY(diff & gpt::kGptDiffType, &X, &Y);
        uint8_to_guid_string(id, (const uint8_t*)p->type);
        printf("    type: %s%s%s\n", X, id, Y);
        SetXY(diff & gpt::kGptDiffName, &X, &Y);
        printf("    flags: %s%s%s\n", X,
               FlagsToCString(flags_str, sizeof(flags_str), p->type, p->flags), Y);
    }
    if (count) {
        *count = i;
    }
}

void DumpPartitions(const char* dev) {
    fbl::unique_ptr<GptDevice> gpt = Init(dev);
    if (!gpt)
        return;

    if (!gpt->Valid()) {
        fprintf(stderr, "No valid GPT found\n");
        return;
    }

    printf("Partition table is valid\n");

    uint64_t start, end;
    if (gpt->Range(&start, &end) != ZX_OK) {
        fprintf(stderr, "Couldn't identify device range\n");
        return;
    }

    printf("GPT contains usable blocks from %" PRIu64 " to %" PRIu64 " (inclusive)\n", start, end);
    int count;
    Dump(gpt.get(), &count);
    printf("Total: %d partitions\n", count);
}

bool ConfirmCommit(const GptDevice* gpt, const char* dev) {
    if (confirm_writes) {
        Dump(gpt, NULL);
        printf("\n");
        printf("WARNING: About to write partition table to: %s\n", dev);
        printf("WARNING: Type 'y' to continue, 'n' or ESC to cancel\n");

        for (;;) {
            switch (CGetC()) {
            case 'y':
            case 'Y':
                return true;
            case 'n':
            case 'N':
            case 27:
                return false;
            }
        }
    }

    return true;
}

zx_status_t Commit(GptDevice* gpt, const char* dev) {

    if (!ConfirmCommit(gpt, dev)) {
        return ZX_OK;
    }

    zx_status_t rc = gpt->Sync();
    if (rc != ZX_OK) {
        fprintf(stderr, "Error: GPT device sync failed.\n");
        return rc;
    }
    if ((rc = gpt->BlockRrPart()) != ZX_OK) {
        fprintf(stderr, "Error: GPT updated but device could not be rebound. Please reboot.\n");
        return rc;
    }
    printf("GPT changes complete.\n");
    return ZX_OK;
}

zx_status_t InitGpt(const char* dev) {
    fbl::unique_ptr<GptDevice> gpt = Init(dev);
    if (!gpt) {
        return ZX_ERR_INTERNAL;
    }

    zx_status_t status;
    // generate a default header
    if ((status = gpt->RemoveAllPartitions()) != ZX_OK) {
        fprintf(stderr, "Failed to remove partitions: %s\n", zx_status_get_string(status));
        return status;
    }
    return Commit(gpt.get(), dev);
}

zx_status_t AddPartition(const char* dev, uint64_t start, uint64_t end, const char* name) {
    uint8_t guid[GPT_GUID_LEN];
    zx_cprng_draw(guid, GPT_GUID_LEN);

    fbl::unique_ptr<GptDevice> gpt = Init(dev);
    if (!gpt) {
        return ZX_ERR_INTERNAL;
    }

    if (!gpt->Valid()) {
        // generate a default header
        if (Commit(gpt.get(), dev)) {
            return ZX_ERR_INTERNAL;
        }
    }

    uint8_t type[GPT_GUID_LEN];
    memset(type, 0xff, GPT_GUID_LEN);
    zx_status_t rc = gpt->AddPartition(name, type, guid, start, end - start + 1, 0);
    if (rc != ZX_OK) {
        fprintf(stderr, "Add partition failed: %s\n", zx_status_get_string(rc));
        return rc;
    }
    printf("add partition: name=%s start=%" PRIu64 " end=%" PRIu64 "\n", name, start, end);
    return Commit(gpt.get(), dev);
}

/*
 * Converts a GUID of the format xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx to
 * a properly arranged, 16 byte sequence. This takes care of flipping the byte
 * order section-wise for the first three sections (8 bytes total) of the GUID.
 * bytes_out should be a 16 byte array where the final output will be placed.
 * A bool is returned representing success of parsing the GUID. false will be
 * returned if the GUID string is the wrong length or contains invalid
 * characters.
 */
bool ParseGuid(const char* guid, uint8_t* bytes_out) {
    if (strlen(guid) != gpt::kGuidStrLength - 1) {
        fprintf(stderr, "GUID length is wrong: %zd but expected %" PRIu64 "\n", strlen(guid),
                (gpt::kGuidStrLength - 1));
        return false;
    }

    // how many nibbles of the byte we've processed
    uint8_t nibbles = 0;
    // value to accumulate byte as we parse its two char nibbles
    uint8_t val = 0;
    // which byte we're parsing
    uint8_t out_idx = 0;
    uint8_t dashes = 0;

    for (uint64_t idx = 0; idx < gpt::kGuidStrLength - 1; idx++) {
        char c = guid[idx];

        uint8_t char_val = 0;
        if (c == '-') {
            dashes++;
            continue;
        } else if (c >= '0' && c <= '9') {
            char_val = static_cast<uint8_t>(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            char_val = static_cast<uint8_t>(c - (uint8_t)'A' + (uint8_t)10);
        } else if (c >= 'a' && c <= 'f') {
            char_val = static_cast<uint8_t>(c - 'a' + 10);
        } else {
            fprintf(stderr, "'%c' is not a valid GUID character\n", c);
            return false;
        }

        val = static_cast<uint8_t>(val + (char_val << (4 * (1 - nibbles))));

        if (++nibbles == 2) {
            bytes_out[out_idx++] = val;
            nibbles = 0;
            val = 0;
        }
    }

    if (dashes != 4) {
        fprintf(stderr, "Error, incorrect number of hex characters.\n");
        return false;
    }

    // Shuffle bytes because endianness is swapped for certain sections
    uint8_t swap;
    swap = bytes_out[0];
    bytes_out[0] = bytes_out[3];
    bytes_out[3] = swap;
    swap = bytes_out[1];
    bytes_out[1] = bytes_out[2];
    bytes_out[2] = swap;

    swap = bytes_out[4];
    bytes_out[4] = bytes_out[5];
    bytes_out[5] = swap;

    swap = bytes_out[6];
    bytes_out[6] = bytes_out[7];
    bytes_out[7] = swap;

    return true;
}

zx_status_t RemovePartition(const char* dev, uint32_t n) {
    fbl::unique_ptr<GptDevice> gpt = Init(dev);
    if (!gpt) {
        return ZX_ERR_INTERNAL;
    }

    gpt_partition_t* p = gpt->GetPartition(n);
    if (p == nullptr) {
        fprintf(stderr, "Failed to get partition at index %u\n", n);
        return ZX_ERR_INVALID_ARGS;
    }

    zx_status_t status;
    if ((status = gpt->RemovePartition(p->guid)) != ZX_OK) {
        fprintf(stderr, "Failed to remove partiton: %s\n", zx_status_get_string(status));
        return status;
    }
    char name[gpt::kGuidStrLength];
    printf("remove partition: n=%u name=%s\n", n,
           utf16_to_cstring(name, (const uint16_t*)p->name, gpt::kGuidStrLength - 1));
    return Commit(gpt.get(), dev);
}

zx_status_t AdjustPartition(const char* dev, uint32_t idx_part, uint64_t start, uint64_t end) {
    zx_status_t rc;

    fbl::unique_ptr<GptDevice> gpt = Init(dev);
    if (!gpt) {
        return ZX_ERR_INTERNAL;
    }

    if ((rc = gpt->SetPartitionRange(idx_part, start, end)) != ZX_OK) {
        if (rc == ZX_ERR_INVALID_ARGS) {
            fprintf(stderr, "partition #%u would be outside of valid block range\n", idx_part);
        } else if (rc == ZX_ERR_OUT_OF_RANGE) {
            fprintf(stderr, "New partition range overlaps existing partition(s)\n");
        } else {
            fprintf(stderr, "Edit parition failed: %s\n", zx_status_get_string(rc));
        }
        return rc;
    }

    return Commit(gpt.get(), dev);
}

/*
 * Edit a partition, changing either its type or ID GUID. path_device should be
 * the path to the device where the GPT can be read. idx_part should be the
 * index of the partition in the GPT that you want to change. guid should be the
 * string/human-readable form of the GUID and should be 36 characters plus a
 * null terminator.
 */
zx_status_t EditPartition(const char* dev, uint32_t idx_part, char* type_or_id, char* guid_name) {
    zx_status_t rc;

    fbl::unique_ptr<GptDevice> gpt = Init(dev);
    if (!gpt) {
        return ZX_ERR_INTERNAL;
    }

    uint8_t guid_bytes[GPT_GUID_LEN];
    if (!KnownGuid::NameToGuid(guid_name, guid_bytes) && !ParseGuid(guid_name, guid_bytes)) {
        fprintf(stderr, "GUID could not be parsed.\n");
        return ZX_ERR_INVALID_ARGS;
    }

    if (!strcmp(type_or_id, "type")) {
        rc = gpt->SetPartitionType(idx_part, guid_bytes);
    } else if (!strcmp(type_or_id, "id")) {
        rc = gpt->SetPartitionGuid(idx_part, guid_bytes);
    } else {
        fprintf(stderr, "Invalid arguments to edit partition");
        return Usage(ZX_ERR_INVALID_ARGS);
    }

    if (rc != ZX_OK) {
        fprintf(stderr, "Edit parition failed: %s\n", zx_status_get_string(rc));
        return rc;
    }

    return Commit(gpt.get(), dev);
}

struct cros_partition_args_t {
    const char* dev;
    uint32_t idx_part;
    std::optional<long> tries;
    std::optional<long> priority;
    std::optional<long> successful;
};

// Parses arguments for EditCrosPartition. Returns ZX_OK on successfully parsing
// all required arguments. Fields of unpassed optional arguments are left
// unchanged.
int GetCrosPartitionArgs(char* const* argv, int argc, cros_partition_args_t* out_args) {
    uint32_t idx_part;
    if (ReadPartitionIndex(argv[0], &idx_part) != ZX_OK) {
        return Usage(ZX_ERR_INVALID_ARGS);
    }

    char* end;

    int c;
    while ((c = getopt(argc, argv, "T:P:S:")) > 0) {
        switch (c) {
        case 'T': {
            long val = strtol(optarg, &end, 10);
            if (*end != 0 || optarg[0] == 0) {
                return Usage(ZX_ERR_INVALID_ARGS);
            }
            if (val < 0 || val > 15) {
                fprintf(stderr, "tries must be in the range [0, 16)\n");
                return Usage(ZX_ERR_INVALID_ARGS);
            }
            out_args->tries = val;
            break;
        }
        case 'P': {
            long val = strtol(optarg, &end, 10);
            if (*end != 0 || optarg[0] == 0) {
                return Usage(ZX_ERR_INVALID_ARGS);
            }
            if (val < 0 || val > 15) {
                fprintf(stderr, "priority must be in the range [0, 16)\n");
                return Usage(ZX_ERR_INVALID_ARGS);
            }
            out_args->priority = val;
            break;
        }
        case 'S': {
            if (!strncmp(optarg, "0", 2)) {
                out_args->successful = 0;
            } else if (!strncmp(optarg, "1", 2)) {
                out_args->successful = 1;
            } else {
                fprintf(stderr, "successful must be 0 or 1\n");
                return Usage(ZX_ERR_INVALID_ARGS);
            }
            break;
        }
        default:
            fprintf(stderr, "Unknown option\n");
            return Usage(ZX_ERR_INVALID_ARGS);
        }
    }

    if (optind != argc - 1) {
        fprintf(stderr, "Did not specify device arg\n");
        return Usage(ZX_ERR_INVALID_ARGS);
    }

    out_args->idx_part = idx_part;
    out_args->dev = argv[optind];
    return ZX_OK;
}

// Edit a Chrome OS kernel partition, changing its attributes.
// argv/argc should correspond only to the arguments after the command.
zx_status_t EditCrosPartition(char* const* argv, int argc) {
    gpt_partition_t* part = NULL;
    int rc;
    zx_status_t ret;

    cros_partition_args_t args = {};

    if ((ret = GetCrosPartitionArgs(argv, argc, &args)) != 0) {
        return ret;
    }

    fbl::unique_ptr<GptDevice> gpt = Init(args.dev);
    if (!gpt) {
        return ZX_ERR_INTERNAL;
    }

    if ((part = gpt->GetPartition(args.idx_part)) == nullptr) {
        fprintf(stderr, "Partition not found at given index\n");
        return ZX_ERR_INVALID_ARGS;
    }

    if (!gpt_cros_is_kernel_guid(part->type, GPT_GUID_LEN)) {
        fprintf(stderr, "Partition is not a CrOS kernel partition\n");
        return ZX_ERR_INVALID_ARGS;
    }

    uint64_t flags;

    rc = gpt->GetPartitionFlags(args.idx_part, &flags);

    if (rc != ZX_OK) {
        fprintf(stderr, "Failed to get partition flags: %s\n", zx_status_get_string(rc));
        return rc;
    }

    if (args.tries) {
        if (gpt_cros_attr_set_tries(&flags, static_cast<uint8_t>(*args.tries)) < 0) {
            fprintf(stderr, "Failed to set tries\n");
            return ZX_ERR_INVALID_ARGS;
        }
    }
    if (args.priority) {
        if (gpt_cros_attr_set_priority(&flags, static_cast<uint8_t>(*args.priority)) < 0) {
            fprintf(stderr, "Failed to set priority\n");
            return ZX_ERR_INVALID_ARGS;
        }
    }
    if (args.successful) {
        gpt_cros_attr_set_successful(&flags, *args.successful);
    }

    rc = gpt->SetPartitionFlags(args.idx_part, flags);

    if (rc != ZX_OK) {
        fprintf(stderr, "Failed to set partition flags: %s\n", zx_status_get_string(rc));
        return rc;
    }
    return Commit(gpt.get(), args.dev);
}

/*
 * Set whether a partition is visible or not to the EFI firmware. If a
 * partition is set as hidden, the firmware will not attempt to boot from the
 * partition.
 */
zx_status_t SetVisibility(char* dev, uint32_t idx_part, bool visible) {
    fbl::unique_ptr<GptDevice> gpt = Init(dev);
    if (!gpt) {
        return ZX_ERR_INTERNAL;
    }

    zx_status_t rc;
    rc = gpt->SetPartitionVisibility(idx_part, visible);
    if (rc != ZX_OK) {
        fprintf(stderr, "Partition visibility edit failed: %s\n", zx_status_get_string(rc));
        return rc;
    }

    return Commit(gpt.get(), dev);
}

// ParseSize parses long integers in base 10, expanding p, t, g, m, and k
// suffices as binary byte scales. If the suffix is %, the value is returned as
// negative, in order to indicate a proportion.
int64_t ParseSize(char* s) {
    char* end = s;
    long long int v = strtoll(s, &end, 10);

    switch (*end) {
    case 0:
        break;
    case '%':
        v = -v;
        break;
    case 'p':
    case 'P':
        v *= 1024;
        __FALLTHROUGH;
    case 't':
    case 'T':
        v *= 1024;
        __FALLTHROUGH;
    case 'g':
    case 'G':
        v *= 1024;
        __FALLTHROUGH;
    case 'm':
    case 'M':
        v *= 1024;
        __FALLTHROUGH;
    case 'k':
    case 'K':
        v *= 1024;
    }
    return v;
}

// TODO(raggi): this should eventually get moved into ulib/gpt.
// Align finds the next block at or after base that is aligned to a physical
// block boundary. The gpt specification requires that all partitions are
// aligned to physical block boundaries.
uint64_t Align(uint64_t base, uint64_t logical, uint64_t physical) {
    uint64_t a = logical;
    if (physical > a)
        a = physical;
    uint64_t base_bytes = base * logical;
    uint64_t d = base_bytes % a;
    return (base_bytes + a - d) / logical;
}

// Repartition expects argv to start with the disk path and be followed by
// triples of name, type and size.
zx_status_t Repartition(int argc, char** argv) {
    const char* dev = argv[0];
    uint64_t logical, free_space;
    fbl::unique_ptr<GptDevice> gpt = Init(dev);
    if (gpt == NULL) {
        return ZX_ERR_INTERNAL;
    }

    argc--;
    argv = &argv[1];
    int num_partitions = argc / 3;

    gpt_partition_t* p = gpt->GetPartition(0);
    while (p) {
        ZX_ASSERT(gpt->RemovePartition(p->guid) == ZX_OK);
        p = gpt->GetPartition(0);
    }

    logical = gpt->BlockSize();
    free_space = gpt->TotalBlockCount() * logical;

    {
        // expand out any proportional sizes into absolute sizes
        uint64_t sizes[num_partitions];
        memset(sizes, 0, sizeof(sizes));
        {
            uint64_t percent = 100;
            uint64_t portions[num_partitions];
            memset(portions, 0, sizeof(portions));
            for (int i = 0; i < num_partitions; i++) {
                int64_t sz = ParseSize(argv[(i * 3) + 2]);
                if (sz > 0) {
                    sizes[i] = sz;
                    free_space -= sz;
                } else {
                    if (percent == 0) {
                        fprintf(stderr, "more than 100%% of free space requested\n");
                        return ZX_ERR_INVALID_ARGS;
                    }
                    // portions from ParseSize are negative
                    portions[i] = -sz;
                    percent -= -sz;
                }
            }
            for (int i = 0; i < num_partitions; i++) {
                if (portions[i] != 0)
                    sizes[i] = (free_space * portions[i]) / 100;
            }
        }

        // TODO(raggi): get physical block size...
        uint64_t physical = 8192;

        uint64_t first_usable = 0;
        uint64_t last_usable = 0;
        ZX_ASSERT(gpt->Range(&first_usable, &last_usable) == ZX_OK);

        uint64_t start = Align(first_usable, logical, physical);

        for (int i = 0; i < num_partitions; i++) {
            char* name = argv[i * 3];
            char* guid_name = argv[(i * 3) + 1];

            uint64_t byte_size = sizes[i];

            uint8_t type[GPT_GUID_LEN];
            if (!KnownGuid::NameToGuid(guid_name, type) && !ParseGuid(guid_name, type)) {
                fprintf(stderr, "GUID could not be parsed: %s\n", guid_name);
                return ZX_ERR_INVALID_ARGS;
            }

            uint8_t guid[GPT_GUID_LEN];
            zx_cprng_draw(guid, GPT_GUID_LEN);

            // end is clamped to the sector before the next aligned partition, in order
            // to avoid wasting alignment space at the tail of partitions.
            uint64_t nblocks = (byte_size / logical) + (byte_size % logical == 0 ? 0 : 1);
            uint64_t end = Align(start + nblocks + 1, logical, physical) - 1;
            if (end > last_usable)
                end = last_usable;

            if (start > last_usable) {
                fprintf(stderr, "partition %s does not fit\n", name);
                return ZX_ERR_OUT_OF_RANGE;
            }

            printf("%s: %" PRIu64 " bytes, %" PRIu64 " blocks, %" PRIu64 "-%" PRIu64 "\n", name,
                   byte_size, nblocks, start, end);
            ZX_ASSERT(gpt->AddPartition(name, type, guid, start, end - start, 0) == ZX_OK);

            start = end + 1;
        }
    }

    return Commit(gpt.get(), dev);
}

} // namespace

int main(int argc, char** argv) {
    bin_name = argv[0];
    const char* cmd;
    uint32_t idx_part;

    if (argc > 1) {
        if (!strcmp(argv[1], "--live-dangerously")) {
            confirm_writes = false;
            argc--;
            argv++;
        }
    }

    if (argc == 1) {
        return Usage(ZX_OK);
    }

    cmd = argv[1];
    if (!strcmp(cmd, "dump")) {
        if (argc <= 2) {
            return Usage(ZX_OK);
        }
        DumpPartitions(argv[2]);
    } else if (!strcmp(cmd, "Init")) {
        if (argc <= 2) {
            return Usage(ZX_OK);
        }
        if (InitGpt(argv[2]) != ZX_OK) {
            return 1;
        }
    } else if (!strcmp(cmd, "add")) {
        if (argc <= 5) {
            return Usage(ZX_OK);
        }
        if (AddPartition(argv[5], strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0),
                         argv[4]) != ZX_OK) {
            return 1;
        }
    } else if (!strcmp(cmd, "remove")) {
        if (argc <= 3) {
            return Usage(ZX_OK);
        }
        if (ReadPartitionIndex(argv[2], &idx_part) != ZX_OK) {
            return Usage(ZX_OK);
        }
        if (RemovePartition(argv[3], idx_part) != ZX_OK) {
            return 1;
        }
    } else if (!strcmp(cmd, "edit")) {
        if (argc <= 5) {
            return Usage(ZX_OK);
        }
        if (ReadPartitionIndex(argv[2], &idx_part) != ZX_OK) {
            return Usage(ZX_OK);
        }
        if (EditPartition(argv[5], idx_part, argv[3], argv[4]) != ZX_OK) {
            return 1;
        }
    } else if (!strcmp(cmd, "edit_cros")) {
        if (argc <= 4) {
            return Usage(ZX_OK);
        }
        if (EditCrosPartition(argv + 2, argc - 2) != ZX_OK) {
            return 1;
        }
    } else if (!strcmp(cmd, "adjust")) {
        if (argc <= 5) {
            return Usage(ZX_OK);
        }
        if (ReadPartitionIndex(argv[2], &idx_part) != ZX_OK) {
            return Usage(ZX_OK);
        }
        if (AdjustPartition(argv[5], idx_part, strtoull(argv[3], NULL, 0),
                            strtoull(argv[4], NULL, 0)) != ZX_OK) {
            return 1;
        }
    } else if (!strcmp(cmd, "visible")) {
        if (argc < 5) {
            return Usage(ZX_OK);
        }
        bool visible;
        if (!strcmp(argv[3], "true")) {
            visible = true;
        } else if (!strcmp(argv[3], "false")) {
            visible = false;
        } else {
            return Usage(ZX_OK);
        }

        if (ReadPartitionIndex(argv[2], &idx_part) != ZX_OK) {
            return Usage(ZX_OK);
        }
        if (SetVisibility(argv[4], idx_part, visible) != ZX_OK) {
            return 1;
        }
    } else if (!strcmp(cmd, "repartition")) {
        if (argc < 6) {
            return Usage(ZX_OK);
        }
        if (argc % 3 != 0) {
            return Usage(ZX_OK);
        }
        return Repartition(argc - 2, &argv[2]);
    } else {
        return Usage(ZX_OK);
    }

    return 0;
}
