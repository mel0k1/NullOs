#!/bin/bash
# Headless fork-resume debugging via custom GDB remote client (ISO boot).
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1

EXTRA_DEFS="-DFORK_AUTORUN_DEBUG" make -B 2>&1 | grep -E ' error' | head -3
make iso >/dev/null 2>&1

ADDR=$(nm build/kernel.bin | awk '/ fork_resume_child$/{print $1}')
echo "[*] fork_resume_child = 0x$ADDR"

rm -f /tmp/gdb_ser.log

qemu-system-x86_64 -cdrom build/nullos.iso -m 512M \
  -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
  -display none -no-reboot -S -s \
  -monitor unix:/tmp/qmon.sock,server,nowait \
  -serial file:/tmp/gdb_ser.log \
  >/tmp/qemu_err.log 2>&1 &
QPID=$!
sleep 0.5

python3 scripts/gdbstub.py "$ADDR" > /tmp/stub_out.txt 2>&1 &
CPID=$!

# wait until crash marker appears in serial, then inspect via HMP
for i in $(seq 1 60); do
  grep -q 'PF-HALT' /tmp/gdb_ser.log && break
  sleep 1
done
sleep 1
python3 scripts/qmon.py

echo "=== stub output ==="
cat /tmp/stub_out.txt
echo ""
echo "=== serial tail ==="
tail -6 /tmp/gdb_ser.log
kill $CPID $QPID 2>/dev/null
