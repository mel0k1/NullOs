#!/usr/bin/env python3
"""HMP monitor client: dump physical + virtual memory views."""
import socket, time, sys

class Mon:
    def __init__(self, path='/tmp/qmon.sock'):
        for _ in range(20):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.3)
        else:
            raise RuntimeError('monitor socket never appeared')
        self.s.settimeout(1)
        self.drain(1.5)

    def drain(self, secs):
        end = time.time() + secs
        data = b''
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                data += d
                if not d:
                    break
            except socket.timeout:
                break
        return data

    def cmd(self, c, wait=1.2):
        self.s.sendall(c.encode() + b'\n')
        return self.drain(wait)

def main():
    m = Mon()
    print('--- xp /8gx 0x3e0000 (PHYSICAL code page) ---')
    print(m.cmd('xp /8gx 0x3e0000', 2).decode(errors='replace'))
    print('--- x /8gx 0x400000 (VIRTUAL via current CR3) ---')
    print(m.cmd('x /8gx 0x400000', 2).decode(errors='replace'))
    print('--- xp /8gx 0x401000 (rodata phys?) ---')
    print(m.cmd('xp /8gx 0x401000', 2).decode(errors='replace'))
    print('--- info registers ---')
    print(m.cmd('info registers', 1.5).decode(errors='replace')[:800])

if __name__ == '__main__':
    main()
