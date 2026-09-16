#!/bin/bash
# Smoke-test helper: boot ISO/kernel in QEMU (headless) and dump VGA text.
# Usage: smoke.sh [iso|kernel] [seconds]
set -e
cd /mnt/c/Users/admin/Downloads/NullOs
MODE=${1:-iso}
SECS=${2:-20}

rm -f /tmp/qsmoke.log /tmp/vga.bin /tmp/vga.txt
(
  sleep "$SECS"
  echo "xp /4096bx 0xb8000"
  sleep 2
  echo quit
) | timeout $((SECS+25)) qemu-system-x86_64 \
    ${MODE:+-$( [ "$MODE" = iso ] && echo cdrom build/nullos.iso || echo kernel build/kernel.bin )} \
    -m 512M -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio > /tmp/qsmoke.log 2>&1 || true

grep '^[0-9a-f]\{16\}: ' /tmp/qsmoke.log | sed 's/^[^:]*: //; s/0x//g' \
  | tr -d ' \r\n' > /tmp/hex.txt
xxd -r -p /tmp/hex.txt > /tmp/vga.bin
perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
  print $c if $c =~ /[\x20-\x7e]/ }' /tmp/vga.bin > /tmp/vga.txt

echo "== VGA chars: $(wc -c < /tmp/vga.txt)"
fold -w 80 /tmp/vga.txt | grep -n . | tail -"${3:-32}"
