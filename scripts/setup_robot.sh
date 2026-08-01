#!/usr/bin/env bash
# setup_robot.sh - one-shot, idempotent setup of the txdwell build
# environment on the robot (Ubuntu 22.04, kernel 6.8.0-134-generic).
#
# Run ON the robot, from anywhere:  ./scripts/setup_robot.sh
# If sudo needs a password and you run non-interactively: SUDO_PASS=xxx ./scripts/setup_robot.sh
set -euo pipefail

LIBBPF_VER="v1.4.5"
LIBBPF_HOME="${LIBBPF_HOME:-$HOME/libbpf}"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"

SUDO="sudo"
if ! sudo -n true 2>/dev/null; then
	if [ -n "${SUDO_PASS:-}" ]; then
		printf '%s\n' "$SUDO_PASS" | sudo -S true
	else
		echo "sudo needs a password; set SUDO_PASS or run interactively" >&2
	fi
fi

echo "[1/4] apt deps (clang, libelf-dev, zlib1g-dev)"
missing=0
for p in clang libelf-dev zlib1g-dev; do
	dpkg -s "$p" >/dev/null 2>&1 || missing=1
done
if [ "$missing" = 1 ]; then
	$SUDO apt-get update -qq
	$SUDO apt-get install -y clang libelf-dev zlib1g-dev
else
	echo "  already installed"
fi

echo "[2/4] libbpf $LIBBPF_VER -> $LIBBPF_HOME"
if [ ! -f "$LIBBPF_HOME/src/libbpf.a" ]; then
	if [ ! -d "$LIBBPF_HOME/src" ]; then
		rm -rf "$LIBBPF_HOME"
		git clone --depth 1 --branch "$LIBBPF_VER" \
			https://github.com/libbpf/libbpf "$LIBBPF_HOME"
	fi
	make -C "$LIBBPF_HOME/src" BUILD_STATIC_ONLY=1 -j"$(nproc)"
else
	echo "  already built"
fi
# public headers (<bpf/bpf_helpers.h> etc.) into a repo-local staging dir,
# no system install needed
if [ ! -d "$LIBBPF_HOME/dest/usr/include/bpf" ]; then
	make -C "$LIBBPF_HOME/src" BUILD_STATIC_ONLY=1 \
		DESTDIR="$LIBBPF_HOME/dest" install_headers
fi

echo "[3/4] vmlinux.h"
if [ ! -f "$REPO_DIR/vmlinux.h" ]; then
	# /sys/kernel/btf/vmlinux is world-readable on this kernel; use sudo
	# only as a fallback.
	if [ -r /sys/kernel/btf/vmlinux ]; then
		bpftool btf dump file /sys/kernel/btf/vmlinux format c > "$REPO_DIR/vmlinux.h"
	else
		$SUDO bpftool btf dump file /sys/kernel/btf/vmlinux format c > "$REPO_DIR/vmlinux.h"
	fi
else
	echo "  already present"
fi

echo "[4/4] build txdwell"
make -C "$REPO_DIR" LIBBPF_DIR="$LIBBPF_HOME"

echo "setup OK. run: sudo $REPO_DIR/txdwell -h"
