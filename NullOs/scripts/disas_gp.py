import subprocess
out = subprocess.run(['objdump','-d','--start-address=0x10f980',
                      '--stop-address=0x10fa00','build/kernel.bin'],
                     capture_output=True,text=True)
print(out.stdout[-1400:])
