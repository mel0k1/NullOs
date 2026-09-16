#!/bin/bash
# Pipe/redirection/filter interactive test via QEMU monitor sendkey.
SECS=${1:-20}
ROOT=/mnt/c/Users/admin/Downloads/NullOs
cd "$ROOT" || exit 1

type_str() {
  local s="$1"
  for ((i=0; i<${#s}; i++)); do
    ch="${s:$i:1}"
    case "$ch" in
      " ") key="spc" ;;
      ">") key="shift-dot" ;;
      "|") key="shift-backslash" ;;
      "_") key="shift-minus" ;;
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
  type_str "echo alpha beta | wc";            echo "sendkey ret"; sleep 1
  type_str "echo alpha beta | grep beta";     echo "sendkey ret"; sleep 1
  type_str "echo alpha beta | grep -v beta";  echo "sendkey ret"; sleep 1
  type_str "help | head -n 3";                echo "sendkey ret"; sleep 1
  type_str "help | tail -n 3";                echo "sendkey ret"; sleep 1
  type_str "ls /test_dir | cat";              echo "sendkey ret"; sleep 1
  type_str "cat /hello.txt | wc";             echo "sendkey ret"; sleep 1
  type_str "echo one | echo two";             echo "sendkey ret"; sleep 1
  sleep 3
  echo "xp /4096bx 0xb8000"
  sleep 2
  echo quit
) | timeout $((SECS+40)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio > /tmp/pipe_mon.log 2>&1

grep '^[0-9a-f]\{16\}: ' /tmp/pipe_mon.log | sed 's/^[^:]*: //; s/0x//g' \
  | tr -d ' \r\n' > /tmp/pipe_hex.txt
xxd -r -p /tmp/pipe_hex.txt > /tmp/pipe_vga.bin 2>/dev/null
perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
  print $c if $c =~ /[\x20-\x7e]/ }' /tmp/pipe_vga.bin > /tmp/pipe_vga.txt

fold -w 80 /tmp/pipe_vga.txt | tail -30
