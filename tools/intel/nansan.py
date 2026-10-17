#!/usr/bin/env python3
# usage: nansan.py in.spv out.spv   env NANSAN=FDiv,InverseSqrt,... : NaN results of those scalar float ops become NANVAL (default 0)
import subprocess,sys,re,os
src,dst=sys.argv[1],sys.argv[2]
hsh=sys.argv[3] if len(sys.argv)>3 else ""
only=os.environ.get("NANHASH")
lo,hi=[int(x) for x in os.environ.get("NANRANGE","0-100000").split("-")]
dis=subprocess.run(["spirv-dis",src],capture_output=True,text=True).stdout
ops=set(os.environ.get("NANSAN","FDiv").split(","))
val=os.environ.get("NANVAL","0")
stage=os.environ.get("NANSTAGE","Fragment")
if ("OpEntryPoint "+stage) not in dis or (only and hsh not in only.split(",")):
    open(dst,"wb").write(open(src,"rb").read()); sys.exit(0)
lines=dis.split("\n")
ft=bt=None
for l in lines:
    m=re.match(r"\s*(%\w+) = OpTypeFloat 32\s*$",l)
    if m: ft=m.group(1)
    m=re.match(r"\s*(%\w+) = OpTypeBool\s*$",l)
    if m: bt=m.group(1)
inptr=set(re.findall(r'(%\w+) = OpTypePointer Input '+re.escape(ft)+r'\s*$',dis,re.M))
outptr=set(re.findall(r'(%\w+) = OpTypePointer Output '+re.escape(ft)+r'\s*$',dis,re.M))
out_ids=set()
in_ids=set()
for l in lines:
    m=re.match(r'\s*(%\w+) = OpAccessChain (%\w+) (%\w+) ',l)
    nv=os.environ.get('NANVARS')
    if m and m.group(2) in inptr and (not nv or m.group(3) in nv.split(',')): in_ids.add(m.group(1))
    if m and m.group(2) in outptr: out_ids.add(m.group(1))
print("in_ids",len(in_ids),len(inptr),file=sys.stderr)
out=[]; n=0; placed=False; fcount=-1
for l in lines:
    if not placed and re.match(r"\s*(%\w+ = )?OpFunction ",l):
        placed=True
        if not bt: out.append("%9000001 = OpTypeBool"); bt="%9000001"
        out.append("%9000002 = OpConstant "+ft+" "+val)
        out.append("%9000003 = OpTypeVector "+bt+" 4")
        v4=None
        for l2 in lines:
            m3=re.match(r"\s*(%\w+) = OpTypeVector "+re.escape(ft)+r" 4\s*$",l2)
            if m3: v4=m3.group(1)
        out.append("%9000004 = OpConstantNull "+v4)
    m=re.match(r"\s*(%\w+) = (OpFDiv|OpFMul|OpFAdd|OpFSub|OpExtInst %\w+ %\w+) ?(\w*)",l)
    done=False
    if placed:
        if re.match(r"\s*%\w+ = OpExtInst "+re.escape(ft)+r" %\w+ Fma ",l): fcount+=1
        m1=re.match(r"(\s*)(%\w+) = OpFDiv "+re.escape(ft)+r" (.*)$",l)
        m2=re.match(r"(\s*)(%\w+) = OpExtInst "+re.escape(ft)+r" (%\w+) (\w+) (.*)$",l)
        m3=re.match(r"(\s*)(%\w+) = (OpImageSample\w+|OpImageFetch|OpImageRead|OpImageGather) ("+re.escape(v4 or "%none")+r") (.*)$",l)
        m4=re.match(r'(\s*)(%\w+) = OpLoad '+re.escape(ft)+r' (%\w+)\s*$',l)
        m5=re.match(r'(\s*)OpStore (%\w+) (%\w+)\s*$',l)
        if m5 and m5.group(2) in out_ids and 'OutStoreInf' in ops:
            n+=1; r=m5.group(3)
            out.append(f'%san_nan{n} = OpIsNan {bt} {r}')
            out.append(f'%san_inf{n} = OpIsInf {bt} {r}')
            out.append(f'%san_bad{n} = OpLogicalOr {bt} %san_nan{n} %san_inf{n}')
            out.append(f'%san_val{n} = OpSelect {ft} %san_bad{n} %9000002 {r}')
            out.append(f'OpStore {m5.group(2)} %san_val{n}'); done=True
        elif m5 and m5.group(2) in out_ids and 'OutStore' in ops:
            n+=1; r=m5.group(3)
            out.append(f'%san_nan{n} = OpIsNan {bt} {r}')
            out.append(f'%san_val{n} = OpSelect {ft} %san_nan{n} %9000002 {r}')
            out.append(f'OpStore {m5.group(2)} %san_val{n}'); done=True
        elif m4 and m4.group(3) in in_ids and 'Input' in ops:
            n+=1; r=m4.group(2)
            out.append(f'{r}_raw{n} = OpLoad {ft} {m4.group(3)}')
            out.append(f'{r}_nan{n} = OpIsNan {bt} {r}_raw{n}')
            out.append(f'{r} = OpSelect {ft} {r}_nan{n} %9000002 {r}_raw{n}'); done=True
        elif m3 and "Sample" in ops:
            n+=1; r=m3.group(2)
            out.append(f"{r}_raw{n} = {m3.group(3)} {m3.group(4)} {m3.group(5)}")
            out.append(f"{r}_nan{n} = OpIsNan %9000003 {r}_raw{n}")
            out.append(f"{r} = OpSelect {m3.group(4)} {r}_nan{n} %9000004 {r}_raw{n}"); done=True
        elif m1 and "FDiv" in ops:
            n+=1; r=m1.group(2)
            out.append(f"{r}_raw{n} = OpFDiv {ft} {m1.group(3)}")
            out.append(f"{r}_nan{n} = OpIsNan {bt} {r}_raw{n}")
            out.append(f"{r} = OpSelect {ft} {r}_nan{n} %9000002 {r}_raw{n}"); done=True
        elif m2 and m2.group(4) in ops and lo <= fcount < hi:
            n+=1; r=m2.group(2)
            out.append(f"{r}_raw{n} = OpExtInst {ft} {m2.group(3)} {m2.group(4)} {m2.group(5)}")
            out.append(f"{r}_nan{n} = OpIsNan {bt} {r}_raw{n}")
            out.append(f"{r} = OpSelect {ft} {r}_nan{n} %9000002 {r}_raw{n}"); done=True
    if not done: out.append(l)
open(dst+".txt","w").write("\n".join(out))
r=subprocess.run(["spirv-as","--target-env","spv1.3",dst+".txt","-o",dst],capture_output=True,text=True)
if r.returncode: sys.stderr.write(r.stderr); open(dst,"wb").write(open(src,"rb").read())
os.remove(dst+".txt")
