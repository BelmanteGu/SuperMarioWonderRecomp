#!/usr/bin/env python3
import subprocess,sys,re,os
src,dst=sys.argv[1],sys.argv[2]
dis=subprocess.run(["spirv-dis",src],capture_output=True,text=True).stdout
t=re.sub(r"(OpExtInst %\w+ %\w+ )(FMin|FMax|FClamp)\b",lambda m:m.group(1)+{"FMin":"NMin","FMax":"NMax","FClamp":"NClamp"}[m.group(2)],dis)
open(dst+".txt","w").write(t)
r=subprocess.run(["spirv-as","--target-env","spv1.3",dst+".txt","-o",dst],capture_output=True,text=True)
if r.returncode: sys.stderr.write(r.stderr); open(dst,"wb").write(open(src,"rb").read())
os.remove(dst+".txt")
