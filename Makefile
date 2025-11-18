SYSROOT ?= sysroot
ISO ?= /tmp/fishix.iso
DISK ?= fishix.qcow2

NPROC := $(patsubst -j%,%,$(filter -j%,$(MAKEFLAGS)))
ifeq ($(NPROC),)
NPROC := 1
endif

.PHONY: all kernel run iso-clean

all: kernel

run: ovmf/OVMF.fd $(ISO) $(DISK)
	qemu-system-x86_64 -cdrom $(ISO) -m 16G -serial stdio \
		-no-reboot -no-shutdown -M smm=off -smp 1 -machine q35 -cpu host \
		-bios ovmf/OVMF.fd \
        -drive file=$(DISK),if=virtio \
		-netdev user,id=net0 -device virtio-net,netdev=net0 \
		-enable-kvm -display gtk,gl=on -s
#		-object filter-dump,id=f1,netdev=net0,file=dump.pcap

kernel: kernel/build
	cd kernel && meson compile --jobs $(NPROC) -C build

kernel/build:
	cd kernel && meson setup build

limine:
	git clone --depth 1 --branch v10.x-binary https://codeberg.org/Limine/Limine limine
	cd limine && make

ovmf/OVMF.fd:
	mkdir -p ovmf
	cd ovmf && curl -o OVMF.fd https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd

$(ISO): kernel limine
	./distro-files/makeiso.sh $(ISO) $(SYSROOT)

$(DISK):
	qemu-img create -f qcow2 $(DISK) 16G

iso-clean:
	rm -rf $(ISO)
