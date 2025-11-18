#! /bin/bash
# $1 is the iso filename
# $2 is the sysroot directory

set -ex

WORK_DIR=`mktemp -d`

if [[ ! "$WORK_DIR" || ! -d "$WORK_DIR" ]]; then
  echo "Could not create temp dir"
  exit 1
fi

function cleanup {
  rm -rf "$WORK_DIR"
  echo "Deleted temp working directory $WORK_DIR"
}
trap cleanup EXIT

ISOROOT=$WORK_DIR/isoroot

SYSROOT=$2
mkdir -p $SYSROOT
# cp -R distro-files/root $SYSROOT || true
cp -R /usr/share/terminfo $SYSROOT/usr/share/ || true
cp distro-files/root/.xinitrc $SYSROOT/root/.xinitrc || true
cp distro-files/usr/bin/init_wrapper $SYSROOT/usr/bin/init_wrapper || true
cp distro-files/etc/bash/bashrc $SYSROOT/etc/bash/bashrc || true
cp distro-files/etc/X11/xorg.conf $SYSROOT/etc/X11/xorg.conf || true
mkdir -p $SYSROOT/var/lib/xkb || true
mkdir -p $SYSROOT/sys/class/graphics/fb0/device || true
mkdir -p $SYSROOT/sys/bus/idk || true
ln -s ../../../../../../bus/idk $SYSROOT/sys/class/graphics/fb0/device/subsystem || true

# Make an initramfs with the sysroot.
( cd $SYSROOT && tar --format=ustar -cf $WORK_DIR/initramfs.tar * )

# Prepare the iso and boot directories.
mkdir -pv $ISOROOT/boot
cp kernel/build/fishix $ISOROOT/boot/
cp $WORK_DIR/initramfs.tar $ISOROOT/boot/
cp distro-files/limine.conf $ISOROOT/boot/

# Install the limine binaries.
cp limine/limine-bios.sys $ISOROOT/boot/
cp limine/limine-bios-cd.bin $ISOROOT/boot/
cp limine/limine-uefi-cd.bin $ISOROOT/boot/
mkdir -pv $ISOROOT/EFI/BOOT
cp limine/BOOT*.EFI $ISOROOT/EFI/BOOT/

# Create the disk image.
xorriso -as mkisofs -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
    -boot-info-table --efi-boot boot/limine-uefi-cd.bin -efi-boot-part \
    --efi-boot-image --protective-msdos-label $ISOROOT -o $1

# Install limine.
./limine/limine bios-install $1
