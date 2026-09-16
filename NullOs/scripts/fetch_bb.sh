#!/bin/bash
mkdir -p /tmp/bb && cd /tmp/bb || exit 1
for IP in 151.101.64.1 151.101.65.1 151.101.128.1 146.75.x.x; do
  [ "$IP" = "146.75.x.x" ] && continue
  echo "--- try $IP ---"
  timeout 15 curl -s --resolve dl-cdn.alpinelinux.org:80:$IP \
    "http://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64/" -o idx.html \
    && [ -s idx.html ] && { echo "OK via $IP"; break; }
done
[ -s idx.html ] && grep -o 'busybox-static[^"]*\.apk' idx.html | head -3
