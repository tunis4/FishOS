ISO := /tmp/fishos.iso
DISK := fishos.qcow2

.PHONY: all run-tcg-bios run-uefi-kvm distro-base kernel-clean init-clean base-files-clean clean dist-clean iso-clean

all: $(ISO)

run-tcg-bios: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -m 8G -serial stdio \
		-no-reboot -no-shutdown -M smm=off -s -d int

run-uefi-kvm: ovmf/OVMF.fd $(ISO) $(DISK)
	qemu-system-x86_64 -cdrom $(ISO) -m 12G -serial stdio \
		-no-reboot -no-shutdown -M smm=off -smp 1 -machine q35 \
		-bios ovmf/OVMF.fd \
        -drive file=$(DISK),if=virtio \
		-netdev user,id=net0 -device virtio-net,netdev=net0 \
		-enable-kvm -s -display gtk,gl=on -cpu host \
		-object filter-dump,id=f1,netdev=net0,file=dump.pcap

ovmf/OVMF.fd:
	mkdir -p ovmf
	cd ovmf && curl -o OVMF.fd https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd

$(ISO):
#	rm -f builds/kernel.built builds/kernel.packaged
	./build-support/makeiso.sh $(ISO)

$(DISK):
	qemu-img create -f qcow2 fishos.qcow2 16G

# kernel-clean:
# 	rm -rf builds/kernel* pkgs/kernel*

# init-clean:
# 	rm -rf builds/init* pkgs/init*

# base-files-clean:
# 	rm -rf builds/base-files* pkgs/base-files*

iso-clean:
	rm -rf $(ISO)

# clean: kernel-clean init-clean base-files-clean iso-clean
# 	rm -rf sysroot

# dist-clean: ./jinx
# 	./jinx clean
# 	rm -rf sysroot $(ISO) jinx ovmf
# 	chmod -R 777 .jinx-cache
# 	rm -rf .jinx-cache
