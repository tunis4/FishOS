# Fishix

A hobby kernel for x86_64 with Linux binary compatibility. It is written in C++ and is capable of running a decent amount of Linux userspace programs.

It can run on real hardware from a live USB. Since the focus is currently entirely on userspace support, drivers are very limited.  Only the Limine bootloader is supported.

The long term goal of the project is for it to be usable in place of the Linux kernel on a basic desktop system.

![Screenshot](/screenshot.png?raw=true "Screenshot")
*Progress screenshot from 18 November 2025*

## Building and running

The kernel itself uses Meson and you can build it as you would with any other Meson project.
```sh
cd kernel
meson setup --wipe build
meson compile --jobs $(nproc) -C build
```

Alternatively, you can use the Makefile in the project root which contains convenience commands for building the kernel, building an .iso, downloading Limine and running it in QEMU.

Build kernel: `make -j$(nproc)`

Build an .iso and run in QEMU: `SYSROOT=sysroot ISO=/tmp/fishix.iso make -j$(nproc) run`

The provided distro-files have a bootloader option for launching into a graphical environment, which expects the sysroot to have `xorg` and `mate` installed. You can modify the distro-files to change this.

### Setting up a basic Void Linux sysroot

`XBPS_ARCH=x86_64 xbps-install -S -r sysroot -R "https://repo-default.voidlinux.org/current" base-files bash coreutils runit-void util-linux glibc-locales`

Edit `sysroot/etc/default/libc-locales` then run `xbps-reconfigure -r sysroot -f glibc-locales`

## License

This code is licensed under the [MIT License](LICENSE).

See the [NOTICE.md](NOTICE.md) file for attributions.
