#!/bin/bash
# $1 is the iso filename

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

SYSROOT=$WORK_DIR/sysroot
ISOROOT=$WORK_DIR/isoroot

# Build the sysroot with jinx and build limine.
./jinx install $SYSROOT '*'
./jinx host-build limine

# Make an initramfs with the sysroot.
( cd $SYSROOT && tar cf $WORK_DIR/initramfs.tar * )

# Prepare the iso and boot directories.
mkdir -pv $ISOROOT/boot
cp $SYSROOT/usr/bin/fishos $ISOROOT/boot/
cp $WORK_DIR/initramfs.tar $ISOROOT/boot/
cp build-support/limine.conf $ISOROOT/boot/

# Install the limine binaries.
cp host-pkgs/limine/usr/local/share/limine/limine-bios.sys $ISOROOT/boot/
cp host-pkgs/limine/usr/local/share/limine/limine-bios-cd.bin $ISOROOT/boot/
cp host-pkgs/limine/usr/local/share/limine/limine-uefi-cd.bin $ISOROOT/boot/
mkdir -pv $ISOROOT/EFI/BOOT
cp host-pkgs/limine/usr/local/share/limine/BOOT*.EFI $ISOROOT/EFI/BOOT/

# Create the disk image.
xorriso -as mkisofs -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
    -boot-info-table --efi-boot boot/limine-uefi-cd.bin -efi-boot-part \
    --efi-boot-image --protective-msdos-label $ISOROOT -o $1

# Install limine.
host-pkgs/limine/usr/local/bin/limine bios-install $1
