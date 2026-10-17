import re,sys,statistics as st
import os
for d in sys.argv[1:]:
    if not os.path.exists(d+"/nx.log"): continue
    L=[l for l in open(d+"/nx.log") if "per frame over" in l]
    rows=[]
    for l in L:
        m=re.search(r"([\d.]+) fps.*replay ([\d.]+) ms, submit \(encode\) ([\d.]+) ms, fence wait ([\d.]+) ms.*upload ([\d.]+) MB",l)
        rows.append(tuple(map(float,m.groups())))
    # steady: last 12 windows
    r=rows[-12:]
    print(d.split("intel-")[-1], len(rows), "last12 median fps %.1f min %.1f replay %.1f enc %.2f fence %.1f upl %.1f"%(st.median(x[0] for x in r),min(x[0] for x in r),st.median(x[1] for x in r),st.median(x[2] for x in r),st.median(x[3] for x in r),st.median(x[4] for x in r)))
