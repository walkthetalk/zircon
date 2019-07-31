// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains information for gathering Blobfs metrics.

#pragma once

#ifndef __Fuchsia__
#error Fuchsia-only Header
#endif

#include <cobalt-client/cpp/collector.h>
#include <fs/metrics/cobalt-metrics.h>
#include <fs/metrics/composite-latency-event.h>
#include <fs/metrics/events.h>
#include <fs/metrics/histograms.h>
#include <fs/ticker.h>
#include <lib/inspect-vmo/inspect.h>
#include <lib/inspect-vmo/types.h>
#include <lib/zx/time.h>

namespace blobfs {

// Alias for the LatencyEvent used in blobfs.
using LatencyEvent = fs_metrics::CompositeLatencyEvent;

class BlobfsMetrics {
public:
    // Print information about metrics to stdout.
    //
    // TODO(ZX-1999): This is a stop-gap solution; long-term, this information
    // should be extracted from devices.
    void Dump() const;

    void Collect() { cobalt_metrics_.EnableMetrics(true); }
    bool Collecting() const { return cobalt_metrics_.IsEnabled(); }
    void Disable() { cobalt_metrics_.EnableMetrics(false); }

    // Updates aggregate information about the total number of created
    // blobs since mounting.
    void UpdateAllocation(uint64_t size_data, const fs::Duration& duration);

    // Updates aggregate information about the number of blobs opened
    // since mounting.
    void UpdateLookup(uint64_t size);

    // Updates aggregates information about blobs being written back
    // to blobfs since mounting.
    void UpdateClientWrite(uint64_t data_size, uint64_t merkle_size,
                           const fs::Duration& enqueue_duration,
                           const fs::Duration& generate_duration);

    // Updates aggregate information about flushing bits down
    // to the underlying storage driver.
    void UpdateWriteback(uint64_t size, const fs::Duration& duration);

    // Updates aggregate information about reading blobs from storage
    // since mounting.
    void UpdateMerkleDiskRead(uint64_t size, const fs::Duration& duration);

    // Updates aggregate information about decompressing blobs from storage
    // since mounting.
    void UpdateMerkleDecompress(uint64_t size_compressed, uint64_t size_uncompressed,
                                const fs::Duration& read_duration,
                                const fs::Duration& decompress_duration);

    // Updates aggregate information about general verification info
    // since mounting.
    void UpdateMerkleVerify(uint64_t size_data, uint64_t size_merkle, const fs::Duration& duration);

    // Returns a new Latency event for the given event. This requires the event to be backed up by
    // an histogram in both cobalt metrics and Inspect.
    LatencyEvent NewLatencyEvent(fs_metrics::Event event) {
        return LatencyEvent(event, &histograms_, cobalt_metrics_.mutable_vnode_metrics());
    }

    // Returns the underlying collector of cobalt metrics.
    cobalt_client::Collector* mutable_collector() { return cobalt_metrics_.mutable_collector(); }

private:
    static cobalt_client::CollectorOptions GetBlobfsOptions();

    // ALLOCATION STATS

    // Created with external-facing "Create".
    uint64_t blobs_created_ = 0;
    // Measured by space allocated with "Truncate".
    uint64_t blobs_created_total_size_ = 0;
    zx::ticks total_allocation_time_ticks_ = {};

    // WRITEBACK STATS

    // Measurements, from the client's perspective, of writing and enqueing
    // data that will later be written to disk.
    uint64_t data_bytes_written_ = 0;
    uint64_t merkle_bytes_written_ = 0;
    zx::ticks total_write_enqueue_time_ticks_ = {};
    zx::ticks total_merkle_generation_time_ticks_ = {};
    // Measured by true time writing back to disk. This may be distinct from
    // the client time because of asynchronous writeback buffers.
    zx::ticks total_writeback_time_ticks_ = {};
    uint64_t total_writeback_bytes_written_ = 0;

    // LOOKUP STATS

    // Total time waiting for reads from disk.
    zx::ticks total_read_from_disk_time_ticks_ = {};
    uint64_t bytes_read_from_disk_ = 0;

    zx::ticks total_read_compressed_time_ticks_ = {};
    zx::ticks total_decompress_time_ticks_ = {};
    uint64_t bytes_compressed_read_from_disk_ = 0;
    uint64_t bytes_decompressed_from_disk_ = 0;

    // Opened via "LookupBlob".
    uint64_t blobs_opened_ = 0;
    uint64_t blobs_opened_total_size_ = 0;
    // Verified blob data (includes both blobs read and written).
    uint64_t blobs_verified_ = 0;
    uint64_t blobs_verified_total_size_data_ = 0;
    uint64_t blobs_verified_total_size_merkle_ = 0;
    zx::ticks total_verification_time_ticks_ = {};

    // FVM STATS
    // TODO(smklein)

    // Inspect instrumentation data, with an initial size of the current histogram size.
    inspect::vmo::Inspector inspector_ =
        inspect::vmo::Inspector(fs_metrics::Histograms::Size(), 2 * fs_metrics::Histograms::Size());
    inspect::vmo::Object root_ = inspector_.CreateObject("metrics");
    fs_metrics::Histograms histograms_ = fs_metrics::Histograms(&root_);

    // Cobalt metrics.
    fs_metrics::Metrics cobalt_metrics_ = fs_metrics::Metrics(GetBlobfsOptions(), false, "blobfs");
};

} // namespace blobfs
