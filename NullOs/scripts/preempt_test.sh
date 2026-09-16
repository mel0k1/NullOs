#!/bin/bash
# Preemption test: run `multi 3`, snapshot VGA, verify interleaving.
SECS=${1:-20}
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1

type_str() {
  local s="$1"
  for ((i=0; i<${#s}; i++)); do
    ch="${s:$i:1}"
    case "$ch" in
      " ") key="spc" ;;
      "-") key="minus" ;;
      "/") key="slash" ;;
      ".") key="dot" ;;
      [A-Z]) key="shift-${ch,,}" ;;
      *)   key="$ch" ;;
    esac
    echo "sendkey $key"
    sleep 0.06
  done
}

(
  sleep "$SECS"
  type_str "multi 3"
  echo "sendkey ret"
  sleep 45
  echo "xp /4096bx 0xb8000"
  sleep 2
  echo quit
) | timeout $((SECS+60)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio > /tmp/multi_mon.log 2>&1

grep '^[0-9a-f]\{16\}: ' /tmp/multi_mon.log | sed 's/^[^:]*: //; s/0x//g' \
  | tr -d ' \r\n' | xxd -r -p 2>/dev/null \
  | perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
      print $c if $c =~ /[\x20-\x7e]/ }' > /tmp/multi_vga.txt

fold -w 80 /tmp/multi_vga.txt | grep . | tail -30

echo "=== analysis ==="
SEQ=$(grep -o '\[mtask[0-9]\]' /tmp/multi_vga.txt | tr -d '[]mtask' | tr -d '\n')
echo "task sequence: $SEQ"
SWITCHES=$(echo "$SEQ" | awk '{
  cnt=0; for(i=2;i<=length($0);i++){ if(substr($0,i,1)!=substr($0,i-1,1)) cnt++ }
  print cnt }')
echo "switches between tasks: $SWITCHES"
if [ "${SWITCHES:-0}" -ge 4 ]; then
  echo "[PASS] tasks interleave => IRQ preemption works"
else
  echo "[FAIL] no/low interleaving"
fi