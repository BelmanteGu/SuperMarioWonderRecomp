#!/usr/bin/env python3
# in.spv out.spv: float stores to fragment outputs are clamped to +-65504 (what an fp16 ROP saturates to)
import subprocess,sys,re,os
src,dst=sys.argv[1],sys.argv[2]
dis=subprocess.run(["spirv-dis",src],capture_output=True,text=True).stdout
if "OpEntryPoint Fragment" not in dis:
    open(dst,"wb").write(open(src,"rb").read()); sys.exit(0)
lines=dis.split("\n")
ft=None; glsl=None
for l in lines:
    m=re.match(r"\s*(%\w+) = OpTypeFloat 32\s*$",l)
    if m: ft=m.group(1)
    m=re.match(r"\s*(%\w+) = OpExtInstImport \"GLSL.std.450\"",l)
    if m: glsl=m.group(1)
fp=set()
for l in lines:
    m=re.match(r"\s*(%\w+) = OpTypePointer Output (%\w+)\s*$",l)
    if m and m.group(2)==ft: fp.add(m.group(1))
ptrs=set()
for l in lines:
    m=re.match(r"\s*(%\w+) = OpVariable (%\w+) Output",l)
    if m and m.group(2) in fp: ptrs.add(m.group(1))
    m=re.match(r"\s*(%\w+) = OpAccessChain (%\w+) (%\w+)",l)
    if m and m.group(2) in fp: ptrs.add(m.group(1))
if not glsl or not ptrs:
    open(dst,"wb").write(open(src,"rb").read()); sys.exit(0)
out=[]; n=0; placed=False
for l in lines:
    if not placed and re.match(r"\s*(%\w+ = )?OpFunction ",l):
        placed=True
        out.append("%9000002 = OpConstant "+ft+" 65504")
        out.append("%9000003 = OpConstant "+ft+" -65504")
    m=re.match(r"(\s*)OpStore (%\w+) (%\w+)\s*$",l)
    if m and m.group(2) in ptrs:
        n+=1
        out.append(f"%nvq{n} = OpExtInst {ft} {glsl} FClamp {m.group(3)} %9000003 %9000002")
        out.append(f"OpStore {m.group(2)} %nvq{n}")
        continue
    out.append(l)
open(dst+".txt","w").write("\n".join(out))
r=subprocess.run(["spirv-as","--target-env","spv1.3",dst+".txt","-o",dst],capture_output=True,text=True)
if r.returncode: sys.stderr.write(r.stderr); open(dst,"wb").write(open(src,"rb").read())
os.remove(dst+".txt")
