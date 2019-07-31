// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <crashsvc/crashsvc.h>
#include <fs/pseudo-dir.h>
#include <fs/service.h>
#include <fs/synchronous-vfs.h>
#include <fuchsia/crash/c/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/wait.h>
#include <lib/fidl-async/bind.h>
#include <lib/zx/event.h>
#include <lib/zx/job.h>
#include <lib/zx/process.h>
#include <lib/zx/thread.h>
#include <lib/zx/vmar.h>
#include <mini-process/mini-process.h>
#include <threads.h>
#include <zircon/syscalls/exception.h>
#include <zircon/syscalls/object.h>
#include <zxtest/zxtest.h>

#include <memory>

namespace {

TEST(crashsvc, StartAndStop) {
  zx::job job;
  ASSERT_OK(zx::job::create(*zx::job::default_job(), 0, &job));

  thrd_t thread;
  zx::job job_copy;
  ASSERT_OK(job.duplicate(ZX_RIGHT_SAME_RIGHTS, &job_copy));
  ASSERT_OK(start_crashsvc(std::move(job_copy), ZX_HANDLE_INVALID, &thread));

  ASSERT_OK(job.kill());

  int exit_code = -1;
  EXPECT_EQ(thrd_join(thread, &exit_code), thrd_success);
  EXPECT_EQ(exit_code, 0);
}

constexpr char kTaskName[] = "crashsvc-test";
constexpr uint32_t kTaskNameLen = sizeof(kTaskName) - 1;

// Creates a mini-process under |job|.
void CreateMiniProcess(const zx::job& job, zx::process* process, zx::thread* thread,
                       zx::channel* command_channel) {
  zx::vmar vmar;
  ASSERT_OK(zx::process::create(job, kTaskName, kTaskNameLen, 0, process, &vmar));
  ASSERT_OK(zx::thread::create(*process, kTaskName, kTaskNameLen, 0, thread));

  zx::event event;
  ASSERT_OK(zx::event::create(0, &event));

  ASSERT_OK(start_mini_process_etc(process->get(), thread->get(), vmar.get(), event.release(), true,
                                   command_channel->reset_and_get_address()));
}

// Creates a mini-process under |job| and tells it to crash.
void CreateAndCrashProcess(const zx::job& job, zx::process* process, zx::thread* thread) {
  zx::channel command_channel;
  ASSERT_NO_FATAL_FAILURES(CreateMiniProcess(job, process, thread, &command_channel));

  // Use mini_process_cmd_send() here to send but not wait for a response
  // so we can handle the exception.
  printf("Intentionally crashing test thread '%s', the following dump is expected\n", kTaskName);
  ASSERT_OK(mini_process_cmd_send(command_channel.get(), MINIP_CMD_BUILTIN_TRAP));
}

// Creates a mini-process under |job| and tells it to request a backtrace.
// Blocks until the mini-process thread has successfully resumed.
void CreateAndBacktraceProcess(const zx::job& job, zx::process* process, zx::thread* thread) {
  zx::channel command_channel;
  ASSERT_NO_FATAL_FAILURES(CreateMiniProcess(job, process, thread, &command_channel));

  // Use mini_process_cmd() here to send and block until we get a response.
  printf("Intentionally dumping test thread '%s', the following dump is expected\n", kTaskName);
  ASSERT_OK(mini_process_cmd(command_channel.get(), MINIP_CMD_BACKTRACE_REQUEST, nullptr));
}

TEST(crashsvc, ThreadCrashNoAnalyzer) {
  zx::job parent_job, job;
  ASSERT_OK(zx::job::create(*zx::job::default_job(), 0, &parent_job));
  ASSERT_OK(zx::job::create(parent_job, 0, &job));

  // Catch exceptions on |parent_job| so that the crashing thread doesn't go
  // all the way up to the system crashsvc when our local crashsvc is done.
  zx::channel exception_channel;
  ASSERT_OK(parent_job.create_exception_channel(0, &exception_channel));

  thrd_t cthread;
  zx::job job_copy;
  ASSERT_OK(job.duplicate(ZX_RIGHT_SAME_RIGHTS, &job_copy));
  ASSERT_OK(start_crashsvc(std::move(job_copy), ZX_HANDLE_INVALID, &cthread));

  zx::process process;
  zx::thread thread;
  ASSERT_NO_FATAL_FAILURES(CreateAndCrashProcess(job, &process, &thread));

  // crashsvc should pass exception handling up the chain when done. Once we
  // get the exception, kill the job which will stop exception handling and
  // cause the crashsvc thread to exit.
  ASSERT_OK(exception_channel.wait_one(ZX_CHANNEL_READABLE, zx::time::infinite(), nullptr));
  ASSERT_OK(job.kill());
  EXPECT_EQ(thrd_join(cthread, nullptr), thrd_success);
}

TEST(crashsvc, ThreadBacktraceNoAnalyzer) {
  zx::job parent_job, job;
  ASSERT_OK(zx::job::create(*zx::job::default_job(), 0, &parent_job));
  ASSERT_OK(zx::job::create(parent_job, 0, &job));

  zx::channel exception_channel;
  ASSERT_OK(parent_job.create_exception_channel(0, &exception_channel));

  thrd_t cthread;
  zx::job job_copy;
  ASSERT_OK(job.duplicate(ZX_RIGHT_SAME_RIGHTS, &job_copy));
  ASSERT_OK(start_crashsvc(std::move(job_copy), ZX_HANDLE_INVALID, &cthread));

  zx::process process;
  zx::thread thread;
  ASSERT_NO_FATAL_FAILURES(CreateAndBacktraceProcess(job, &process, &thread));

  // The backtrace request exception should not make it out of crashsvc.
  ASSERT_EQ(exception_channel.wait_one(ZX_CHANNEL_READABLE, zx::time(0), nullptr),
            ZX_ERR_TIMED_OUT);
  ASSERT_OK(job.kill());
  EXPECT_EQ(thrd_join(cthread, nullptr), thrd_success);
}

// Returns the object's koid, or ZX_KOID_INVALID and marks test failure if
// get_info() fails.
template <typename T>
zx_koid_t GetKoid(const zx::object<T>& object) {
  zx_info_handle_basic_t info;
  info.koid = ZX_KOID_INVALID;
  EXPECT_OK(object.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));
  return info.koid;
}

// Provides FIDL stubs for fuchsia::crash::Analyzer.
class CrashAnalyzerStub {
 public:
  enum class Behavior {
    kSuccess,  // Return ZX_OK.
    kError     // Simulate analyzer failure by returning an error.
  };

  // Sets the behavior to use on the next OnNativeException() call. |process|
  // and |thread| are the tasks we expect to be given from crashsvc.
  void SetBehavior(Behavior behavior, const zx::process& process, const zx::thread& thread) {
    behavior_ = behavior;
    process_koid_ = GetKoid(process);
    thread_koid_ = GetKoid(thread);
    ASSERT_NE(process_koid_, ZX_KOID_INVALID);
    ASSERT_NE(thread_koid_, ZX_KOID_INVALID);
  }

  // Creates a virtual file system serving this analyzer at the appropriate path.
  void Serve(async_dispatcher_t* dispatcher, std::unique_ptr<fs::SynchronousVfs>* vfs,
             zx::channel* client) {
    auto directory = fbl::MakeRefCounted<fs::PseudoDir>();
    auto node = fbl::MakeRefCounted<fs::Service>([dispatcher, this](zx::channel channel) {
      auto dispatch = reinterpret_cast<fidl_dispatch_t*>(fuchsia_crash_Analyzer_dispatch);
      return fidl_bind(dispatcher, channel.release(), dispatch, this, &CrashAnalyzerStub::kOps);
    });
    ASSERT_OK(directory->AddEntry(fuchsia_crash_Analyzer_Name, std::move(node)));

    zx::channel server;
    ASSERT_OK(zx::channel::create(0u, client, &server));

    *vfs = std::make_unique<fs::SynchronousVfs>(dispatcher);
    ASSERT_OK((*vfs)->ServeDirectory(std::move(directory), std::move(server)));
  }

  // Returns the number of times OnNativeException() has fired.
  int on_native_exception_count() const { return on_native_exception_count_; }

 private:
  static zx_status_t OnNativeExceptionWrapper(void* ctx, zx_handle_t process, zx_handle_t thread,
                                              fidl_txn_t* txn) {
    return reinterpret_cast<CrashAnalyzerStub*>(ctx)->OnNativeException(zx::process(process),
                                                                        zx::thread(thread), txn);
  }

  zx_status_t OnNativeException(zx::process process, zx::thread thread, fidl_txn_t* txn) {
    ++on_native_exception_count_;

    // Make sure crashsvc passed us the correct task handles.
    EXPECT_EQ(process_koid_, GetKoid(process));
    EXPECT_EQ(thread_koid_, GetKoid(thread));

    // Build a reply corresponding to our desired behavior.
    fuchsia_crash_Analyzer_OnNativeException_Result result;
    if (behavior_ == Behavior::kSuccess) {
      result.tag = fuchsia_crash_Analyzer_OnNativeException_ResultTag_response;
    } else {
      result.tag = fuchsia_crash_Analyzer_OnNativeException_ResultTag_err;
      result.err = ZX_ERR_BAD_STATE;
    }

    zx_status_t status = fuchsia_crash_AnalyzerOnNativeException_reply(txn, &result);
    EXPECT_EQ(status, ZX_OK);
    return status;
  }

  static constexpr fuchsia_crash_Analyzer_ops_t kOps = {
      .OnNativeException = OnNativeExceptionWrapper,
      .OnManagedRuntimeException = nullptr,
      .OnKernelPanicCrashLog = nullptr};

  Behavior behavior_;
  zx_koid_t process_koid_ = ZX_KOID_INVALID;
  zx_koid_t thread_koid_ = ZX_KOID_INVALID;
  int on_native_exception_count_ = 0;
};

// Creates a new thread, crashes it, and processes the resulting Analyzer FIDL
// message from crashsvc according to |behavior|.
//
// |parent_job| is used to catch exceptions after they've been analyzed on |job|
// so that they don't bubble up to the real crashsvc.
void AnalyzeCrash(CrashAnalyzerStub* analyzer, async::Loop* loop, const zx::job& parent_job,
                  const zx::job& job, CrashAnalyzerStub::Behavior behavior) {
  zx::channel exception_channel;
  ASSERT_OK(parent_job.create_exception_channel(0, &exception_channel));

  zx::process process;
  zx::thread thread;
  ASSERT_NO_FATAL_FAILURES(CreateAndCrashProcess(job, &process, &thread));

  ASSERT_NO_FATAL_FAILURES(analyzer->SetBehavior(behavior, process, thread));

  // Run the loop until the exception filters up to our job handler.
  async::Wait wait(exception_channel.get(), ZX_CHANNEL_READABLE, [&loop](...) { loop->Quit(); });
  ASSERT_OK(wait.Begin(loop->dispatcher()));
  ASSERT_EQ(loop->Run(), ZX_ERR_CANCELED);
  ASSERT_OK(loop->ResetQuit());

  // The exception is now waiting in |exception_channel|, kill the process
  // before the channel closes to keep it from propagating further.
  ASSERT_OK(process.kill());
  ASSERT_OK(process.wait_one(ZX_PROCESS_TERMINATED, zx::time::infinite(), nullptr));
}

TEST(crashsvc, ThreadCrashAnalyzerSuccess) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  std::unique_ptr<fs::SynchronousVfs> vfs;
  zx::channel client;
  CrashAnalyzerStub analyzer;
  ASSERT_NO_FATAL_FAILURES(analyzer.Serve(loop.dispatcher(), &vfs, &client));

  zx::job parent_job, job, job_copy;
  thrd_t cthread;
  ASSERT_OK(zx::job::create(*zx::job::default_job(), 0, &parent_job));
  ASSERT_OK(zx::job::create(parent_job, 0, &job));
  ASSERT_OK(job.duplicate(ZX_RIGHT_SAME_RIGHTS, &job_copy));
  ASSERT_OK(start_crashsvc(std::move(job_copy), client.get(), &cthread));

  ASSERT_NO_FATAL_FAILURES(
      AnalyzeCrash(&analyzer, &loop, parent_job, job, CrashAnalyzerStub::Behavior::kSuccess));
  EXPECT_EQ(1, analyzer.on_native_exception_count());

  ASSERT_OK(job.kill());
  EXPECT_EQ(thrd_join(cthread, nullptr), thrd_success);
}

TEST(crashsvc, ThreadCrashAnalyzerFailure) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  std::unique_ptr<fs::SynchronousVfs> vfs;
  zx::channel client;
  CrashAnalyzerStub analyzer;
  ASSERT_NO_FATAL_FAILURES(analyzer.Serve(loop.dispatcher(), &vfs, &client));

  zx::job parent_job, job, job_copy;
  thrd_t cthread;
  ASSERT_OK(zx::job::create(*zx::job::default_job(), 0, &parent_job));
  ASSERT_OK(zx::job::create(parent_job, 0, &job));
  ASSERT_OK(job.duplicate(ZX_RIGHT_SAME_RIGHTS, &job_copy));
  ASSERT_OK(start_crashsvc(std::move(job_copy), client.get(), &cthread));

  ASSERT_NO_FATAL_FAILURES(
      AnalyzeCrash(&analyzer, &loop, parent_job, job, CrashAnalyzerStub::Behavior::kError));
  EXPECT_EQ(1, analyzer.on_native_exception_count());

  ASSERT_OK(job.kill());
  EXPECT_EQ(thrd_join(cthread, nullptr), thrd_success);
}

TEST(crashsvc, MultipleThreadCrashAnalyzer) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  std::unique_ptr<fs::SynchronousVfs> vfs;
  zx::channel client;
  CrashAnalyzerStub analyzer;
  ASSERT_NO_FATAL_FAILURES(analyzer.Serve(loop.dispatcher(), &vfs, &client));

  zx::job parent_job, job, job_copy;
  thrd_t cthread;
  ASSERT_OK(zx::job::create(*zx::job::default_job(), 0, &parent_job));
  ASSERT_OK(zx::job::create(parent_job, 0, &job));
  ASSERT_OK(job.duplicate(ZX_RIGHT_SAME_RIGHTS, &job_copy));
  ASSERT_OK(start_crashsvc(std::move(job_copy), client.get(), &cthread));

  // Make sure crashsvc continues to loop no matter what the analyzer does.
  ASSERT_NO_FATAL_FAILURES(
      AnalyzeCrash(&analyzer, &loop, parent_job, job, CrashAnalyzerStub::Behavior::kSuccess));
  ASSERT_NO_FATAL_FAILURES(
      AnalyzeCrash(&analyzer, &loop, parent_job, job, CrashAnalyzerStub::Behavior::kError));
  ASSERT_NO_FATAL_FAILURES(
      AnalyzeCrash(&analyzer, &loop, parent_job, job, CrashAnalyzerStub::Behavior::kSuccess));
  ASSERT_NO_FATAL_FAILURES(
      AnalyzeCrash(&analyzer, &loop, parent_job, job, CrashAnalyzerStub::Behavior::kError));
  EXPECT_EQ(4, analyzer.on_native_exception_count());

  ASSERT_OK(job.kill());
  EXPECT_EQ(thrd_join(cthread, nullptr), thrd_success);
}

TEST(crashsvc, ThreadBacktraceAnalyzer) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  std::unique_ptr<fs::SynchronousVfs> vfs;
  zx::channel client;
  CrashAnalyzerStub analyzer;
  ASSERT_NO_FATAL_FAILURES(analyzer.Serve(loop.dispatcher(), &vfs, &client));

  zx::job parent_job, job, job_copy;
  thrd_t cthread;
  ASSERT_OK(zx::job::create(*zx::job::default_job(), 0, &parent_job));
  ASSERT_OK(zx::job::create(parent_job, 0, &job));
  ASSERT_OK(job.duplicate(ZX_RIGHT_SAME_RIGHTS, &job_copy));
  ASSERT_OK(start_crashsvc(std::move(job_copy), client.get(), &cthread));

  zx::process process;
  zx::thread thread;
  ASSERT_NO_FATAL_FAILURES(CreateAndBacktraceProcess(job, &process, &thread));

  // Thread backtrace requests shouldn't be sent out to the analyzer.
  EXPECT_EQ(0, analyzer.on_native_exception_count());

  ASSERT_OK(job.kill());
  EXPECT_EQ(thrd_join(cthread, nullptr), thrd_success);
}

}  // namespace
