import re
log = open('/tmp/nsh_mon.log', errors='ignore').read()
out = []
for line in log.splitlines():
    m = re.match(r'^([0-9a-f]{16}): (.*)$', line)
    if m and 0xb8000 <= int(m.group(1),16) < 0xc0000:
        for tok in m.group(2).split():
            try: out.append(int(tok,16)&0xFF)
            except: pass

# reconstruct in ADDRESS order (xp dumps are sequential over 0xb8000)
txt = ''.join(chr(b) if 32 <= b < 127 else ' ' for b in out)
print('total bytes:', len(txt))
for pat in ['echo hello','uname','/bin/','hello.txt','Hello from NullOs',
            'Process finished']:
    hits = txt.count(pat)
    print(f'{pat!r:28} -> {hits}')
