#!/usr/bin/env python3
# in.spv out.spv [hash]: results of InverseSqrt and 1/x style FDiv (scalar float) are clamped to +-FLT_MAX, so 0*rsqrt(0) is 0, not NaN
import subprocess,sys,re,os
src,dst=sys.argv[1],sys.argv[2]
dis=subprocess.run(["spirv-dis",src],capture_output=True,text=True).stdout
ops=set(os.environ.get("INFOPS","InverseSqrt,FDiv").split(","))
if "OpEntryPoint Fragment" not in dis and "OpEntryPoint Vertex" not in dis:
    open(dst,"wb").write(open(src,"rb").read()); sys.exit(0)
lines=dis.split("\n")
ft=glsl=None
for l in lines:
    m=re.match(r"\s*(%\w+) = OpTypeFloat 32\s*$",l)
    if m: ft=m.group(1)
    m=re.match(r"\s*(%\w+) = OpExtInstImport \"GLSL.std.450\"",l)
    if m: glsl=m.group(1)
out=[]; n=0; placed=False
for l in lines:
    if not placed and re.match(r"\s*(%\w+ = )?OpFunction ",l):
        placed=True
        out.append("%9000002 = OpConstant "+ft+" 3.4028234e+38")
        out.append("%9000003 = OpConstant "+ft+" -3.4028234e+38")
    done=False
    if placed:
        m1=re.match(r"(\s*)(%\w+) = OpFDiv "+re.escape(ft)+r" (.*)$",l)
        m2=re.match(r"(\s*)(%\w+) = OpExtInst "+re.escape(ft)+r" (%\w+) (\w+) (.*)$",l)
        if m1 and "FDiv" in ops:
            n+=1; r=m1.group(2)
            out.append(f"{r}_raw{n} = OpFDiv {ft} {m1.group(3)}")
            out.append(f"{r} = OpExtInst {ft} {glsl} FClamp {r}_raw{n} %9000003 %9000002"); done=True
        elif m2 and m2.group(4) in ops:
            n+=1; r=m2.group(2)
            out.append(f"{r}_raw{n} = OpExtInst {ft} {m2.group(3)} {m2.group(4)} {m2.group(5)}")
            out.append(f"{r} = OpExtInst {ft} {glsl} FClamp {r}_raw{n} %9000003 %9000002"); done=True
    if not done: out.append(l)
open(dst+".txt","w").write("\n".join(out))
r=subprocess.run(["spirv-as","--target-env","spv1.3",dst+".txt","-o",dst],capture_output=True,text=True)
if r.returncode: sys.stderr.write(r.stderr); open(dst,"wb").write(open(src,"rb").read())
os.remove(dst+".txt")
