import csv
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

work=Path(__file__).resolve().parent
root=work.parent
repo=root/'pr38-device-expansion'
new=subprocess.check_output(['git','diff','--diff-filter=A','--name-only','origin/main','HEAD','--','schematics'],
                            cwd=repo,text=True).splitlines()
paths=[p for p in new if p.endswith('.livewdf')]
ngspice=root/'pedal-ngspice/usr/bin/ngspice'
env=dict(os.environ,SPICE_SCRIPTS=str(root/'parts-validation-work/ngspice-init'))
def run(item):
 i,p=item
 folder=work/'oracle'/str(i)
 folder.mkdir(exist_ok=True)
 path=folder/'full.cir'
 export=subprocess.run([work/'export',repo/p,path],capture_output=True,text=True)
 if export.returncode:
  return p,'export failed',0,0
 try:
  result=subprocess.run([ngspice,'-b','-o','ng.log','full.cir'],cwd=folder,env=env,capture_output=True,text=True,timeout=20)
 except subprocess.TimeoutExpired:
  return p,'timed out',0,0
 dc=folder/'dc_nodes.txt'; tran=folder/'tran_nodes.txt'
 return p,result.returncode,dc.stat().st_size if dc.exists() else 0,tran.stat().st_size if tran.exists() else 0
with (work/'oracle-export.csv').open('w') as out:
 writer=csv.writer(out)
 writer.writerow(['schematic','status','dc_bytes','transient_bytes'])
 with ThreadPoolExecutor(max_workers=4) as pool:
  for row in pool.map(run,enumerate(paths)):
   writer.writerow(row)
   out.flush()
