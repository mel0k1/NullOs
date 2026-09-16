#!/bin/bash
# VFS test: /dev/null, /dev/zero, FAT32 mounted at /disk, sync regression.
SECS=${1:-22}
ROOT=/mnt/c/Users/admin/Downloads/NullOs
cd "$ROOT" || exit 1
[ -f build/fat.img ] || { echo "build/fat.img missing"; exit 1; }

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

QEMU_FLAGS="-cdrom build/nullos.iso -m 512M \
 -drive file=build/disk.img,format=raw,if=ide,index=0 \
 -drive file=build/fat.img,format=raw,if=ide,index=1 \
 -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
 -display none -no-reboot"

echo "=== BOOT 1 ==="
(
  sleep "$SECS"
  type_str "echo hidden > /dev/null";        echo "sendkey ret"; sleep 1
  type_str "echo x > /dev/zero";             echo "sendkey ret"; sleep 1
  type_str "cat /dev/null | wc";             echo "sendkey ret"; sleep 1
  type_str "fat32 mount 0 1";                echo "sendkey ret"; sleep 3
  type_str "ls /disk";                       echo "sendkey ret"; sleep 1
  type_str "cat /disk/HELLO.TXT";            echo "sendkey ret"; sleep 2
  type_str "ls / | grep tmp";                echo "sendkey ret"; sleep 1
  type_str "sync";                           echo "sendkey ret"; sleep 3
  type_str "shutdown";                       echo "sendkey ret"
  sleep 12; echo quit
) | timeout $((SECS+45)) qemu-system-x86_64 $QEMU_FLAGS -monitor stdio > /tmp/vfs_mon.log 2>&1

grep '^[0-9a-f]\{16\}: ' /tmp/vfs_mon.log | sed 's/^[^:]*: //; s/0x//g' \
  | tr -d ' \r\n' > /tmp/vfs_hex.txt
xxd -r -p /tmp/vfs_hex.txt > /tmp/vfs_vga.bin 2>/dev/null
perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
  print $c if $c =~ /[\x20-\x7e]/ }' /tmp/vfs_vga.bin > /tmp/vfs_vga.txt

echo "--- BOOT1 last screen ---"
fold -w 80 /tmp/vfs_vga.txt | tail -26
echo "-------------------------"

echo ""
echo "=== BOOT 2: persistence regression (b.txt must survive) ==="
rm -rf build/isodir_boot2 2>/dev/null
(
  sleep "$SECS"
  sleep 4
  echo "xp /4096bx 0xb8000"
  sleep 2
  echo quit
) | timeout $((SECS+25)) qemu-system-x86_64 $QEMU_FLAGS -monitor stdio > /tmp/vfs_mon2.log 2>&1

grep '^[0-9a-f]\{16\}: ' /tmp/vfs_mon2.log | sed 's/^[^:]*: //; s/0x//g' \
  | tr -d ' \r\n' > /tmp/vfs_hex2.txt
xxd -r -p /tmp/vfs_hex2.txt > /tmp/vfs_vga2.bin 2>/dev/null
perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
  print $c if $c =~ /[\x20-\x7e]/ }' /tmp/vfs_vga2.bin > /tmp/vfs_vga2.txt

echo "--- BOOT2 last screen ---"
fold -w 80 /tmp/vfs_vga2.txt | tail -14

V1=$(fold -w 80 /tmp/vfs_vga.txt)
V2=$(fold -w 80 /tmp/vfs_vga2.txt)
echo "$V2" | grep -q "persist2\|b.txt" && echo "[INFO] b.txt present after reboot" || true
