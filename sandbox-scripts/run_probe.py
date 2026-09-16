#!/usr/bin/env python3
"""run_probe.py — драйвер QEMU-прогонов NullOs.

Рецепт из worklog Task 20-22:
  - monitor ТОЛЬКО unix-сокетом (stdio-pipe ломает boot: stdin-EOF убивает QEMU)
  - HMP-вывод приходит НА СОКЕТ — drain после каждой команды
  - sendkey: пробел = 'spc'; регистр через 'shift-x'; спец-символы по карте
  - VGA-консоль читается HMP `xp /4096bx 0xb8000` (чётные байты = символы)
  - коды выхода процессов — serial-лог: [PROC] reap pid=%d code=%d

Использование (сценарий батареи):
  python3 run_probe.py battery --iso .../nullos.iso --disk .../disk.img --tag name
  python3 run_probe.py custom --iso ... --cmds "elf /bin/wltest" "elf /bin/unixtest"
"""
import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import time

VGA_BASE = 0xB8000
VGA_LEN = 4096
PROMPT = "]$ "

SENDKEY_MAP = {
    ' ': 'spc', '\t': 'tab', '\n': 'ret', '.': 'dot', ',': 'comma',
    '-': 'minus', '=': 'equal', '/': 'slash', '\\': 'backslash',
    ';': 'semicolon', "'": 'apostrophe', '[': 'bracket_left',
    ']': 'bracket_right', '`': 'grave_accent',
}
SHIFT_MAP = {
    '!': 'shift-1', '@': 'shift-2', '#': 'shift-3', '$': 'shift-4',
    '%': 'shift-5', '^': 'shift-6', '&': 'shift-7', '*': 'shift-8',
    '(': 'shift-9', ')': 'shift-0',
    '>': 'shift-dot', '<': 'shift-comma', '_': 'shift-minus',
    '+': 'shift-equal', '?': 'shift-slash', '|': 'shift-backslash',
    ':': 'shift-semicolon', '"': 'shift-apostrophe',
    '{': 'shift-bracket_left', '}': 'shift-bracket_right', '~': 'shift-grave_accent',
}


def key_for(ch):
    if ch in SENDKEY_MAP:
        return SENDKEY_MAP[ch]
    if ch in SHIFT_MAP:
        return SHIFT_MAP[ch]
    if ch.isupper():
        return 'shift-' + ch.lower()
    return ch


class HMPClient:
    def __init__(self, path, timeout=180):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise RuntimeError('monitor socket never appeared')
                time.sleep(0.3)
        self.s.settimeout(0.4)
        self.drain(1.0)

    def drain(self, secs):
        end = time.time() + secs
        data = b''
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if not d:
                    break
                data += d
            except socket.timeout:
                pass
        return data

    def cmd(self, c, wait=1.0):
        self.s.sendall(c.encode() + b'\n')
        return self.drain(wait).decode(errors='replace')


class NullOsVM:
    def __init__(self, tag, iso, disk=None, net=False, logs_dir='/home/z/my-project/logs'):
        self.dir = os.path.join(logs_dir, tag)
        os.makedirs(self.dir, exist_ok=True)
        self.serial_path = os.path.join(self.dir, 'serial.log')
        self.hmp_path = os.path.join(self.dir, 'hmp.sock')
        self.vga_path = os.path.join(self.dir, 'vga.txt')
        cmd = ['qemu-system-x86_64',
               '-L', '/home/z/qemu-root/usr/share/qemu/',
               '-m', '512M',
               '-cdrom', iso, '-boot', 'd',
               '-cpu', 'qemu64,+sse,+sse2,+sse3,+ssse3',
               '-display', 'none', '-no-reboot',
               '-serial', 'file:' + self.serial_path,
               '-monitor', 'unix:' + self.hmp_path + ',server,nowait']
        if disk:
            cmd += ['-drive', 'file=%s,format=raw,if=ide,index=0' % disk]
        if net:
            # slirp: 10.0.2.2 = хост (для httpget-феча с локального python-сервера)
            cmd += ['-netdev', 'user,id=n0',
                    '-device', 'rtl8139,netdev=n0']
        else:
            cmd += ['-nic', 'none']
        self.log = open(os.path.join(self.dir, 'qemu.log'), 'w')
        self.proc = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT)
        self.mon = HMPClient(self.hmp_path)
        self._serial_off = 0
        self._vga_hist = []

    # ---------- serial ----------
    def serial_all(self):
        try:
            with open(self.serial_path, 'rb') as f:
                return f.read().decode(errors='replace')
        except FileNotFoundError:
            return ''

    def serial_new(self):
        data = self.serial_all()
        new = data[self._serial_off:]
        self._serial_off = len(data)
        return new

    def wait_serial(self, pattern, timeout=30, poll=0.5):
        rx = re.compile(pattern)
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = rx.search(self.serial_all())
            if m:
                return m
            time.sleep(poll)
        return None

    # ---------- vga ----------
    def vga_raw(self):
        out = self.mon.cmd('xp /%dbx 0x%x' % (VGA_LEN, VGA_BASE), wait=2.5)
        return out

    def vga_text(self):
        raw = self.vga_raw()
        chars = []
        for line in raw.splitlines():
            if ':' not in line:
                continue
            tail = line.split(':', 1)[1]
            tokens = re.findall(r'0x[0-9a-fA-F]{1,2}|(?<!\w)[0-9a-fA-F]{2}(?!\w)', tail)
            for i, tok in enumerate(tokens):
                if i % 2 == 0:  # атрибуты на нечётных
                    b = int(tok, 16) if not tok.startswith('0x') else int(tok, 16)
                    if 0x20 <= b <= 0x7e:
                        chars.append(chr(b))
                    else:
                        chars.append('\n' if b == 0 else ' ')
        text = ''.join(chars)
        # сжатие повторных переводов строки
        text = re.sub(r'\n{2,}', '\n', text)
        return text

    def vga_dump(self):
        t = self.vga_text()
        with open(self.vga_path, 'w') as f:
            f.write(t)
        return t

    # ---------- ввод ----------
    def type_cmd(self, s, settle=0.045):
        for ch in s:
            k = key_for(ch)
            if k is None:
                continue
            self.mon.cmd('sendkey %s' % k, wait=settle)
        self.mon.cmd('sendkey ret', wait=0.15)

    def sendkey(self, k):
        self.mon.cmd('sendkey %s' % k, wait=0.2)

    # ---------- жизненный цикл ----------
    def wait_boot(self, timeout=120):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.proc.poll() is not None:
                raise RuntimeError('QEMU умер при буте, rc=%s' % self.proc.returncode)
            t = self.vga_text()
            if PROMPT in t[-400:]:
                return time.time() - t0
            time.sleep(2)
        raise RuntimeError('boot timeout: промпт не появился')

    def wait_prompt(self, timeout=90):
        # ждём ВОЗВРАЩЕНИЯ промпта (после команды): последний непустой хвост
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.proc.poll() is not None:
                return False
            t = self.vga_text()
            if PROMPT in t[-400:]:
                return True
            time.sleep(1.5)
        # modeset-пробы (drmtest) переинициализируют консоль БЕЗ перерисовки
        # промпта — тот печатается только по следующему вводу. Ткнём ret.
        self.mon.cmd('sendkey ret', wait=0.3)
        for _ in range(8):
            if self.proc.poll() is not None:
                return False
            t = self.vga_text()
            if PROMPT in t[-400:]:
                return True
            time.sleep(1.5)
        return False

    def kill(self):
        # ВАЖНО: QEMU file-serial буферизует — при SIGKILL хвост лога ТЕРЯЕТСЯ.
        # Сначала просим QEMU выйти честно (HMP quit = flush всех файлов).
        try:
            self.mon.cmd('quit', wait=1.0)
        except Exception:
            pass
        try:
            self.proc.wait(timeout=15)
            try:
                self.log.close()
            except Exception:
                pass
            return
        except Exception:
            pass
        try:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait(timeout=10)
        except Exception:
            pass
        try:
            self.log.close()
        except Exception:
            pass


# ---------------- батарея ----------------
BATTERY = [
    # (команда, таймаут, комментарий)
    ('elf /bin/hello', 25, 'ring3 ELF hello (code=42 в serial)'),
    ('elf /bin/wltest', 60, 'Wayland-субстрат: 7 стадий'),
    ('elf /bin/unixtest', 60, 'AF_UNIX+SCM_RIGHTS+epoll'),
    ('elf /bin/layertest', 90, 'mprotect W^X + /proc + /dev/shm + EFD_SEM'),
    ('elf /bin/drmtest', 60, 'DRM-lite modeset+dumb+pageflip'),
]


def run_battery(args):
    vm = NullOsVM(args.tag, args.iso, disk=args.disk, net=False)
    results = []
    try:
        bt = vm.wait_boot()
        print('[BOOT] OK за %.1fs' % bt)
        base_serial = vm.serial_all()
        for idx, (cmd, to, note) in enumerate(BATTERY):
            vm.type_cmd(cmd)
            ok = vm.wait_prompt(timeout=to)
            vga = vm.vga_dump()
            # снапшот VGA на каждую команду
            safe = cmd.split()[-1].replace('/', '_')
            with open(os.path.join(vm.dir, 'vga_%02d_%s.txt' % (idx, safe)), 'w') as f:
                f.write(vga)
            ser = vm.serial_all()
            chunk = ser[len(base_serial):]
            base_serial = ser
            # код выхода: elf-путь пишет [PROC] pid=N exit=C; wait4-путь — reap
            exits = re.findall(r'\[PROC\] pid=(\d+) exit=(-?\d+)', chunk)
            reaps = re.findall(r'\[PROC\] reap pid=(\d+) code=(-?\d+)', chunk)
            code = int(exits[-1][1]) if exits else (int(reaps[-1][1]) if reaps else None)
            verdict = 'PASS' if (ok and code == 0) else ('FAIL' if not ok else 'code=%s' % code)
            print('[%s] %-28s exit=%s  (%s)' % (verdict, cmd.split()[-1], code, note))
            results.append((cmd, verdict, code))
        print('\n=== VGA-консоль (последний дамп) ===')
        print(vm.vga_text()[-1200:])
    finally:
        vm.kill()
    return results


def run_custom(args):
    vm = NullOsVM(args.tag, args.iso, disk=args.disk, net=args.net)
    try:
        bt = vm.wait_boot()
        print('[BOOT] OK за %.1fs' % bt)
        for cmd in args.cmds:
            vm.type_cmd(cmd)
            vm.wait_prompt(timeout=120)
            time.sleep(1)
        time.sleep(3)
        print('=== VGA ===')
        print(vm.vga_dump()[-1500:])
        print('=== SERIAL (хвост) ===')
        print(vm.serial_all()[-2000:])
    finally:
        vm.kill()


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['battery', 'custom'])
    ap.add_argument('--tag', default='probe')
    ap.add_argument('--iso', required=True)
    ap.add_argument('--disk', default=None)
    ap.add_argument('--net', action='store_true')
    ap.add_argument('--cmds', nargs='*', default=[])
    a = ap.parse_args()
    if a.mode == 'battery':
        run_battery(a)
    else:
        run_custom(a)
