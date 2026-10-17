#!/usr/bin/env python3
# usage: zinit.py in.spv out.spv [hash]   vertex shaders: every Location output variable gets a zero initializer
import subprocess,sys,re,os
src,dst=sys.argv[1],sys.argv[2]
dis=subprocess.run(["spirv-dis",src],capture_output=True,text=True).stdout
if "OpEntryPoint Vertex" not in dis:
    open(dst,"wb").write(open(src,"rb").read()); sys.exit(0)
ptr={}
for m in re.finditer(r"(%\w+) = OpTypePointer Output (%\w+)",dis): ptr[m.group(1)]=m.group(2)
loc=set(re.findall(r"OpDecorate (%\w+) Location \d+",dis))
out=[];n=0
for l in dis.split("\n"):
    m=re.match(r"(\s*)(%\w+) = OpVariable (%\w+) Output\s*$",l)
    if m and m.group(3) in ptr and m.group(2) in loc:
        n+=1
        out.append(f"%zi_{n} = OpConstantNull {ptr[m.group(3)]}")
        out.append(f"{m.group(2)} = OpVariable {m.group(3)} Output %zi_{n}")
    else: out.append(l)
open(dst+".txt","w").write("\n".join(out))
r=subprocess.run(["spirv-as","--target-env","spv1.3",dst+".txt","-o",dst],capture_output=True,text=True)
if r.returncode: sys.stderr.write(r.stderr); open(dst,"wb").write(open(src,"rb").read())
os.remove(dst+".txt")
