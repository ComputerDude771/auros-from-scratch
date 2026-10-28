#!/bin/bash
# Makes a GitHub-hosted ubuntu-24.04 runner able to run the othersys VM
# test: room on disk, KVM for the runner user, the packages. Prints
# OS_DIR=<where the machine's files go> for $GITHUB_ENV.
set -euo pipefail
df -h / /mnt >&2 || true
sudo rm -rf /usr/share/dotnet /usr/local/lib/android /opt/ghc /usr/local/.ghcup \
    /opt/hostedtoolcache/CodeQL /usr/local/share/boost /usr/share/swift \
    /usr/local/share/powershell /usr/local/share/chromium /opt/microsoft /opt/google \
    /usr/lib/jvm /opt/az 2>/dev/null || true
sudo swapoff -a 2>/dev/null || true
sudo rm -f /mnt/swapfile /swapfile 2>/dev/null || true
echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' |
    sudo tee /etc/udev/rules.d/99-kvm4all.rules >/dev/null
sudo udevadm control --reload-rules
sudo udevadm trigger --name-match=kvm
sleep 1
ls -l /dev/kvm >&2
[ -w /dev/kvm ] || { echo "runner-setup: no usable /dev/kvm" >&2; exit 1; }
nproc >&2; free -g >&2
sudo apt-get update -q >&2
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
    qemu-system-x86 qemu-utils ovmf xorriso gdisk fdisk dosfstools mtools ntfs-3g \
    tesseract-ocr tesseract-ocr-eng python3-pil gpgv zip >&2
for f in /usr/share/OVMF/OVMF_CODE_4M.ms.fd /usr/share/OVMF/OVMF_VARS_4M.ms.fd; do
    [ -e "$f" ] || { echo "runner-setup: missing $f" >&2; exit 1; }
done
free_kb() { df -Pk "$1" | awk 'NR==2 {print $4}'; }
dir=$HOME/othersys
if [ "$(free_kb /mnt)" -gt "$(free_kb /)" ]; then
    sudo mkdir -p /mnt/othersys && sudo chown "$(id -u):$(id -g)" /mnt/othersys
    dir=/mnt/othersys
fi
mkdir -p "$dir"
df -h / /mnt >&2
echo "OS_DIR=$dir"
