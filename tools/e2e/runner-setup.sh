#!/bin/bash
# Makes a GitHub-hosted ubuntu-24.04 runner able to run the test: room
# on disk, KVM for the runner user, and the packages. Prints where the
# machine's files should go as E2E_DIR=... (the workflow puts it in
# $GITHUB_ENV).
set -euo pipefail

say() { echo "runner-setup: $*" >&2; }

df -h / /mnt >&2 || true

# The preinstalled toolchains this test never uses: about 30 GB. The
# Windows disk image alone grows past 20 GB.
sudo rm -rf /usr/share/dotnet /usr/local/lib/android /opt/ghc /usr/local/.ghcup \
    /opt/hostedtoolcache/CodeQL /opt/hostedtoolcache/go /opt/hostedtoolcache/PyPy \
    /usr/local/share/boost /usr/share/swift /usr/local/share/powershell \
    /usr/local/share/chromium /opt/microsoft /opt/google /usr/lib/jvm \
    /usr/local/julia* /opt/az /usr/share/miniconda 2>/dev/null || true
sudo docker system prune -af >/dev/null 2>&1 || true
sudo swapoff -a 2>/dev/null || true
sudo rm -f /mnt/swapfile /swapfile 2>/dev/null || true

# /dev/kvm exists on these runners but belongs to root:kvm 0660.
echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' |
    sudo tee /etc/udev/rules.d/99-kvm4all.rules >/dev/null
sudo udevadm control --reload-rules
sudo udevadm trigger --name-match=kvm
sleep 1
ls -l /dev/kvm >&2
if [ ! -w /dev/kvm ]; then
    say "no usable /dev/kvm: Windows would take a day under TCG. Stopping."
    exit 1
fi
grep -m1 -o -E 'vmx|svm' /proc/cpuinfo >&2 || true
nproc >&2; free -g >&2

sudo apt-get update -q >&2
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
    qemu-system-x86 qemu-utils ovmf swtpm swtpm-tools xorriso \
    tesseract-ocr tesseract-ocr-eng python3-numpy python3-pil zstd unzip gdisk >&2
qemu-system-x86_64 --version >&2
# Ubuntu confines swtpm to libvirt's directories; this TPM's state lives
# beside the disk image.
if [ -e /etc/apparmor.d/usr.bin.swtpm ]; then
    sudo apparmor_parser -R /etc/apparmor.d/usr.bin.swtpm 2>/dev/null || true
fi
dpkg -l ovmf | tail -1 >&2
for f in /usr/share/OVMF/OVMF_CODE_4M.ms.fd /usr/share/OVMF/OVMF_VARS_4M.ms.fd; do
    [ -e "$f" ] || { say "missing $f"; exit 1; }
done

# Whichever of / and /mnt has more room holds the machine.
free_kb() { df -Pk "$1" | awk 'NR==2 {print $4}'; }
dir=$HOME/e2e
if [ "$(free_kb /mnt)" -gt "$(free_kb /)" ]; then
    sudo mkdir -p /mnt/e2e && sudo chown "$(id -u):$(id -g)" /mnt/e2e
    dir=/mnt/e2e
fi
mkdir -p "$dir"
df -h / /mnt >&2
echo "E2E_DIR=$dir"
