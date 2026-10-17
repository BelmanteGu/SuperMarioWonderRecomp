import sys,subprocess,collections,re
f=sys.argv[1]; exe=sys.argv[2]; thread=sys.argv[3]
wins=open(f).read().split("WINDOW\n")[1:]
use=wins[len(wins)//2:]   # second half = gameplay
samples=[]
for w in use:
    for l in w.splitlines():
        p=l.split()
        if len(p)>3 and p[2]==thread: samples.append(p[3:])
print("samples",len(samples))
addrs=set()
for s in samples:
    for fr in s:
        m,o=fr.rsplit("+",1)
        if m=="wonder": addrs.add(o)
addrs=sorted(addrs)
out=subprocess.run(["addr2line","-f","-C","-e",exe]+["0x"+a for a in addrs],capture_output=True,text=True).stdout.split("\n")[0::2]
name={a:n[:90] for a,n in zip(addrs,out)}
incl=collections.Counter(); self_=collections.Counter()
for s in samples:
    seen=set()
    for i,fr in enumerate(s):
        m,o=fr.rsplit("+",1)
        n=name[o] if m=="wonder" else "["+m+"]"
        if n not in seen: incl[n]+=1; seen.add(n)
        if i==0: self_[n]+=1
N=len(samples)
print("--inclusive")
for n,c in incl.most_common(45): print("%5.1f%% self %4.1f%% %s"%(100*c/N,100*self_[n]/N,n))
