# txdwell - usbnet TX queue dwell-time / queue-length observer
#
# Build layout on the robot (Ubuntu 22.04, kernel 6.8):
#   ~/libbpf        libbpf v1.4.5 source build (scripts/setup_robot.sh)
#   ~/usbnet-ebpf   this repo
# Override with: make LIBBPF_DIR=/path/to/libbpf

LIBBPF_DIR ?= ../libbpf
LIBBPF_A   := $(LIBBPF_DIR)/src/libbpf.a
LIBBPF_HDR := $(LIBBPF_DIR)/dest/usr/include

CLANG   ?= clang
CC      ?= gcc
BPFTOOL ?= bpftool

INCLUDES := -I. -Isrc -I$(LIBBPF_HDR)

all: txdwell

vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@.tmp
	mv $@.tmp $@

txdwell.bpf.o: src/txdwell.bpf.c src/txdwell.h vmlinux.h $(LIBBPF_A)
	$(CLANG) -O2 -g -target bpf -D__TARGET_ARCH_x86 $(INCLUDES) -c $< -o $@

txdwell: src/txdwell.c src/txdwell.h txdwell.bpf.o $(LIBBPF_A)
	$(CC) -O2 -g -Wall $(INCLUDES) src/txdwell.c -o $@ $(LIBBPF_A) -lelf -lz

$(LIBBPF_A):
	@echo "static libbpf not found: $(LIBBPF_A)"
	@echo "run scripts/setup_robot.sh, or:"
	@echo "  git clone --depth 1 -b v1.4.5 https://github.com/libbpf/libbpf $(LIBBPF_DIR)"
	@echo "  make -C $(LIBBPF_DIR)/src BUILD_STATIC_ONLY=1"
	@echo "  make -C $(LIBBPF_DIR)/src BUILD_STATIC_ONLY=1 DESTDIR=\$$(cd $(LIBBPF_DIR) && pwd)/dest install_headers"
	@exit 1

clean:
	rm -f txdwell txdwell.bpf.o

distclean: clean
	rm -f vmlinux.h

.PHONY: all clean distclean
