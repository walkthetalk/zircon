# zx_handle_duplicate

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

Duplicate a handle.

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_handle_duplicate(zx_handle_t handle,
                                zx_rights_t rights,
                                zx_handle_t* out);
```

## DESCRIPTION

`zx_handle_duplicate()` creates a duplicate of *handle*, referring
to the same underlying object, with new access rights *rights*.

To duplicate the handle with the same rights use **ZX_RIGHT_SAME_RIGHTS**. If different
rights are desired they must be strictly lesser than of the source handle. It is possible
to specify no rights by using 0.

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

*handle* must have **ZX_RIGHT_DUPLICATE**.

## RETURN VALUE

`zx_handle_duplicate()` returns **ZX_OK** and the duplicate handle via *out* on success.

## ERRORS

**ZX_ERR_BAD_HANDLE**  *handle* isn't a valid handle.

**ZX_ERR_INVALID_ARGS**  The *rights* requested are not a subset of *handle* rights or
*out* is an invalid pointer.

**ZX_ERR_ACCESS_DENIED**  *handle* does not have **ZX_RIGHT_DUPLICATE** and may not be duplicated.

**ZX_ERR_NO_MEMORY**  Failure due to lack of memory.
There is no good way for userspace to handle this (unlikely) error.
In a future build this error will no longer occur.

## SEE ALSO

 - [rights](../rights.md)
 - [`zx_handle_close()`]
 - [`zx_handle_close_many()`]
 - [`zx_handle_replace()`]

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_handle_close()`]: handle_close.md
[`zx_handle_close_many()`]: handle_close_many.md
[`zx_handle_replace()`]: handle_replace.md
