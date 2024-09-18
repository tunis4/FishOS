# FishOS

operating system of the fish

- written in c++
- currently capable of running xorg, gcc, bash, coreutils, etc
- uses mlibc and limine
- ps/2 keyboard and mouse passed to userspace with linux evdev interface
- nothing else impressive yet lol

![Screenshot](/screenshot.png?raw=true "Screenshot")

### third party stuff

most of the userspace recipes/patches were copied from [Vinix](https://github.com/vlang/vinix/)

the terminal font (ter-u16n.psf, ter-u16b.psf) is [Terminus Font](https://terminus-font.sourceforge.net/)
