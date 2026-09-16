import subprocess, re
sym = subprocess.run(['nm', 'build/kernel.bin'], capture_output=True, text=True).stdout
a = re.search(r'([0-9a-f]+) T sched_exit_switch_away', sym)
addr = a.group(1)
out = subprocess.run(['objdump', '-d', '--start-address=0x'+addr,
                      '--stop-address='+hex(int(addr,16)+0x30),
                      'build/kernel.bin'], capture_output=True, text=True).stdout
print(out[-500:])
