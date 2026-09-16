#!/bin/bash
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1
rm -f /tmp/bb_int.log /tmp/bb_ser.log
type_str() {
  local s="$1"
  for ((i=0; i<${#s}; i++)); do
    ch="${s:$i:1}"
    case "$ch" in " ") key="spc" ;; "/" ) key="slash" ;; *) key="$ch" ;; esac
    echo "sendkey $key"; sleep 0.06
  done
}
(
  sleep "$SECS"
  type_str "elf /bin/busybox echo"; echo "sendkey ret"
  sleep 10
  echo quit
) | timeout $((SECS+50)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio \
    -serial file:/tmp/bb_ser.log \
    -d int -D /tmp/bb_int.log >/dev/null 2>&1
echo "int lines: $(wc -l < /tmp/bb_int.log)"
echo "=== last exceptions ==="
grep -nE 'v=0[de8]' /tmp/bb_int.log | tail -4
echo "=== serial ==="
grep -aE 'PENTRY|SC.|PROC|PF-HALT' /tmp/bb_ser.log | tail -6
