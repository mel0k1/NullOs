import socket, time, sys

mode = sys.argv[1] if len(sys.argv) > 1 else 'probe'
s = socket.create_connection(('127.0.0.1', 1234), timeout=10)
s.settimeout(2)
time.sleep(0.3)

def drain():
    try:
        while True:
            d = s.recv(4096)
            if not d:
                return b''
    except socket.timeout:
        pass
    return b'drain-done'

print('initial drain:', drain())

def send(pkt):
    cs = sum(pkt.encode()) & 0xFF
    s.sendall(b'$' + pkt.encode() + ('#%02x' % cs).encode())
    # wait ack
    t0 = time.time()
    while time.time() - t0 < 3:
        try:
            d = s.recv(1)
            if d == b'+':
                return True
        except socket.timeout:
            return False
    return False

print('?', send('?'))
try:
    s.settimeout(1)
    r = s.recv(4096)
    print('reply:', r[:80])
except socket.timeout:
    print('no reply')

if mode == 'run':
    print('c ->', send('c'))

# watch aliveness
for i in range(int(sys.argv[2]) if len(sys.argv) > 2 else 6):
    time.sleep(2)
    try:
        s.settimeout(0.5)
        d = s.recv(4096)
        if not d:
            print(i, 'CONNECTION CLOSED BY PEER')
            sys.exit(0)
        print(i, 'data:', d[:60])
    except socket.timeout:
        print(i, 'alive')
