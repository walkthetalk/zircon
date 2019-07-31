# zx_vmo_set_size

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

Resize a VMO object.

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_vmo_set_size(zx_handle_t handle, uint64_t size);
```

## DESCRIPTION

`zx_vmo_set_size()` sets the new size of a VMO object.

The size will be rounded up to the next page size boundary.
Subsequent calls to [`zx_vmo_get_size()`] will return the rounded up size.

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

*handle* must be of type **ZX_OBJ_TYPE_VMO** and have **ZX_RIGHT_WRITE**.

## RETURN VALUE

`zx_vmo_set_size()` returns **ZX_OK** on success. In the event
of failure, a negative error value is returned.

## ERRORS

**ZX_ERR_BAD_HANDLE**  *handle* is not a valid handle.

**ZX_ERR_WRONG_TYPE**  *handle* is not a VMO handle.

**ZX_ERR_ACCESS_DENIED**  *handle* does not have the **ZX_RIGHT_WRITE** right.

**ZX_ERR_UNAVAILABLE** The VMO was not created with **ZX_VMO_RESIZABLE**
or **ZX_VMO_CHILD_RESIZABLE**.

**ZX_ERR_OUT_OF_RANGE**  Requested size is too large.

**ZX_ERR_NO_MEMORY**  Failure due to lack of system memory.

## SEE ALSO

 - [`zx_vmo_create()`]
 - [`zx_vmo_create_child()`]
 - [`zx_vmo_get_size()`]
 - [`zx_vmo_op_range()`]
 - [`zx_vmo_read()`]
 - [`zx_vmo_write()`]

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_vmo_create()`]: vmo_create.md
[`zx_vmo_create_child()`]: vmo_create_child.md
[`zx_vmo_get_size()`]: vmo_get_size.md
[`zx_vmo_op_range()`]: vmo_op_range.md
[`zx_vmo_read()`]: vmo_read.md
[`zx_vmo_write()`]: vmo_write.md
