import sys,subprocess,collections
f=sys.argv[1]; exe=sys.argv[2]; thread=sys.argv[3]; mods=sys.argv[4].split(",")
wins=open(f).read().split("WINDOW\n")[1:]
use=wins[len(wins)//2:]
S=[]
for w in use:
    for l in w.splitlines():
        p=l.split()
        if len(p)>3 and p[2]==thread and p[3].split("+")[0] in mods: S.append(p[3:])
addrs=set()
def nearest(s):
    for fr in s:
        m,o=fr.rsplit("+",1)
        if m=="wonder": return o
for s in S:
    o=nearest(s)
    if o: addrs.add(o)
addrs=sorted(addrs)
out=subprocess.run(["addr2line","-f","-C","-e",exe]+["0x"+a for a in addrs],capture_output=True,text=True).stdout.split("\n")[0::2]
name={a:n[:100] for a,n in zip(addrs,out)}
c=collections.Counter()
for s in S:
    o=nearest(s); c[name.get(o,"?")]+=1
tot=sum(c.values()); print(tot)
for n,v in c.most_common(18): print("%5.1f%% %s"%(100*v/tot,n))
