import re,subprocess,sys,collections
log=open(sys.argv[1]).read().split("\n")
exe=sys.argv[2]
# last window only: find last "sample: N samples in the window"
idx=max(i for i,l in enumerate(log) if "samples in the window" in l)
blk=log[idx:]
cur=None;items=[]
for l in blk:
    m=re.match(r"sample: thread (\d+) \((.*?)\): (\d+) samples",l)
    if m: cur=(m.group(1),m.group(2),int(m.group(3))); continue
    m=re.match(r"sample:\s+([\d.]+)% (\S+)\+0x([0-9a-f]+)",l)
    if m and cur: items.append((cur,float(m.group(1)),m.group(2),m.group(3)))
wo=[i for i in items if i[2].endswith("/wonder")]
out=subprocess.run(["addr2line","-f","-C","-e",exe]+["0x"+i[3] for i in wo],capture_output=True,text=True).stdout.split("\n")
fn=out[0::2]
agg=collections.defaultdict(lambda: collections.defaultdict(float))
for i,f in zip(wo,fn): agg[i[0]][f[:110]]+=i[1]
for t,d in agg.items():
    print(t)
    for f,v in sorted(d.items(),key=lambda x:-x[1])[:6]: print("   %5.1f%% %s"%(v,f))
