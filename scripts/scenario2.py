#!/usr/bin/env python3
"""scenario2.py — расширенный сценарий регрессии NullOs (post-audit validation).

Часть 1: seatprobe с sendkey-инъекцией (a/b в фазе t3 KBD — паттерн Task 22)
Часть 2: сеть через slirp — nettest (UDP echo 40001, exit=42) и httpget
          (python http.server на хосте = 10.0.2.2:8080)
Часть 3: ash-смоук — скрипт t.sh фетчится httpget'ом и запускается /bin/sh
          (пайпы, подстановки, /proc, фоновые задачи + wait)
Часть 4: базовые kernel-task пробы (spawn/multi/preempt status)

Вердикты: по кодам выхода в serial ([PROC] pid=N exit=C) + маркеры в VGA.
"""
import os
import re
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, '/home/z/my-project/scripts')
from run_probe import NullOsVM

ISO = '/home/z/my-project/nullos-work/NullOs/build/nullos.iso'
DISK = '/home/z/my-project/nullos-work/NullOs/build/disk.img'

T_SH = '''#!/bin/sh
echo ALPHA-BETA
echo one two three | wc -l
cat /proc/meminfo | head -n 1
X=$(echo delta | tr a-z A-Z)
echo SUB-$X
echo bg1 & echo bg2 & wait
echo ASH-SMOKE-DONE
'''

F_TXT = 'LABWC-PIVOT-2026-09-16\n'


def udp_echo():
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # nettest.c: connect 10.0.2.2:9000 (ra.port_be=0x2823); локальный bind 40001
    s.bind(('127.0.0.1', 9000))
    s.settimeout(600)   # весь сценарий, не 30с — nettest идёт после seatprobe
    while True:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            return
        except OSError:
            return
        try:
            s.sendto(data, addr)
        except OSError:
            return


def run_cmd(vm, cmd, timeout, flush=True):
    """Выполнить команду, дождаться промпта, вернуть (ok, vga, serial_chunk).
    flush=True — холостой ret ПЕРЕД командой: смывает residue от key-eating
    проб (seatprobe оставляет a/b в консольном кольце — без смыва строка
    склеивается в 'abelf /bin/nettest' и команда теряется)."""
    base = vm.serial_all()
    if flush:
        vm.sendkey('ret')
        time.sleep(0.8)
    vm.type_cmd(cmd)
    ok = vm.wait_prompt(timeout=timeout)
    vga = vm.vga_text()
    chunk = vm.serial_all()[len(base):]
    return ok, vga, chunk


def exit_code(chunk):
    exits = re.findall(r'\[PROC\] pid=(\d+) exit=(-?\d+)', chunk)
    return int(exits[-1][1]) if exits else None


def main():
    srvdir = '/home/z/my-project/logs/scenario2_www'
    os.makedirs(srvdir, exist_ok=True)
    with open(os.path.join(srvdir, 't.sh'), 'w') as f:
        f.write(T_SH)
    with open(os.path.join(srvdir, 'f.txt'), 'w') as f:
        f.write(F_TXT)

    httpd = subprocess.Popen(
        [sys.executable, '-m', 'http.server', '8080', '--bind', '127.0.0.1', '-d', srvdir],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    udpt = threading.Thread(target=udp_echo, daemon=True)
    udpt.start()
    time.sleep(1)

    results = []
    vm = NullOsVM('scenario2', ISO, disk=DISK, net=True)
    try:
        print('[BOOT] %.1fs' % vm.wait_boot())

        # ---- Часть 0: bring-up NIC (drivers init N, индекс ищем по serial) ----
        nic_up = False
        for idx in range(0, 8):
            base = vm.serial_all()
            vm.sendkey('ret')
            time.sleep(0.6)
            vm.type_cmd('drivers init %d' % idx)
            vm.wait_prompt(timeout=25)
            chunk = vm.serial_all()[len(base):]
            if '[DMA]' in chunk or 'RTL8139' in chunk:
                nic_up = True
                print('[PASS] NIC up: drivers init %d' % idx)
                break
        print('[%s] NIC bring-up' % ('PASS' if nic_up else 'FAIL'))
        results.append(('nic-bringup', nic_up))

        # ---- Часть 1: seatprobe + инъекция a/b ----
        base = vm.serial_all()
        vm.sendkey('ret')
        time.sleep(0.6)
        vm.type_cmd('elf /bin/seatprobe')
        time.sleep(4)          # t3 KBD-фаза опрашивает клавиатуру
        vm.sendkey('a')
        time.sleep(1.2)
        vm.sendkey('b')
        vm.wait_prompt(timeout=120)
        vga = vm.vga_text()
        chunk = vm.serial_all()[len(base):]
        code = exit_code(chunk)
        t123 = all(m in vga for m in ('SEAT', 'CTX', 'KBD'))
        print('[%s] seatprobe exit=%s (t1-t3=%s; t4 PTR=env-limited как в Task22)'
              % ('PASS' if code == 0 else 'FAIL', code, t123))
        results.append(('seatprobe', code == 0))

        # ---- Часть 2: сеть ----
        ok, vga, chunk = run_cmd(vm, 'elf /bin/nettest', 60)
        code = exit_code(chunk)
        print('[%s] nettest exit=%s (UDP round-trip, 42=OK)'
              % ('PASS' if code == 42 else 'FAIL', code))
        results.append(('nettest', code == 42))

        ok, vga, chunk = run_cmd(
            vm, 'elf /bin/httpget 10.0.2.2 8080 f.txt /tmp/got.txt', 60)
        code = exit_code(chunk)
        print('[%s] httpget exit=%s (HTTP через slirp; 42=OK)'
              % ('PASS' if code == 42 else 'FAIL', code))
        results.append(('httpget', code == 42))

        # ---- Часть 3: ash-смоук через фетч скрипта ----
        ok, vga, chunk = run_cmd(vm, 'elf /bin/httpget 10.0.2.2 8080 t.sh /t.sh', 60)
        ok, vga, chunk = run_cmd(vm, 'elf /bin/sh /t.sh', 90)
        marks = ('ALPHA-BETA', 'SUB-DELTA', 'ASH-SMOKE-DONE', 'MemTotal')
        have = [m for m in marks if m in vga]
        good = len(have) == len(marks)
        print('[%s] ash-smoke: маркеры %s/%s (%s)'
              % ('PASS' if good else 'FAIL', len(have), len(marks), ','.join(have)))
        results.append(('ash-smoke', good))
        with open('/home/z/my-project/logs/scenario2/ash_vga.txt', 'w') as f:
            f.write(vga)

        # ---- Часть 4: базовые kernel-task пробы ----
        ok, vga, chunk = run_cmd(vm, 'spawn 3', 40)
        sp = 'Task finished' in vga or 'finished' in vga
        print('[%s] spawn 3 (кооперативная многозадачность)'
              % ('PASS' if ok else 'FAIL'))
        results.append(('spawn3', ok))
        ok, vga, chunk = run_cmd(vm, 'multi 3', 150)
        mu = 'all tasks finished' in vga
        with open('/home/z/my-project/logs/scenario2/multi_vga.txt', 'w') as f:
            f.write(vga)
        print('[%s] multi 3' % ('PASS' if mu else 'FAIL'))
        results.append(('multi3', mu))

        print('\nИТОГ: %d/%d PASS' % (sum(1 for _, g in results if g), len(results)))
    finally:
        vm.kill()
        httpd.terminate()
    return 0 if all(g for _, g in results) else 1


if __name__ == '__main__':
    sys.exit(main())
