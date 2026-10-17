#!/usr/bin/env python3
# usage: nanvis.py in.spv out.spv : fragment shaders only; NaN float stores to Output become 50.0 (NX_SPIRV_TOOL hook)
import subprocess,sys,re,os
src,dst=sys.argv[1],sys.argv[2]
dis=subprocess.run(["spirv-dis",src],capture_output=True,text=True).stdout
if "OpEntryPoint Fragment" not in dis:
    open(dst,"wb").write(open(src,"rb").read()); sys.exit(0)
lines=dis.split("\n")
outvars=set(); outf=set()
float_ptr=set()
for l in lines:
    m=re.match(r"\s*(%\w+) = OpTypePointer Output (%\w+)",l)
    if m: float_ptr.add(m.group(1))
# need float type id
ft=None
for l in lines:
    m=re.match(r"\s*(%\w+) = OpTypeFloat 32\s*$",l)
    if m: ft=m.group(1)
bt=None
for l in lines:
    m=re.match(r"\s*(%\w+) = OpTypeBool\s*$",l)
    if m: bt=m.group(1)
# pointer types whose pointee is f32
fp=set()
for l in lines:
    m=re.match(r"\s*(%\w+) = OpTypePointer Output (%\w+)\s*$",l)
    if m and m.group(2)==ft: fp.add(m.group(1))
mask=set(os.environ.get("NANVIS","0.0,0.1,0.2,0.3,1.0,1.1,1.2,1.3").split(","))
loc={}
for l in lines:
    m=re.match(r"\s*OpDecorate (%\w+) Location (\d+)",l)
    if m: loc[m.group(1)]=m.group(2)
ptrs=set(); ptrinfo={}
for l in lines:
    m=re.match(r"\s*(%\w+) = OpVariable (%\w+) Output",l)
    if m and m.group(2) in fp: ptrs.add(m.group(1))
    m=re.match(r"\s*(%\w+) = OpAccessChain (%\w+) (%\w+) %int_(\d+)",l)
    if m and m.group(2) in fp and (loc.get(m.group(3),"?")+"."+m.group(4)) in mask: ptrs.add(m.group(1))
out=[]; n=0; placed=False
for l in lines:
    if not placed and re.match(r"\s*(%\w+ = )?OpFunction ",l):
        placed=True
        if not bt: out.append("%9000001 = OpTypeBool"); b="%9000001"
        else: b=bt
        out.append("%9000002 = OpConstant " + ft + " 50")
        bt=b
    m=re.match(r"(\s*)OpStore (%\w+) (%\w+)\s*$",l)
    if m and m.group(2) in ptrs:
        n+=1
        out.append(f"%{9100000+2*n} = OpIsNan {bt} {m.group(3)}")
        out.append(f"%{9100001+2*n} = OpSelect {ft} %{9100000+2*n} %9000002 {m.group(3)}")
        out.append(f"OpStore {m.group(2)} %{9100001+2*n}")
        continue
    out.append(l)
open(dst+".txt","w").write("\n".join(out))
r=subprocess.run(["spirv-as","--target-env","spv1.3",dst+".txt","-o",dst],capture_output=True,text=True)
if r.returncode: sys.stderr.write(r.stderr); open(dst,"wb").write(open(src,"rb").read())
os.remove(dst+".txt")
