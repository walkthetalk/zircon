# zx_object_wait_one

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

Wait for signals on an object.

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_object_wait_one(zx_handle_t handle,
                               zx_signals_t signals,
                               zx_time_t deadline,
                               zx_signals_t* observed);
```

## DESCRIPTION

`zx_object_wait_one()` is a blocking syscall which causes the caller to
wait until either the *deadline* passes or the object to which *handle* refers
asserts at least one of the specified *signals*. If the object is already
asserting at least one of the specified *signals*, the wait ends immediately.

Upon return, if non-NULL, *observed* is a bitmap of *all* of the
signals which were observed asserted on that object while waiting.

The *observed* signals may not reflect the actual state of the object's
signals if the state of the object was modified by another thread or
process.  (For example, a Channel ceases asserting **ZX_CHANNEL_READABLE**
once the last message in its queue is read).

The *deadline* parameter specifies a deadline with respect to
**ZX_CLOCK_MONOTONIC** and will be automatically adjusted according to the job's
[timer slack] policy.  **ZX_TIME_INFINITE** is a special value meaning wait
forever.

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

*handle* must have **ZX_RIGHT_WAIT**.

## RETURN VALUE

`zx_object_wait_one()` returns **ZX_OK** if any of *signals* were observed
on the object before *deadline* passes.

In the event of **ZX_ERR_TIMED_OUT**, *observed* may reflect state changes
that occurred after the deadline passed, but before the syscall returned.

In the event of **ZX_ERR_CANCELED**, *handle* has been closed,
and *observed* will have the **ZX_SIGNAL_HANDLE_CLOSED** bit set.

For any other return value, *observed* is undefined.

## ERRORS

**ZX_ERR_INVALID_ARGS**  *observed* is an invalid pointer.

**ZX_ERR_BAD_HANDLE**  *handle* is not a valid handle.

**ZX_ERR_ACCESS_DENIED**  *handle* does not have **ZX_RIGHT_WAIT** and may
not be waited upon.

**ZX_ERR_CANCELED**  *handle* was invalidated (e.g., closed) during the wait.

**ZX_ERR_TIMED_OUT**  The specified deadline passed before any of the specified
*signals* are observed on *handle*.

**ZX_ERR_NOT_SUPPORTED**  *handle* is a handle that cannot be waited on
(for example, a Port handle).

## SEE ALSO

 - [timer slack](../timer_slack.md)
 - [`zx_object_wait_async()`]
 - [`zx_object_wait_many()`]

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_object_wait_async()`]: object_wait_async.md
[`zx_object_wait_many()`]: object_wait_many.md
