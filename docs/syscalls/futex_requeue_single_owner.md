# zx_futex_requeue_single_owner

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

Wake some number of threads waiting on a futex, and move more waiters to another wait queue.

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_futex_requeue_single_owner(const zx_futex_t* value_ptr,
                                          zx_futex_t current_value,
                                          const zx_futex_t* requeue_ptr,
                                          uint32_t requeue_count,
                                          zx_handle_t new_requeue_owner);
```

## DESCRIPTION

See [`zx_futex_requeue()`] for a full description.

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

None.

## RETURN VALUE

`zx_futex_requeue_single_owner()` returns **ZX_OK** on success.

## ERRORS

**ZX_ERR_INVALID_ARGS**  One of the following is true:
+ Either *value_ptr* or *requeue_ptr* is not a valid userspace pointer
+ Either *value_ptr* or *requeue_ptr* is not aligned to a `sizeof(zx_futex_t)` boundary.
+ *value_ptr* is the same futex as *requeue_ptr*
+ *new_requeue_owner* is currently a member of the waiters for either *value_ptr* or *requeue_ptr*

**ZX_ERR_BAD_HANDLE**  *new_requeue_owner* is not **ZX_HANDLE_INVALID**, and not a valid handle.
**ZX_ERR_WRONG_TYPE**  *new_requeue_owner* is a valid handle, but is not a handle to a thread.
**ZX_ERR_BAD_STATE**  *current_value* does not match the value at *value_ptr*.

## SEE ALSO

 - [futex objects](../objects/futex.md)
 - [`zx_futex_requeue()`]
 - [`zx_futex_wait()`]
 - [`zx_futex_wake()`]

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_futex_requeue()`]: futex_requeue.md
[`zx_futex_wait()`]: futex_wait.md
[`zx_futex_wake()`]: futex_wake.md
