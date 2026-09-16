#!/bin/bash
# build_iso.sh — подмена kernel.bin ВНУТРЬ известно-рабочего ISO (рецепт Task 20+:
# xorriso -indev/-outdev -boot_image any replay — El Torito цепочка сохраняется).
# Использование: build_iso.sh [путь-к-дереву-NullOs] (по умолчанию nullos-work)
set -eu
TREE=${1:-/home/z/my-project/nullos-work/NullOs}
ISO="$TREE/build/nullos.iso"
KRN="$TREE/build/kernel.bin"
OUT="$TREE/build/nullos.new.iso"
. /home/z/my-project/scripts/env.sh

[ -f "$ISO" ] || { echo "ERROR: базовый ISO не найден: $ISO"; exit 1; }
[ -f "$KRN" ] || { echo "ERROR: kernel.bin не найден: $KRN"; exit 1; }

rm -f "$OUT"
xorriso -osirrox off \
  -indev "$ISO" \
  -outdev "$OUT" \
  -boot_image any replay \
  -update "$KRN" /boot/kernel.bin \
  -commit 2>&1 | grep -E "Update|Writing|completed|libisofs" | head -8

# атомарная подмена
mv -f "$OUT" "$ISO"

echo "=== Проверка ==="
xorriso -osirrox on -indev "$ISO" -extract /boot/kernel.bin /tmp/_chk_kernel.bin 2>/dev/null
if cmp -s /tmp/_chk_kernel.bin "$KRN"; then
  echo "[OK] kernel.bin в ISO = свежесобранный ($(md5sum "$KRN" | cut -d' ' -f1))"
else
  echo "[FAIL] kernel.bin в ISO НЕ совпадает!" ; exit 1
fi
