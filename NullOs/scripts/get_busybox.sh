#!/bin/bash
set -e
mkdir -p /tmp/bb && cd /tmp/bb
IP=151.101.64.1
URL="http://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64"
PKG=busybox-static-1.36.1-r31.apk

curl -s --resolve dl-cdn.alpinelinux.org:80:$IP "$URL/$PKG" -o $PKG
echo "apk size: $(wc -c < $PKG)"
tar -xzf $PKG
ls -la . | grep -v '^d'
file bin/busybox.static || true
cp bin/busybox.static /mnt/c/Users/admin/Downloads/NullOs/userland/busybox.static
echo "copied to userland/busybox.static: $(wc -c < /mnt/c/Users/admin/Downloads/NullOs/userland/busybox.static) bytes"
readelf -h bin/busybox.static 2>/dev/null | head -12 || true
