#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds the Arch Linux / CachyOS package, run from the source tree as root
# in an Arch-based system, e.g. in a container:
#   podman run --rm -v "$PWD":/src -w /src archlinux:latest packaging/build-arch.sh
#   podman run --rm -v "$PWD":/src -w /src cachyos/cachyos:latest packaging/build-arch.sh
# XD_VERSION overrides pkgver (letters, digits, dots and underscores only).
# The package lands in ./dist.
set -eu

pacman -Syu --noconfirm --needed base-devel git >/dev/null

src=$PWD
packaging/fetch-subprojects.sh
work=$(mktemp -d)
cp packaging/arch/PKGBUILD "$work/"
version=${XD_VERSION:-$(sed -n 's/^pkgver=//p' "$work/PKGBUILD")}
sed -i "s/^pkgver=.*/pkgver=$version/" "$work/PKGBUILD"
tar --exclude='./build*' --exclude='./dist' --exclude='./.git' \
    --transform "s|^\.|xeondesktop-$version|" -czf "$work/xeondesktop-$version.tar.gz" .

# makepkg refuses to run as root.
useradd -m builder 2>/dev/null || true
chown -R builder "$work"
echo 'builder ALL=(ALL) NOPASSWD: ALL' >/etc/sudoers.d/builder
cd "$work"
sudo -u builder makepkg --syncdeps --noconfirm --cleanbuild

mkdir -p "$src/dist"
cp ./*.pkg.tar.zst "$src/dist/"
for pkg in ./*.pkg.tar.zst; do
    echo "== $pkg"
    pacman -Qip "$pkg" | grep -E '^(Name|Version|Depends On|Optional Deps)'
done
