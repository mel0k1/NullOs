#!/bin/bash
# Real Alpine BusyBox test.
SECS=${1:-18}
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1
rm -f /tmp/bb_ser.log /tmp/bb_mon.log
type_str() {
  local s="$1"
  for ((i=0; i<${#s}; i++)); do
    ch="${s:$i:1}"
    case "$ch" in " ") key="spc" ;; "/" ) key="slash" ;; "." ) key="dot" ;;
      *) key="$ch" ;; esac
    echo "sendkey $key"; sleep 0.06
  done
}
(
  sleep "$SECS"
  type_str "elf /bin/busybox echo";  echo "sendkey ret"; sleep 5
  type_str "elf /bin/busybox uname -a"; echo "sendkey ret"; sleep 6
  type_str "elf /bin/busybox ls /";  echo "sendkey ret"; sleep 8
  echo "xp /4096bx 0xb8000"
  sleep 2
  echo quit
) | timeout $((SECS+70)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio \
    -serial file:/tmp/bb_ser.log -d int -D /tmp/bb_int.log > /tmp/bb_mon.log 2>&1

grep '^[0-9a-f]\{16\}: ' /tmp/bb_mon.log | sed 's/^[^:]*: //; s/0x//g' \
  | tr -d ' \r\n' | xxd -r -p 2>/dev/null \
  | perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
      print $c if $c =~ /[\x20-\x7e]/ }' > /tmp/bb_vga.txt

echo "--- VGA tail ---"
fold -w 80 /tmp/bb_vga.txt | grep . | tail -26
echo "--- serial: syscalls & exits ---"
grep -aE 'SC.|PROC|EXIT|unimplemented' /tmp/bb_ser.log | tail -20