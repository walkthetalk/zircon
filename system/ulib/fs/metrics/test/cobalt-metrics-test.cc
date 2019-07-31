// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

#include <cobalt-client/cpp/collector.h>
#include <cobalt-client/cpp/counter.h>
#include <cobalt-client/cpp/histogram.h>
#include <fs/metrics/cobalt-metrics.h>
#include <lib/zx/time.h>
#include <lib/zx/vmo.h>
#include <zxtest/zxtest.h>

#include <utility>

namespace fs_metrics {
namespace {

// Observed latency.
constexpr uint32_t kLatencyNs = 5000;

constexpr uint32_t kBuckets = 20;

cobalt_client::CollectorOptions MakeOptions() {
    cobalt_client::CollectorOptions options = cobalt_client::CollectorOptions::Debug();
    options.load_config = [](zx::vmo*, size_t*) { return false; };
    options.initial_response_deadline = zx::duration(0);
    options.response_deadline = zx::duration(0);
    return options;
}

cobalt_client::HistogramOptions MakeHistogramOptions() {
    cobalt_client::HistogramOptions options =
        cobalt_client::HistogramOptions::CustomizedExponential(10, 2, 1, 0);
    options.SetMode(cobalt_client::MetricOptions::Mode::kRemote);
    options.metric_id = 1;
    options.event_code = 0;
    return options;
}

cobalt_client::MetricOptions MakeCounterOptions() {
    cobalt_client::MetricOptions options;
    options.SetMode(cobalt_client::MetricOptions::Mode::kRemote);
    options.metric_id = 1;
    options.event_code = 0;
    return options;
}

TEST(CobaltMetricsTest, LogWhileEnabled) {
    fs_metrics::Metrics metrics(MakeOptions(), /*local_metrics*/ false, "TestFs");
    metrics.EnableMetrics(/*should_collect*/ true);

    fs_metrics::VnodeMetrics* vnodes = metrics.mutable_vnode_metrics();
    ASSERT_NOT_NULL(vnodes);
    if (metrics.IsEnabled()) {
        vnodes->close.Add(kLatencyNs);
    }
    // We should have observed 15 hundred usecs.
    EXPECT_EQ(vnodes->close.GetRemoteCount(kLatencyNs), 1);
}

TEST(CobaltMetricsTest, LogWhileNotEnabled) {
    fs_metrics::Metrics metrics(MakeOptions(), /*local_metrics*/ false, "TestFs");
    metrics.EnableMetrics(/*should_collect*/ false);

    fs_metrics::VnodeMetrics* vnodes = metrics.mutable_vnode_metrics();
    ASSERT_NOT_NULL(vnodes);
    if (metrics.IsEnabled()) {
        vnodes->close.Add(kLatencyNs);
    }
    EXPECT_EQ(vnodes->close.GetRemoteCount(kLatencyNs), 0);
}

TEST(CobaltMetricsTest, EnableMetricsEnabled) {
    fs_metrics::Metrics metrics(MakeOptions(), /*local_metrics*/ false, "TestFs");
    fs_metrics::VnodeMetrics* vnodes = metrics.mutable_vnode_metrics();
    ASSERT_NOT_NULL(vnodes);
    ASSERT_EQ(vnodes->metrics_enabled, metrics.IsEnabled());
    metrics.EnableMetrics(/*should_collect*/ true);

    EXPECT_TRUE(metrics.IsEnabled());
    EXPECT_TRUE(vnodes->metrics_enabled);
}

TEST(CobaltMetricsTest, EnableMetricsDisabled) {
    fs_metrics::Metrics metrics(MakeOptions(), /*local_metrics*/ false, "TestFs");
    metrics.EnableMetrics(/*should_collect*/ true);
    fs_metrics::VnodeMetrics* vnodes = metrics.mutable_vnode_metrics();

    ASSERT_NOT_NULL(vnodes);
    ASSERT_EQ(vnodes->metrics_enabled, metrics.IsEnabled());
    metrics.EnableMetrics(/*should_collect*/ false);

    EXPECT_FALSE(metrics.IsEnabled());
    EXPECT_FALSE(vnodes->metrics_enabled);
}

TEST(CobaltMetrics, AddCustomMetric) {
    fs_metrics::Metrics metrics(MakeOptions(), /*local_metrics*/ false, "TestFs");
    metrics.EnableMetrics(/*should_collect*/ false);

    cobalt_client::Histogram<kBuckets> hist =
        cobalt_client::Histogram<kBuckets>(MakeHistogramOptions(), metrics.mutable_collector());
    cobalt_client::Counter counter =
        cobalt_client::Counter(MakeCounterOptions(), metrics.mutable_collector());

    hist.Add(25);
    counter.Increment(20);

    ASSERT_EQ(hist.GetRemoteCount(25), 1);
    ASSERT_EQ(counter.GetRemoteCount(), 20);

    // Sanity check.
    metrics.mutable_collector()->Flush();
}
} // namespace
} // namespace fs_metrics
