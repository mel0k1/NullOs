#!/bin/bash
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1
qemu-system-x86_64 -kernel build/kernel.bin -m 512M \
  -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
  -display none -no-reboot -S -s \
  -serial file:/tmp/probe_ser.log \
  >/tmp/qemu_err.log 2>&1 &
QPID=$!
sleep 0.5
python3 scripts/stub_probe.py run 6
kill $QPID 2>/dev/null
echo "=== serial bytes: $(wc -c < /tmp/probe_ser.log) ==="
