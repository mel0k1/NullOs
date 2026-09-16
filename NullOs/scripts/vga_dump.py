import re
data_out = []
log = open('/tmp/nsh_mon.log', errors='ignore').read()
# QEMU monitor xp dumps: lines like "0000000000b80000: 0x... 0x..."
for line in log.splitlines():
    m = re.match(r'^([0-9a-f]{16}): (.*)$', line)
    if not m:
        continue
    addr = int(m.group(1), 16)
    if not (0xb8000 <= addr < 0xc0000):
        continue
    for tok in m.group(2).split():
        try:
            v = int(tok, 16)
        except ValueError:
            continue
        # each token is one byte for /bx
        data_out.append((addr, v & 0xFF))

txt = ''
for _, b in data_out:
    txt += chr(b) if 32 <= b < 127 else ' '
lines = [txt[i:i+80].rstrip() for i in range(0, len(txt), 80)]
for l in [l for l in lines if l.strip()][-24:]:
    print(l)
