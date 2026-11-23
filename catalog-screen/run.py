import csv
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parent.parent
repo=root/'pr38-device-expansion'
exe=root/'catalog-screen/screen'
pedals=sorted((repo/'schematics/pedals_fx').rglob('*.livewdf'))
studio=sorted((repo/'schematics/studio').rglob('*.livewdf'))
new=set(subprocess.check_output(['git','diff','--diff-filter=A','--name-only','origin/main','HEAD','--','schematics'],cwd=repo,text=True).splitlines())
circuits=[p for p in pedals+studio if str(p.relative_to(repo)) in new]
assert len(circuits)==81,len(circuits)

def measure(args):
    path,fs=args
    if path in pedals: peak=.12
    elif 'Mic Preamp' in path.name or 'API 312' in path.name: peak=.02
    else: peak=1.25
    try:
        result=subprocess.run([str(exe),str(path),str(fs),str(peak)],text=True,capture_output=True,timeout=55)
        return str(path.relative_to(repo)),result.stdout.splitlines(),result.stderr or str(result.returncode)
    except subprocess.TimeoutExpired:
        return str(path.relative_to(repo)),[],f'{fs} timeout'

with (root/'catalog-screen/results.csv').open('w') as result, (root/'catalog-screen/errors.log').open('w') as errors:
    writer=csv.writer(result)
    writer.writerow(['path','rate','level','in_peak_V','out_peak_V','out_rms_V','dc_out_V','preroll_rejects','rejects','nonfinite','cpu_us_frame','load_warnings'])
    with ThreadPoolExecutor(max_workers=5) as threads:
        for name,rows,note in threads.map(measure,[(p,r) for p in circuits for r in (48000,96000)]):
            if note!='0':
                print(name,note,file=errors,flush=True)
            for row in rows:
                writer.writerow([name,*row.split(',')])
            result.flush()
