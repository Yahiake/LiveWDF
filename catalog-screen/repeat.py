from pathlib import Path
import csv
import subprocess

work=Path(__file__).resolve().parent
repo=work.parent/'pr38-device-expansion'
paths=(work/'repeat-list.txt').read_text().splitlines()
with (work/'repeat.csv').open('w') as f:
 writer=csv.writer(f)
 writer.writerow(['slug','pass','rate','level','in_peak_V','out_peak_V','out_rms_V','dc_out_V','preroll_rejects','rejects','nonfinite','cpu_us_frame','load_warnings'])
 for repeat in range(3):
  for name in paths if repeat%2 == 0 else paths[::-1]:
   p=repo/name
   ref='.12' if 'pedals_fx' in name else '1.25'
   output=subprocess.check_output([work/'screen',p,'48000',ref],text=True,timeout=45)
   writer.writerows([name,repeat,*row.split(',')] for row in output.splitlines())
   f.flush()
