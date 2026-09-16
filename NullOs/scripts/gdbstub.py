#!/usr/bin/env python3
"""Minimal GDB remote-protocol client (robust byte-wise framing)."""
import socket, sys, time, functools
print = functools.partial(print, flush=True)

class GdbClient:
    def __init__(self, host='127.0.0.1', port=1234):
        last = None
        for attempt in range(20):
            try:
                self.s = socket.create_connection((host, port), timeout=180)
                break
            except ConnectionRefusedError as e:
                last = e
                time.sleep(0.5)
        else:
            raise last
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def _read_byte(self):
        b = self.s.recv(1)
        if not b:
            raise ConnectionError('qemu closed connection')
        return b[0]

    def _recv_packet(self):
        """Skip acks/noise, collect one $payload#cs packet."""
        while True:
            b = self._read_byte()
            while b != 0x24:                 # '$'
                # '+'/'-' acks are consumed silently
                b = self._read_byte()
            payload = bytearray()
            while True:
                b = self._read_byte()
                if b == 0x23:                # '#'
                    c1 = self._read_byte(); c2 = self._read_byte()
                    self.s.sendall(b'+')     # ACK
                    return bytes(payload).decode('latin1')
                payload.append(b)

    def cmd(self, payload):
        cs = sum(payload.encode()) & 0xFF
        pkt = b'$' + payload.encode() + ('#%02x' % cs).encode()
        self.s.sendall(pkt)
        # consume ack
        b = self._read_byte()
        while b == 0x2B or b == 0x2D:        # '+','-'
            b = self._read_byte()
        # b starts a packet? we already consumed '$'... re-collect rest
        payload_b = bytearray([b]) if b != 0x24 else bytearray()
        if b != 0x24:
            # we lost sync; fall back to full packet read
            return self._recv_packet_tail(payload_b)
        while True:
            c = self._read_byte()
            if c == 0x23:
                c1 = self._read_byte(); c2 = self._read_byte()
                self.s.sendall(b'+')
                return bytes(payload_b).decode('latin1')
            payload_b.append(c)

    def _recv_packet_tail(self, payload_b):
        while True:
            b = self._read_byte()
            if b == 0x23:
                self._read_byte(); self._read_byte()
                self.s.sendall(b'+')
                return bytes(payload_b).decode('latin1')
            payload_b.append(b)

    def read_regs(self):
        h = self.cmd('g')
        # x86-64: 16 GPRs (8B each = 256 hex chars), then rip (8B),
        # then eflags/cs/ss/... as 4B fields.
        names = ['rax','rbx','rcx','rdx','rsi','rdi','rbp','rsp',
                 'r8','r9','r10','r11','r12','r13','r14','r15']
        d = {}
        for i, n in enumerate(names):
            raw = bytes.fromhex(h[i*16:(i+1)*16])
            d[n] = int.from_bytes(raw, 'little')
        d['rip'] = int.from_bytes(bytes.fromhex(h[256:272]), 'little')
        try:
            d['eflags'] = int.from_bytes(bytes.fromhex(h[272:280]), 'little')
        except ValueError:
            pass
        return d

    def read_mem(self, addr, length):
        out = bytes()
        while length > 0:
            n = min(length, 100)
            r = self.cmd('m%x,%x' % (addr, length))
            if r.startswith('E'):
                return None
            out += bytes.fromhex(r[:n*2])
            addr += n; length -= n
        return out

    def step(self):
        return self.cmd('s')

    def cont(self):
        return self.cmd('c')

    def kill(self):
        try:
            self.cmd('k')
        except Exception:
            pass


def main():
    ADDR = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0
    CODE_PA = 0x3e0000                  # physical page holding user code

    c = GdbClient()
    print('[*] connected')
    time.sleep(0.2)
    c.cmd('?')

    if ADDR:
        print('[*] Z0 @%#x ->' % ADDR, c.cmd('Z0,%x,1' % ADDR))

    print('[*] continue...')
    stop = c.cont()
    print('[*] stopped:', stop[:60])

    regs = c.read_regs()
    print('[*] rip=%#x rsp=%#x' % (regs['rip'], regs['rsp']))

    # ---- arm a PHYSICAL write watchpoint on the code page ----------
    print('[*] Z2 watchpoint @phys %#x ->' % CODE_PA,
          c.cmd('Z2,%x,8' % CODE_PA))

    # remove the exec breakpoint so we can run freely
    if ADDR:
        c.cmd('z0,%x,1' % ADDR)

    print('[*] continue with watchpoint...')
    stop = c.cont()
    print('[*] WATCHPOINT HIT:', stop[:80])

    regs = c.read_regs()
    for k in ('rip','rsp','rax','rbx','rcx','rdx','rsi','rdi','r11'):
        print('    %-4s = %#x' % (k, regs.get(k, 0)))

    # who wrote? walk one instruction back is hard; dump context:
    mem = c.read_mem(0x400150, 32)
    print('[*] child-view code page now:', mem.hex() if mem else '<E>')

    # leave halted for HMP monitor inspection
    import time as _t
    _t.sleep(25)

if __name__ == '__main__':
    main()
