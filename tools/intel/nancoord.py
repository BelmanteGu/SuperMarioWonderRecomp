#!/usr/bin/env python3
# in.spv out.spv: NaN in the coordinate operand of image samples -> 0 (fragment shaders)
import subprocess,sys,re,os
src,dst=sys.argv[1],sys.argv[2]
dis=subprocess.run(["spirv-dis",src],capture_output=True,text=True).stdout
if "OpEntryPoint Fragment" not in dis:
    open(dst,"wb").write(open(src,"rb").read()); sys.exit(0)
lines=dis.split("\n")
ty={}; vec={}   # id -> type id ; (comp,n)->id
ft=bt=None
for l in lines:
    m=re.match(r"\s*(%\w+) = OpTypeFloat 32\s*$",l)
    if m: ft=m.group(1)
    m=re.match(r"\s*(%\w+) = OpTypeBool\s*$",l)
    if m: bt=m.group(1)
for l in lines:
    m=re.match(r"\s*(%\w+) = OpTypeVector (%\w+) (\d)\s*$",l)
    if m: vec[(m.group(2),int(m.group(3)))]=m.group(1)
    m=re.match(r"\s*(%\w+) = Op\w+ (%\w+) ",l)
    if m: ty[m.group(1)]=m.group(2)
out=[]; n=0; placed=False; need={}
for l in lines:
    if not placed and re.match(r"\s*(%\w+ = )?OpFunction ",l):
        placed=True
        if not bt: out.append("%9000001 = OpTypeBool"); bt="%9000001"
        for k in (2,3,4):
            if (bt,k) not in vec: out.append(f"%90001{k} = OpTypeVector {bt} {k}"); vec[(bt,k)]=f"%90001{k}"
            fv=vec.get((ft,k))
            if fv: out.append(f"%90002{k} = OpConstantNull {fv}")
    m=re.match(r"(\s*)(%\w+) = (OpImageSample\w+) (%\w+) (%\w+) (%\w+)(.*)$",l) if placed else None
    if m:
        cty=ty.get(m.group(6)); k=None
        for (c,kk),tid in vec.items():
            if c==ft and tid==cty: k=kk
        if k:
            n+=1; c=m.group(6)
            out.append(f"%nvc{n}_n = OpIsNan {vec[(bt,k)]} {c}")
            out.append(f"%nvc{n} = OpSelect {cty} %nvc{n}_n %90002{k} {c}")
            out.append(f"{m.group(1)}{m.group(2)} = {m.group(3)} {m.group(4)} {m.group(5)} %nvc{n}{m.group(7)}")
            continue
    out.append(l)
open(dst+".txt","w").write("\n".join(out))
r=subprocess.run(["spirv-as","--target-env","spv1.3",dst+".txt","-o",dst],capture_output=True,text=True)
if r.returncode: sys.stderr.write(r.stderr); open(dst,"wb").write(open(src,"rb").read())
os.remove(dst+".txt")
