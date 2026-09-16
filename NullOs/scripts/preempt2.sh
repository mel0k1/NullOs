#!/bin/bash
# Preemption verification: preempt on -> multi 3, expect interleaved steps.
SECS=${1:-18}
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1
rm -f /tmp/pre_ser.log /tmp/pre_mon.log

type_str() {
  local s="$1"
  for ((i=0; i<${#s}; i++)); do
    ch="${s:$i:1}"
    case "$ch" in
      " ") key="spc" ;;
      [A-Z]) key="shift-${ch,,}" ;;
      *)   key="$ch" ;;
    esac
    echo "sendkey $key"; sleep 0.07
  done
}

(
  sleep "$SECS"
  type_str "preempt on"; echo "sendkey ret"; sleep 1.5
  type_str "multi 3";    echo "sendkey ret"; sleep 35
  echo "xp /4096bx 0xb8000"
  sleep 2
  echo quit
) | timeout $((SECS+70)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio \
    -serial file:/tmp/pre_ser.log > /tmp/pre_mon.log 2>&1

grep '^[0-9a-f]\{16\}: ' /tmp/pre_mon.log | sed 's/^[^:]*: //; s/0x//g' \
  | tr -d ' \r\n' | xxd -r -p 2>/dev/null \
  | perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
      print $c if $c =~ /[\x20-\x7e]/ }' > /tmp/pre_vga.txt

echo "--- VGA tail ---"
fold -w 80 /tmp/pre_vga.txt | grep . | tail -12

echo "--- analysis ---"
SEQ=$(grep -o '\[mtask[0-9]\]' /tmp/pre_vga.txt | tr -d '[]mtask' | tr -d '\n')
echo "sequence: $SEQ"
SW=$(echo "$SEQ" | awk '{n=0; for(i=2;i<=length($0);i++) if(substr($0,i,1)!=substr($0,i-1,1)) n++; print n}')
echo "switches: ${SW:-0}   (need >=4 for PASS)"
if [ "${#SEQ}" -ge 18 ] && [ "${SW:-0}" -ge 4 ]; then
  echo "[PASS] IRQ preemption interleaves tasks"
else
  echo "[FAIL]"
fi
echo "--- serial PSW sample ---"
grep -a 'PSW' /tmp/pre_ser.log | head -8
grep -ac 'PSW' /tmp/pre_ser.log