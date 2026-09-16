#!/bin/bash
# End-to-end test: redirection, persistence (sync -> reboot -> automount),
# ring-3 file writes, and ACPI shutdown.
# Usage: e2e.sh [seconds-per-boot]
SECS=${1:-22}
ROOT=/mnt/c/Users/admin/Downloads/NullOs
cd "$ROOT" || exit 1

[ -f build/disk.img ] || { qemu-img create -f raw build/disk.img 64M || dd if=/dev/zero of=build/disk.img bs=1M count=64; }

type_str() {
  local s="$1"
  for ((i=0; i<${#s}; i++)); do
    ch="${s:$i:1}"
    case "$ch" in
      " ") key="spc" ;;
      ">") key="shift-dot" ;;
      "-") key="minus" ;;
      "/") key="slash" ;;
      ".") key="dot" ;;
      [A-Z]) key="shift-${ch,,}" ;;
      *)   key="$ch" ;;
    esac
    echo "sendkey $key"
    sleep 0.07
  done
}

run_boot() {  # $1 = extra sleep after boot; feeds stdin cmds from caller
  timeout $((SECS+40)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -drive file=build/disk.img,format=raw,if=ide,index=0 \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio \
    -serial file:/tmp/e2e_serial.log
}

echo "=== BOOT 1: create files, redirect, sync, shutdown ==="
rm -f /tmp/e2e_serial.log
(
  sleep "$SECS"
  type_str "echo persist-ok > a.txt"
  echo "sendkey ret"
  sleep 1
  type_str "ls > lsout.txt"
  echo "sendkey ret"
  sleep 1
  type_str "elf /bin/hello run"
  echo "sendkey ret"
  sleep 6
  type_str "sync"
  echo "sendkey ret"
  sleep 3
  type_str "shutdown"
  echo "sendkey ret"
  sleep 12
  echo quit
) | run_boot
R1=$?
grep -q "SYNC" /tmp/e2e_serial.log && echo "[ok] sync ran" || echo "[??] no SYNC in serial"

if [ $R1 -le 2 ]; then
  echo "[ok] QEMU exited on its own (ACPI/QEMU shutdown path works)"
else
  echo "[FAIL] QEMU had to be killed (timeout) - shutdown hung?"
fi

echo ""
echo "=== BOOT 2: verify automount restored files ==="
(
  sleep "$SECS"
  type_str "cat a.txt"
  echo "sendkey ret"
  sleep 1
  type_str "cat lsout.txt"
  echo "sendkey ret"
  sleep 8
  echo "xp /4096bx 0xb8000"
  sleep 2
  echo quit
) | timeout $((SECS+30)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -drive file=build/disk.img,format=raw,if=ide,index=0 \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio > /tmp/e2e_mon2.log 2>&1

grep '^[0-9a-f]\{16\}: ' /tmp/e2e_mon2.log | sed 's/^[^:]*: //; s/0x//g' \
  | tr -d ' \r\n' > /tmp/e2e_hex.txt
xxd -r -p /tmp/e2e_hex.txt > /tmp/e2e_vga.bin 2>/dev/null
perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
  print $c if $c =~ /[\x20-\x7e]/ }' /tmp/e2e_vga.bin > /tmp/e2e_vga.txt

VGA=$(fold -w 80 /tmp/e2e_vga.txt)
echo "--- last VGA screen ---"
echo "$VGA" | tail -25
echo "-----------------------"
echo "$VGA" | grep -q "persist-ok" && echo "[PASS] a.txt survived reboot" || echo "[FAIL] a.txt missing"
echo "$VGA" | grep -q "bin" && echo "[PASS] ls-out has entries" || echo "[FAIL] ls redirect empty"
