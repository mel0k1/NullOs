#!/bin/bash
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1
echo "--- A) plain -kernel, no gdbstub ---"
qemu-system-x86_64 -kernel build/kernel.bin -m 512M \
  -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
  -display none -no-reboot \
  -serial file:/tmp/a_ser.log >/tmp/a_err.log 2>&1 &
Q=$!
sleep 15; kill $Q 2>/dev/null
echo "A serial bytes: $(wc -c < /tmp/a_ser.log)"
head -3 /tmp/a_ser.log

echo "--- B) -kernel + -S -s, continue via stub ---"
qemu-system-x86_64 -kernel build/kernel.bin -m 512M \
  -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
  -display none -no-reboot -S -s \
  -serial file:/tmp/b_ser.log >/tmp/b_err.log 2>&1 &
Q2=$!
python3 scripts/stub_probe.py run 8 >/dev/null 2>&1
kill $Q2 2>/dev/null
echo "B serial bytes: $(wc -c < /tmp/b_ser.log)"
