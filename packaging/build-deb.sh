#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds the .deb in a Debian (testing or newer) system, run from the
# source tree as root, e.g. in a container:
#   podman run --rm -v "$PWD":/src -w /src debian:testing packaging/build-deb.sh
# XD_VERSION overrides the version (CI uses 0.2.0+git<date>.<commit>).
# The packages land in ./dist.
set -eu

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends build-essential devscripts equivs git ca-certificates >/dev/null
packaging/fetch-subprojects.sh

# Build in a copy, so the tree (and its build directories) stay as they are.
work=$(mktemp -d)
tar --exclude='./build*' --exclude='./dist' --exclude='./.git' -cf - . | tar -xf - -C "$work"
cd "$work"

if [ -n "${XD_VERSION:-}" ]; then
    {
        printf 'xeondesktop (%s) unstable; urgency=medium\n\n' "$XD_VERSION"
        printf '  * Automated build.\n\n'
        printf ' -- xeon <xeon.productions@gmail.com>  %s\n\n' "$(date -R)"
        cat debian/changelog
    } >debian/changelog.new
    mv debian/changelog.new debian/changelog
fi

mk-build-deps --install --remove --tool 'apt-get -y -qq --no-install-recommends' debian/control
dpkg-buildpackage -us -uc -b

mkdir -p "$OLDPWD/dist"
cp ../*.deb "$OLDPWD/dist/"
for deb in ../*.deb; do
    echo "== $deb"
    dpkg-deb --field "$deb" Package Version Depends Recommends
done
