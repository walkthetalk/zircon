// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_TASK_H_
#define ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_TASK_H_

#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <fbl/vector.h>
#include <lib/async/cpp/task.h>
#include <lib/fit/function.h>
#include <zircon/types.h>

#include <variant>

namespace devmgr {

// An outstanding operation.  This class is not thread-safe.
class Task : public fbl::RefCounted<Task> {
 public:
  using Completion = fit::function<void(zx_status_t)>;

  Task(async_dispatcher_t* dispatcher, Completion completion = nullptr);

  bool is_completed() const { return std::holds_alternative<zx_status_t>(status_); }
  zx_status_t status() const {
    auto* status = std::get_if<zx_status_t>(&status_);
    return status ? *status : ZX_ERR_UNAVAILABLE;
  }

  // It is an error to destroy a Task before it is completed
  virtual ~Task();

 protected:
  // Run() should never be called manually, instead call PostTask
  //
  // Run() will only be invoked by the async dispatcher
  //
  // Run() may register new dependencies if Complete() has not yet been called,
  // provided that it does not call Complete() afterwards.  In this case Run(),
  // will be invoked again when the new dependencies have completed.
  virtual void Run() = 0;

  // DependencyFailed() may be invoked from outside the async dispatcher, if
  // the dependency was already completed before it was added.
  //
  // This will be invoked whenever a dependency fails.  It may call
  // Complete().  By default, it will mark the task complete and propagate
  // the error code.  This will not be invoked any time after the task has
  // been completed.
  virtual void DependencyFailed(zx_status_t status) { Complete(status); }

  // A task implementation should invoke this when it is completed.
  void Complete(zx_status_t status);

  // Called to record a new dependency
  void AddDependency(const fbl::RefPtr<Task>& dependency);

 private:
  // This will be called when all dependencies have completed.  If when the
  // task is created it has no dependencies, ExecuteTask() should be invoked
  // immediately.  This will call Run()
  void ExecuteTask(async_dispatcher_t* dispatcher, async::TaskBase* task, zx_status_t status);

  // Record a new dependent.  |dependent->DependencyComplete()| will be
  // invoked when |this| is completed (or if it is already completed).
  void RegisterDependent(fbl::RefPtr<Task> dependent);
  // Invoked whenever a dependency completes
  void DependencyComplete(zx_status_t status);

  // List of tasks that should be notified when this task is complete
  fbl::Vector<fbl::RefPtr<Task>> dependents_;

  struct Incomplete {};
  // Whether or not this task has completed
  std::variant<Incomplete, zx_status_t> status_;

  // Function to be called when this task is completed
  Completion completion_;
  // A reference to self that gets set if AddDependency(this) is called on
  // any other Task.  This reference gets dropped by Complete().
  fbl::RefPtr<Task> self_;

  async_dispatcher_t* dispatcher_;
  async::TaskMethod<Task, &Task::ExecuteTask> async_task_{this};

  // Number of dependencies this task has ever had
  size_t total_dependencies_count_ = 0;
  // Number of dependencies of this task that have finished
  size_t finished_dependencies_count_ = 0;
};

}  // namespace devmgr

#endif  // ZIRCON_SYSTEM_CORE_DEVMGR_DEVCOORDINATOR_TASK_H_
