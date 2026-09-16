#!/bin/bash
# nsh (native busybox-style multi-call) test.
SECS=${1:-18}
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1
rm -f /tmp/nsh_ser.log /tmp/nsh_mon.log
type_str() {
  local s="$1"
  for ((i=0; i<${#s}; i++)); do
    ch="${s:$i:1}"
    case "$ch" in " ") key="spc" ;; "/" ) key="slash" ;; "." ) key="dot" ;;
      "-" ) key="minus" ;; [A-Z]) key="shift-${ch,,}" ;; *) key="$ch" ;; esac
    echo "sendkey $key"; sleep 0.12
  done
}
(
  sleep "$SECS"
  type_str "elf /bin/echo hello from nsh"; echo "sendkey ret"; sleep 5
  type_str "elf /bin/uname";              echo "sendkey ret"; sleep 4
  type_str "elf /bin/ls /";               echo "sendkey ret"; sleep 6
  type_str "elf /bin/ls /bin";            echo "sendkey ret"; sleep 6
  type_str "elf /bin/cat /hello.txt";     echo "sendkey ret"; sleep 5
  echo "xp /4096bx 0xb8000"
  sleep 2
  echo quit
) | timeout $((SECS+80)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio \
    -serial file:/tmp/nsh_ser.log > /tmp/nsh_mon.log 2>&1

grep '^[0-9a-f]\{16\}: ' /tmp/nsh_mon.log | sed 's/^[^:]*: //; s/0x//g' \
  | tr -d ' \r\n' | xxd -r -p 2>/dev/null \
  | perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
      print $c if $c =~ /[\x20-\x7e]/ }' > /tmp/nsh_vga.txt

echo "--- VGA tail ---"
fold -w 80 /tmp/nsh_vga.txt | grep . | tail -26
