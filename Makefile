ISO := /tmp/fishos.iso
DISK := fishos.qcow2

.PHONY: all run-tcg-bios run-uefi-kvm distro-base kernel-clean init-clean base-files-clean clean dist-clean iso-clean

all: $(ISO)

run-tcg-bios: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -m 8G -serial stdio \
		-no-reboot -no-shutdown -M smm=off -s -d int

run-uefi-kvm: ovmf/OVMF.fd $(ISO) $(DISK)
	qemu-system-x86_64 -cdrom $(ISO) -m 12G -serial stdio \
		-no-reboot -no-shutdown -M smm=off -smp 4 -machine q35 \
		-bios ovmf/OVMF.fd \
		-drive file=$(DISK),if=virtio \
		-nic user,model=virtio-net-pci \
		-enable-kvm -cpu host -s

ovmf/OVMF.fd:
	mkdir -p ovmf
	cd ovmf && curl -o OVMF.fd https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd

./jinx:
	curl -Lo jinx https://raw.githubusercontent.com/mintsuki/jinx/353c468765dd9404bacba8e5626d0830528e4300/jinx
	chmod +x jinx

distro-base: ./jinx
	./jinx build base-files kernel init bash binutils bzip2 coreutils diffutils findutils gawk gcc gmp grep gzip less make mpc mpfr nano ncurses pcre2 readline sed tar tzdata xz zlib zstd

distro-graphical: distro-base
	./jinx build xorg-server xf86-input-evdev xf86-video-fbdev jwm xorg-xeyes xterm

$(ISO):
	rm -f builds/kernel.built builds/kernel.packaged
	$(MAKE) distro-base
	./build-support/makeiso.sh $(ISO)

$(DISK):
	qemu-img create -f qcow2 fishos.qcow2 4G

kernel-clean:
	rm -rf builds/kernel* pkgs/kernel*

init-clean:
	rm -rf builds/init* pkgs/init*

base-files-clean:
	rm -rf builds/base-files* pkgs/base-files*

iso-clean:
	rm -rf $(ISO)

clean: kernel-clean init-clean base-files-clean iso-clean
	rm -rf sysroot

dist-clean: ./jinx
	./jinx clean
	rm -rf sysroot $(ISO) jinx ovmf
	chmod -R 777 .jinx-cache
	rm -rf .jinx-cache
