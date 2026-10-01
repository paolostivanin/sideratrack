#!/usr/bin/env python3
"""Record a worker's wall time, CPU usage and RSS without altering its inputs."""
import argparse,json,os,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--report',type=Path,required=True)
p.add_argument('--log',type=Path,required=True)
p.add_argument('command',nargs=argparse.REMAINDER)
a=p.parse_args()
if a.command and a.command[0]=='--':a.command.pop(0)
clock=os.sysconf('SC_CLK_TCK');begin=time.monotonic();previous=begin;ticks=0;peak_rss=0;peak_cpu=0;last_cpu=0;samples=[]
with a.log.open('w') as log:
 proc=subprocess.Popen(a.command,stdout=log,stderr=subprocess.STDOUT)
 while proc.poll() is None:
  try:
   stat=Path(f'/proc/{proc.pid}/stat').read_text().rsplit(')',1)[1].split()
   current=int(stat[11])+int(stat[12]);now=time.monotonic()
   cpu=(current-ticks)/clock/(now-previous)*100
   rss=0
   for line in Path(f'/proc/{proc.pid}/status').read_text().splitlines():
    if line.startswith(('VmRSS:','VmHWM:')):rss=max(rss,int(line.split()[1])*1024)
   peak_rss=max(peak_rss,rss);peak_cpu=max(peak_cpu,cpu);last_cpu=current/clock
   samples.append({'seconds':now-begin,'cpu_percent':cpu,'rss_bytes':rss})
   previous,ticks=now,current
  except (FileNotFoundError,ProcessLookupError):pass
  time.sleep(.5)
end=time.monotonic()
result={'command':a.command,'seconds':end-begin,'exit_code':proc.returncode,'peak_rss_bytes':peak_rss,'cpu_seconds_sampled':last_cpu,'average_cpu_percent':last_cpu/(end-begin)*100,'peak_cpu_percent':peak_cpu,'samples':samples}
a.report.write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k not in ('command','samples')},indent=2))
raise SystemExit(proc.returncode)
