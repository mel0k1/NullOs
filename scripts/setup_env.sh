#!/bin/bash
# setup_env.sh — восстановление QEMU-окружения песочницы без root.
# Рецепт из worklog-сессий 20-22: Debian trixie debs → dpkg -x → /home/z/qemu-root.
# ВАЖНО: qemu-system-data ставит bios-256k.bin/vgabios как СИМЛИНКИ — песочница их
# не создаёт → ROM отсутствует → «could not load PC BIOS». Лечение: копировать
# реальные файлы из debs seabios/vgabios/ipxe-qemu (cp -L) в usr/share/qemu.
set -u
ROOT=/home/z/qemu-root
DL=/home/z/qemu-debs
mkdir -p "$ROOT" "$DL"
cd "$DL"

WANT="qemu-system-x86 qemu-utils xorriso seabios vgabios ipxe-qemu"

echo "[1/4] Вычисляю недостающие пакеты (рекурсивное замыкание без recommends)..."
PKGS=$(apt-cache depends --recurse --no-recommends --no-suggests \
  --no-conflicts --no-breaks --no-replaces --no-enhances \
  $WANT 2>/dev/null | grep '^\w' | sort -u | \
  while read -r p; do
    dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" || echo "$p"
  done)
echo "    недостает: $(echo "$PKGS" | wc -w) пакетов"

echo "[2/4] Скачиваю debs..."
# shellcheck disable=SC2086
apt-get download $PKGS 2>&1 | grep -v "^Get\|^Fetched\|^Reading\|^E: Ignoring" || true
echo "    скачано: $(ls -1 *.deb 2>/dev/null | wc -l) debs"

echo "[3/4] Распаковываю в $ROOT..."
for f in *.deb; do dpkg -x "$f" "$ROOT"; done

echo "[4/4] Копирую ROM-ы поверх симлинков (cp -L,sandbox)..."
Q="$ROOT/usr/share/qemu"
mkdir -p "$Q"
cp -L "$ROOT"/usr/share/seabios/bios-256k.bin "$Q/bios-256k.bin" 2>/dev/null || echo "    WARN: bios-256k.bin не найден"
cp -L "$ROOT"/usr/share/seabios/bios.bin "$Q/bios.bin" 2>/dev/null || true
cp -L "$ROOT"/usr/share/vgabios/vgabios.bin "$Q/vgabios.bin" 2>/dev/null || echo "    WARN: vgabios.bin не найден"
cp -L "$ROOT"/usr/share/vgabios/vgabios-*.bin "$Q/" 2>/dev/null || true
cp -L "$ROOT"/usr/share/ipxe/qemu/*.rom "$Q/" 2>/dev/null || echo "    WARN: ipxe ROMs не найдены"
cp -L "$ROOT"/usr/share/qemu/*.bin "$Q/" 2>/dev/null || true
cp -L "$ROOT"/usr/share/qemu/*.fd "$Q/" 2>/dev/null || true

# env.sh для последующих сессий/скриптов
cat > /home/z/my-project/scripts/env.sh <<'EOF'
export QEMU_ROOT=/home/z/qemu-root
export LD_LIBRARY_PATH="$QEMU_ROOT/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$QEMU_ROOT/usr/bin:$PATH"
EOF

echo "=== Проверка ==="
. /home/z/my-project/scripts/env.sh
qemu-system-x86_64 --version | head -1
xorriso --version 2>/dev/null | head -2 || echo "xorriso: см. ниже"
ls "$Q"/bios-256k.bin "$Q"/vgabios.bin >/dev/null 2>&1 && echo "ROM-ы: OK" || echo "ROM-ы: ПРОБЛЕМА"
