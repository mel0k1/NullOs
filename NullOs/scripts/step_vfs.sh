#!/bin/bash
# Step-by-step VFS test: VGA snapshot after EACH command to find crashes.
SECS=${1:-20}
cd /mnt/c/Users/admin/Downloads/NullOs || exit 1

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

dump_vga() {
  echo "xp /4096bx 0xb8000"
  sleep 1
}

CMDS=(
  "echo hidden > /dev/null"
  "echo x > /dev/zero"
  "cat /dev/null | wc"
  "fat32 mount 0 1"
  "ls /disk"
  "cat /disk/HELLO.TXT"
)

(
  sleep "$SECS"
  i=0
  for c in "${CMDS[@]}"; do
    i=$((i+1))
    echo "echo STEP$i"
    echo "sendkey ret"
    sleep 0.5
    type_str "$c"
    echo "sendkey ret"
    sleep 3
    dump_vga
  done
  sleep 1
  echo quit
) | timeout $((SECS+120)) qemu-system-x86_64 \
    -cdrom build/nullos.iso -m 512M \
    -drive file=build/disk.img,format=raw,if=ide,index=0 \
    -drive file=build/fat.img,format=raw,if=ide,index=1 \
    -cpu qemu64,+sse,+sse2,+sse3,+ssse3 \
    -display none -no-reboot -monitor stdio \
    -serial file:/tmp/step_ser.log > /tmp/step_mon.log 2>&1

# Split monitor log into per-dump files and decode like smoke.sh
rm -f /tmp/step_blk_*.txt
awk '/xp \/4096bx/{n++} n>0{print > ("/tmp/step_blk_" n ".txt")}' /tmp/step_mon.log
for f in /tmp/step_blk_*.txt; do
  n=$(basename "$f" | sed 's/.*_//')
  echo "===== DUMP $n ====="
  grep '^[0-9a-f]\{16\}: ' "$f" | sed 's/^[^:]*: //; s/0x//g' \
    | tr -d ' \r\n' | xxd -r -p 2>/dev/null \
    | perl -ne 'for (my $i=0; $i+1<length($_); $i+=2) { my $c=substr($_,$i,1);
        print $c if $c =~ /[\x20-\x7e]/ }' \
    | fold -w 80 | grep . | tail -6
done
echo "=== serial tail ==="
tail -6 /tmp/step_ser.log