// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "work-queue.h"
#include "minfs-private.h"

namespace minfs {

WorkQueue::~WorkQueue() {
    if (thrd_) {
        {
            fbl::AutoLock lock(&lock_);
            unmounting_ = true;
            data_cvar_.Signal();
        }

        int r;
        thrd_join(thrd_.value(), &r);
        ZX_DEBUG_ASSERT(r == 0);
    }

    ZX_DEBUG_ASSERT(IsEmpty());
}

zx_status_t WorkQueue::Create(TransactionalFs* minfs, fbl::unique_ptr<WorkQueue>* out) {
    fbl::unique_ptr<WorkQueue> processor(new WorkQueue(minfs));

    processor->thrd_ = std::make_optional<thrd_t>();

    if (thrd_create_with_name(&processor->thrd_.value(), DataThread, processor.get(),
                              "minfs-data-async") != thrd_success) {
        return ZX_ERR_NO_RESOURCES;
    }

    *out = std::move(processor);
    return ZX_OK;
}

void WorkQueue::EnqueueCallback(TaskCallback task) {
    fbl::AutoLock lock(&lock_);
    ReserveTask(std::move(task));
    data_cvar_.Signal();
}

bool WorkQueue::TasksWaiting() const {
    fbl::AutoLock lock(&lock_);
    return waiting_ > 0;
}

void WorkQueue::ProcessNext() {
    ZX_DEBUG_ASSERT(!IsEmpty());
    std::optional<TaskCallback> task;
    task_queue_[start_].swap(task);
    ZX_DEBUG_ASSERT(task.has_value());

    lock_.Release();
    (*task)(minfs_);
    task = nullptr;
    lock_.Acquire();

    if (waiting_ > 0) {
        sync_cvar_.Signal();
    }

    // Update the queue.
    start_ = (start_ + 1) % kMaxQueued;
    count_--;
}

void WorkQueue::ReserveTask(TaskCallback task) {
    EnsureQueueSpace();
    ZX_DEBUG_ASSERT(count_ < kMaxQueued);
    count_++;
    uint32_t task_index = (start_ + count_ - 1) % kMaxQueued;
    ZX_DEBUG_ASSERT(!task_queue_[task_index].has_value());
    task_queue_[task_index] = std::move(task);
    ZX_DEBUG_ASSERT(task_queue_[task_index].has_value());
}

bool WorkQueue::IsEmpty() const {
    return count_ == 0;
}

void WorkQueue::EnsureQueueSpace() {
    ZX_DEBUG_ASSERT(count_ <= kMaxQueued);
    while (count_ == kMaxQueued) {
        waiting_++;
        sync_cvar_.Wait(&lock_);
        waiting_--;
    }
    ZX_DEBUG_ASSERT(count_ < kMaxQueued);
}

void WorkQueue::ProcessLoop() {
    fbl::AutoLock lock(&lock_);
    while (true) {
        while (!IsEmpty()) {
            ProcessNext();
        }

        if (unmounting_) {
            // Verify that the queue is empty.
            ZX_DEBUG_ASSERT(IsEmpty());
            break;
        }

        if (IsEmpty()) {
            // If no updates have been queued, wait indefinitely until we are signalled.
            data_cvar_.Wait(&lock_);
        }
    }
}

int WorkQueue::DataThread(void* arg) {
    WorkQueue* assigner = static_cast<WorkQueue*>(arg);
    assigner->ProcessLoop();
    return 0;
}

} // namespace minfs
