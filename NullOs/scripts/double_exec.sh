#!/bin/bash
# Double-exec regression: run forktest twice.
SECS=${1:-18}
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1
rm -f /tmp/d2_ser.log /tmp/d2_mon.log
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
  type_str "elf /bin/forktest"; echo "sendkey ret"; sleep 7
  type_str "elf /bin/forktest"; echo "sendkey ret"; sleep 9
  echo "xp /4096bx 0xb8000"
  sleep 2
  echo quit
) | timeout $((SECS+60)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio \
    -serial file:/tmp/d2_ser.log > /tmp/d2_mon.log 2>&1

grep '^[0-9a-f]\{16\}: ' /tmp/d2_mon.log | sed 's/^[^:]*: //; s/0x//g' \
  | tr -d ' \r\n' | xxd -r -p 2>/dev/null \
  | perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
      print $c if $c =~ /[\x20-\x7e]/ }' > /tmp/d2_vga.txt

RUNS=$(grep -ac 'forktest] done' /tmp/d2_vga.txt)
PROMPTS=$(grep -ac 'Process finished' /tmp/d2_vga.txt)
echo "runs completed on screen: $RUNS ; process-finished lines: $PROMPTS"
grep -aE 'PF-HALT|EXCEPTION' /tmp/d2_ser.log | head -3
fold -w 80 /tmp/d2_vga.txt | grep . | tail -8