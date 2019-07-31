# Bootsvc

`bootsvc` is (typically) the first program loaded by usermode (contrast with
[userboot](userboot.md), which is loaded by the kernel).  `bootsvc` provides
several system services:

- A filesystem service with the contents of the bootfs (/boot)
- A loader service that loads from that bootfs

After preparing these services, it launches one program from the bootfs.  The
program may be selected with a [kernel command line argument](kernel_cmdline.md)
`bootsvc.next` (this default to `bin/devmgr` currently).  The launched program
is provided with the bootfs mounted at `/boot` and the loader service.  The
kernel command line arguments are provided to it via `envp`.  See the
documentation in `system/core/bootsvc/main.cpp:LaunchNextProcess()` for more
details.
