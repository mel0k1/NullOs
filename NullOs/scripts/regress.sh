#!/bin/bash
# Final regression: boot, cooperative spawn x2, pipes, persistence round-trip.
SECS=${1:-20}
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1

type_str() {
  local s="$1"
  for ((i=0; i<${#s}; i++)); do
    ch="${s:$i:1}"
    case "$ch" in
      " ") key="spc" ;; ">") key="shift-dot" ;; "|") key="shift-backslash" ;;
      "_") key="shift-minus" ;; "-") key="minus" ;; "/") key="slash" ;;
      ".") key="dot" ;;
      [A-Z]) key="shift-${ch,,}" ;;
      *)   key="$ch" ;;
    esac
    echo "sendkey $key"; sleep 0.06
  done
}

echo "=== BOOT 1: functional pass ==="
rm -f /tmp/reg_ser.log /tmp/reg_mon.log
(
  sleep "$SECS"
  type_str "spawn 3";            echo "sendkey ret"; sleep 4
  type_str "echo hi pipe | wc";  echo "sendkey ret"; sleep 2
  type_str "preempt status";     echo "sendkey ret"; sleep 1
  type_str "multi 3";            echo "sendkey ret"; sleep 14
  type_str "echo persist > keep.txt"; echo "sendkey ret"; sleep 1
  type_str "sync";               echo "sendkey ret"; sleep 3
  echo "xp /4096bx 0xb8000"
  sleep 2
  type_str "shutdown";           echo "sendkey ret"
  sleep 12; echo quit
) | timeout $((SECS+60)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -drive file=build/disk.img,format=raw,if=ide,index=0 \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio \
    -serial file:/tmp/reg_ser.log > /tmp/reg_mon.log 2>&1

decode() {
  grep '^[0-9a-f]\{16\}: ' "$1" | sed 's/^[^:]*: //; s/0x//g' \
    | tr -d ' \r\n' | xxd -r -p 2>/dev/null \
    | perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
        print $c if $c =~ /[\x20-\x7e]/ }'
}
decode /tmp/reg_mon.log > /tmp/reg_vga.txt
V=$(fold -w 80 /tmp/reg_vga.txt)
echo "$V" | tail -22
echo "---- checks ----"
echo "$V" | grep -q "Task finished"       && echo "[PASS] cooperative spawn completes"
echo "$V" | grep -Eq "[0-9]+ +[0-9]+ +[0-9]+" && echo "[PASS] pipe wc works"
echo "$V" | grep -q "all tasks finished"  && echo "[PASS] multi completes"
echo "$V" | grep -q "SYNC"                && echo "[PASS] sync ran"
grep -a "EXIT] code=42" /tmp/reg_ser.log >/dev/null && echo "[PASS] hello ring3 ok" || true

echo ""
echo "=== BOOT 2: persistence ==="
(
  sleep "$SECS"
  type_str "cat keep.txt"; echo "sendkey ret"; sleep 3
  echo "xp /4096bx 0xb8000"
  sleep 2
  echo quit
) | timeout $((SECS+30)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -drive file=build/disk.img,format=raw,if=ide,index=0 \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio >/tmp/reg_mon2.log 2>&1
decode /tmp/reg_mon2.log > /tmp/reg_vga2.txt
V2=$(fold -w 80 /tmp/reg_vga2.txt)
echo "$V2" | tail -6
echo "$V2" | grep -q "persist" && echo "[PASS] files survive reboot" || echo "[FAIL] persistence broken"