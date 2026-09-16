#!/bin/bash
echo "=== DNS/resolv ==="
cat /etc/resolv.conf 2>/dev/null | head -3
echo "=== proxy env ==="
env | grep -i proxy || echo none
echo "=== connectivity probes ==="
for h in 8.8.8.8 dl-cdn.alpinelinux.org deb.debian.org github.com; do
  timeout 4 getent hosts $h >/dev/null 2>&1 && echo "DNS $h OK" || echo "DNS $h FAIL"
done
timeout 5 bash -c 'exec 3<>/dev/tcp/151.101.0.1/80' 2>/dev/null && echo "TCP raw OK" || echo "TCP raw FAIL"
timeout 5 curl -sI https://dl-cdn.alpinelinux.org 2>&1 | head -2 || echo curl_fail
echo "=== apt cache ==="
ls /var/cache/apt/archives/*.deb 2>/dev/null | head -3 || echo empty
apt-get install -y --dry-run musl-tools 2>&1 | tail -3
echo "=== disk search for prebuilt busybox/musl ==="
find /mnt/c/Users/admin/Downloads -maxdepth 3 -iname '*busybox*' -not -path '*/busybox/*' 2>/dev/null | head -10
find /mnt/c/Users/admin/Downloads -maxdepth 3 -iname '*musl*' 2>/dev/null | head -5
