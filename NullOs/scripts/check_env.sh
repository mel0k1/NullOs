#!/bin/bash
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1
echo "=== busybox folder tail ==="
ls busybox | tail -22
echo "=== musl? ==="
find /usr /opt /home -maxdepth 4 -name 'musl*' 2>/dev/null | head -5
which musl-gcc || echo NO_MUSL_GCC
echo "=== network test ==="
timeout 10 wget -q -O /tmp/apk.tar.gz http://dl-cdn.alpinelinux.org/alpine/v3.19/main/x86_64/busybox-static-1.36.1-r19.apk && echo "NET_OK $(wc -c < /tmp/apk.tar.gz) bytes" || echo NET_FAIL
