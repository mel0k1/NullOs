#!/bin/bash
# Watch phys 0x3e0000 content evolution: after boot vs after spawn vs after fork.
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1
EXTRA_DEFS="-DFORK_AUTORUN_DEBUG" make -B 2>&1 | grep -E ' error' | head -3

rm -f /tmp/gdb_ser.log
qemu-system-x86_64 -cdrom build/nullos.iso -m 512M \
  -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
  -display none -no-reboot -S -s \
  -monitor unix:/tmp/qmon.sock,server,nowait \
  -serial file:/tmp/gdb_ser.log \
  >/tmp/qemu_err.log 2>&1 &
QPID=$!
sleep 0.5

python3 scripts/gdbstub.py "$(nm build/kernel.bin | awk '/ fork_resume_child$/{print $1}')" \
  > /tmp/stub_out.txt 2>&1 &
CPID=$!

for i in $(seq 1 90); do
  grep -q 'PF-HALT' /tmp/gdb_ser.log && break
  sleep 1
done
sleep 1

python3 - <<'PYEOF'
import sys
sys.path.insert(0, 'scripts')
from qmon import Mon
m = Mon()
print('=== PHYS 0x3e0000 at crash time ===')
print(m.cmd('xp /4gx 0x3e0000', 1.5).decode(errors='replace').split('(qemu)')[-2])
print('=== PHYS 0x3e1000 (the VMMSPLIT table) ===')
print(m.cmd('xp /4gx 0x3e1000', 1.5).decode(errors='replace').split('(qemu)')[-2])
print('=== PHYS of parent code? search: xp around ===')
print(m.cmd('xp /4gx 0x3c5000', 1.5).decode(errors='replace').split('(qemu)')[-2])
PYEOF

grep -aE 'CANARY|PROC|VMMSPLIT' /tmp/gdb_ser.log | head
kill $CPID $QPID 2>/dev/null