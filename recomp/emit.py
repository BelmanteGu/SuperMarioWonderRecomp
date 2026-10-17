"""AArch64 -> C++ instruction emitter.

Each guest function becomes `void sub_XXXXXXXX(nx::CPU& c)`. Registers live in the context; intra-function
branches are gotos, jump tables are switches over the computed target, calls are direct when the target is
known and go through nx::call otherwise. Instructions we do not handle become nx::unimpl traps and are counted.
"""
import collections
import re
import struct

import capstone
from capstone import arm64_const as A

B = "NXB"

CC = {
    A.ARM64_CC_EQ: "c.zf", A.ARM64_CC_NE: "!c.zf", A.ARM64_CC_HS: "c.cf", A.ARM64_CC_LO: "!c.cf",
    A.ARM64_CC_MI: "c.nf", A.ARM64_CC_PL: "!c.nf", A.ARM64_CC_VS: "c.vf", A.ARM64_CC_VC: "!c.vf",
    A.ARM64_CC_HI: "(c.cf && !c.zf)", A.ARM64_CC_LS: "(!c.cf || c.zf)", A.ARM64_CC_GE: "(c.nf == c.vf)",
    A.ARM64_CC_LT: "(c.nf != c.vf)", A.ARM64_CC_GT: "(!c.zf && c.nf == c.vf)",
    A.ARM64_CC_LE: "(c.zf || c.nf != c.vf)", A.ARM64_CC_AL: "true", A.ARM64_CC_NV: "true",
}
INVERT = {A.ARM64_CC_EQ: A.ARM64_CC_NE, A.ARM64_CC_NE: A.ARM64_CC_EQ, A.ARM64_CC_HS: A.ARM64_CC_LO,
          A.ARM64_CC_LO: A.ARM64_CC_HS, A.ARM64_CC_MI: A.ARM64_CC_PL, A.ARM64_CC_PL: A.ARM64_CC_MI,
          A.ARM64_CC_VS: A.ARM64_CC_VC, A.ARM64_CC_VC: A.ARM64_CC_VS, A.ARM64_CC_HI: A.ARM64_CC_LS,
          A.ARM64_CC_LS: A.ARM64_CC_HI, A.ARM64_CC_GE: A.ARM64_CC_LT, A.ARM64_CC_LT: A.ARM64_CC_GE,
          A.ARM64_CC_GT: A.ARM64_CC_LE, A.ARM64_CC_LE: A.ARM64_CC_GT}

VAS = {  # arrangement -> (element bits, lanes)
    A.ARM64_VAS_16B: (8, 16), A.ARM64_VAS_8B: (8, 8), A.ARM64_VAS_8H: (16, 8), A.ARM64_VAS_4H: (16, 4),
    A.ARM64_VAS_4S: (32, 4), A.ARM64_VAS_2S: (32, 2), A.ARM64_VAS_2D: (64, 2), A.ARM64_VAS_1D: (64, 1),
    A.ARM64_VAS_1B: (8, 1), A.ARM64_VAS_1H: (16, 1), A.ARM64_VAS_1S: (32, 1), A.ARM64_VAS_1Q: (128, 1),
    A.ARM64_VAS_4B: (8, 4), A.ARM64_VAS_2H: (16, 2),
}

SYSREG_TPIDR = 0xDE82
SYSREG_TPIDRRO = 0xDE83
SYSREG_FPCR = 0xDA20
SYSREG_FPSR = 0xDA21
SYSREG_NZCV = 0xDA10


class Unhandled(Exception):
    pass


def subst(expr, **vals):
    """Replace the whole-word placeholders a / b / o in an expression template."""
    return re.sub(r"\b([abo])\b", lambda m: vals.get(m.group(1), m.group(1)), expr)


def u(bits):
    return f"uint{bits}_t"


def s(bits):
    return f"int{bits}_t"


class Reg:
    """A register operand: kind 'x' (64-bit gp), 'w' (32-bit gp), 'zr64'/'zr32', or fp/simd b/h/s/d/q/v."""

    def __init__(self, name):
        self.name = name
        if name is None:  # an operand Capstone decodes but cannot name
            raise Unhandled("register without a name")
        if name in ("sp",):
            self.kind, self.n = "x", 31
        elif name == "wsp":
            self.kind, self.n = "w", 31
        elif name == "xzr":
            self.kind, self.n = "zr64", None
        elif name == "wzr":
            self.kind, self.n = "zr32", None
        elif name == "fp":
            self.kind, self.n = "x", 29
        elif name == "lr":
            self.kind, self.n = "x", 30
        elif name[0] in "xwbhsdqv" and name[1:].isdigit():
            self.kind, self.n = name[0], int(name[1:])
        else:
            raise Unhandled(f"register {name}")

    @property
    def gp(self):
        return self.kind in ("x", "w", "zr64", "zr32")

    @property
    def bits(self):
        return {"x": 64, "zr64": 64, "w": 32, "zr32": 32, "b": 8, "h": 16, "s": 32, "d": 64, "q": 128,
                "v": 128}[self.kind]

    @property
    def zr(self):
        return self.kind.startswith("zr")

    def r(self):
        """Read as unsigned of its width."""
        if self.zr:
            return "0ull" if self.bits == 64 else "0u"
        if self.bits == 64:
            return f"c.x[{self.n}]"
        return f"uint32_t(c.x[{self.n}])"

    def w(self, expr):
        if self.zr:
            return f"(void)({expr});"
        if self.bits == 64:
            return f"c.x[{self.n}] = uint64_t({expr});"
        return f"c.x[{self.n}] = uint32_t({expr});"


# Mixins with more handlers; they import the helpers defined above.
from .emit_ext import ExtOps  # noqa: E402
from .emit_vldst import VecLdSt  # noqa: E402
from .emit_int import IntOps  # noqa: E402
from .emit_fp import FpOps  # noqa: E402
from .emit_neon import NeonOps  # noqa: E402
from .emit_neon_wide import NeonWideOps  # noqa: E402


class Emitter(ExtOps, VecLdSt, IntOps, FpOps, NeonOps, NeonWideOps):
    def __init__(self, an, prefix="sub"):
        self.an = an
        self.m = an.module
        self.cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.cs.detail = True
        self.entries = {f.start for f in an.functions}
        self.prefix = prefix
        # Debug: NX_TRACE_SP=0xADDR,... logs SP after every instruction that changes it in those functions.
        import os
        self.trace_sp = {int(x, 16) for x in os.environ.get("NX_TRACE_SP", "").split(",") if x}
        self.unhandled = collections.Counter()
        self.handled = 0

    # ---- helpers ------------------------------------------------------------------------------------------

    def regop(self, i, o):
        return Reg(i.reg_name(o.reg))

    def fn(self, addr):
        return f"{self.prefix}_{addr:08X}"

    def shifted(self, i, o, bits):
        """Value of a register/immediate second operand with shift or extend applied, as uint{bits}."""
        if o.type == A.ARM64_OP_IMM:
            v = o.imm
            if o.shift.type == A.ARM64_SFT_LSL:
                v <<= o.shift.value
            return f"{u(bits)}({v & ((1 << bits) - 1):#x}ull)"
        r = self.regop(i, o)
        v = r.r()
        if o.ext:
            e = o.ext
            src = {A.ARM64_EXT_UXTB: "uint8_t", A.ARM64_EXT_UXTH: "uint16_t", A.ARM64_EXT_UXTW: "uint32_t",
                   A.ARM64_EXT_UXTX: "uint64_t", A.ARM64_EXT_SXTB: "int8_t", A.ARM64_EXT_SXTH: "int16_t",
                   A.ARM64_EXT_SXTW: "int32_t", A.ARM64_EXT_SXTX: "int64_t"}[e]
            v = f"{u(bits)}({s(bits) if src.startswith('int') else u(bits)}({src}({v})))"
            if o.shift.type == A.ARM64_SFT_LSL and o.shift.value:
                v = f"{u(bits)}({v} << {o.shift.value})"
            return v
        if o.shift.type and o.shift.value:
            n = o.shift.value
            t = o.shift.type
            if t == A.ARM64_SFT_LSL:
                return f"{u(bits)}({u(bits)}({v}) << {n})"
            if t == A.ARM64_SFT_LSR:
                return f"{u(bits)}({u(bits)}({v}) >> {n})"
            if t == A.ARM64_SFT_ASR:
                return f"{u(bits)}({s(bits)}({v}) >> {n})"
            if t == A.ARM64_SFT_ROR:
                return f"nx::ror{bits}({v}, {n})"
            raise Unhandled("shift")
        return f"{u(bits)}({v})"

    def cond(self, i):
        return CC[i.cc]

    def target(self, o):
        return o.imm

    def goto_or_tail(self, f, t):
        if f.start <= t < f.end:
            self.labels.add(t)
            return f"goto L_{t:X};"
        if t in self.entries:
            return f"{{ {self.fn(t)}(c); return; }}"
        return f"{{ nx::call(c, {B} + {t:#x}ull); return; }}"

    # ---- memory addressing --------------------------------------------------------------------------------

    def address(self, i, memo, after):
        """Returns (address expression, statement to run after the access for writeback)."""
        base = Reg(i.reg_name(memo.mem.base))
        bexpr = f"c.x[{base.n}]"
        idx = ""
        if memo.mem.index:
            ir = Reg(i.reg_name(memo.mem.index))
            iv = ir.r()
            if memo.ext in (A.ARM64_EXT_SXTW,):
                iv = f"uint64_t(int64_t(int32_t({iv})))"
            elif memo.ext in (A.ARM64_EXT_UXTW,):
                iv = f"uint64_t(uint32_t({iv}))"
            elif memo.ext in (A.ARM64_EXT_SXTX,):
                iv = f"uint64_t({iv})"
            if memo.shift.type == A.ARM64_SFT_LSL and memo.shift.value:
                iv = f"(uint64_t({iv}) << {memo.shift.value})"
            idx = f" + uint64_t({iv})"
        disp = memo.mem.disp
        post = None
        if after is not None:
            if after.type == A.ARM64_OP_IMM:
                post = f"{after.imm & 0xFFFFFFFFFFFFFFFF:#x}ull"
            elif after.type == A.ARM64_OP_REG:
                post = Reg(i.reg_name(after.reg)).r()
        if post is not None:  # post-index (capstone does not always flag writeback for structured ld/st)
            return bexpr + idx, f"c.x[{base.n}] += {post};"
        a = f"{bexpr}{idx} + {disp & 0xFFFFFFFFFFFFFFFF:#x}ull" if disp else bexpr + idx
        if i.writeback:
            return a, f"c.x[{base.n}] = a;"
        return a, ""

    # ---- function ---------------------------------------------------------------------------------------

    def function(self, f):
        self.labels = set()
        self.setjmp_sites = []
        body = []
        code = bytes(self.m.image[f.start:f.end])
        insns = {}
        for a in range(f.start, f.end, 4):
            got = list(self.cs.disasm(code[a - f.start:a - f.start + 4], a))
            insns[a] = got[0] if got else None
        jt = self.an.jump_tables
        last_term = False
        for a in range(f.start, f.end, 4):
            i = insns[a]
            if i is None:
                word = struct.unpack_from("<I", code, a - f.start)[0]
                body.append((a, f"nx::unimpl(c, {B} + {a:#x}ull, \"undecodable {word:#010x}\");"))
                last_term = True
                continue
            try:
                stmt = self.insn(f, i, jt)
                self.handled += 1
            except Unhandled as e:
                self.unhandled[i.mnemonic] += 1
                txt = f"{i.mnemonic} {i.op_str}".replace('"', "'")
                stmt = f"nx::unimpl(c, {B} + {a:#x}ull, \"{txt}\"); // {e}"
            if f.start in self.trace_sp and ("sp" in i.op_str.split(",")[0] or "[sp" in i.op_str and i.writeback):
                stmt += f" nx::trace_sp(c, {B} + {a:#x}ull);"
            body.append((a, f"{stmt} // {i.mnemonic} {i.op_str}"))
            last_term = i.mnemonic in ("b", "br", "ret", "udf", "brk") or stmt.endswith("return; }")
        out = [f"void {self.fn(f.start)}(nx::CPU& NX_RESTRICT c) {{"]
        sj = self.setjmp_sites
        if sj:
            # Functions that call setjmp run inside a try: a longjmp on one of their buffers resumes after the call.
            out.append(f"    nx::JmpState js_[{len(sj)}]; int jr_ = -1;")
            out.append("    for (;;) { try {")
            cases = " ".join(f"case {k}: goto L_{a:X};" for k, a in enumerate(sj))
            out.append(f"    switch (jr_) {{ {cases} default: break; }}")
        for a, stmt in body:
            if a in self.labels:
                out.append(f"L_{a:X}:;")
            out.append("    " + stmt)
        if not last_term:
            # Falls off the end: continue into whatever follows.
            out.append("    " + (f"{self.fn(f.end)}(c);" if f.end in self.entries else
                                 f"nx::call(c, {B} + {f.end:#x}ull);"))
        if sj:
            out.append("    return;")
            out.append("    } catch (const nx::LongJmp& e_) {")
            out.append("        jr_ = -1;")
            out.append(f"        for (int k = 0; k < {len(sj)}; k++) if (js_[k].buf == e_.buf) {{ jr_ = k; break; }}")
            out.append("        if (jr_ < 0) throw;")
            out.append("        js_[jr_].restore(c); c.x[0] = e_.val ? e_.val : 1;")
            out.append("    } }")
        out.append("}")
        # Darwin may select the load address at startup. Capture it once per function, rather than expanding the
        # guarded layout accessor at every guest ADR/ADRP, branch and return address.
        return "\n".join([out[0], f"    const uint64_t module_base_ = {B};"] +
                         [line.replace(B, "module_base_") for line in out[1:]])

    # ---- instruction dispatch -----------------------------------------------------------------------------

    def insn(self, f, i, jt):
        mn = i.mnemonic
        ops = i.operands
        h = getattr(self, "i_" + mn.replace(".", "_"), None)
        if h:
            return h(f, i, ops) if mn not in ("br",) else h(f, i, ops, jt)
        if mn.startswith("b."):
            return f"if ({self.cond(i)}) {self.goto_or_tail(f, self.target(ops[0]))}"
        if self.LSE.match(mn):
            return self.lse(i, ops)
        raise Unhandled("no handler")

    # branches
    def i_b(self, f, i, ops):
        return self.goto_or_tail(f, self.target(ops[0]))

    def i_bl(self, f, i, ops):
        t = self.target(ops[0])
        if self.an.plt.get(t) in ("setjmp", "_setjmp", "__setjmp"):
            # setjmp: remember the callee-saved state; a later longjmp on this buffer lands at the next label.
            k = len(self.setjmp_sites)
            self.setjmp_sites.append(i.address + 4)
            self.labels.add(i.address + 4)
            return f"js_[{k}].save(c); c.x[0] = 0;"
        call = f"{self.fn(t)}(c);" if t in self.entries else f"nx::call(c, {B} + {t:#x}ull);"
        return f"c.x[30] = {B} + {i.address + 4:#x}ull; {call}"

    def i_blr(self, f, i, ops):
        r = self.regop(i, ops[0])
        return f"{{ uint64_t t = {r.r()}; c.x[30] = {B} + {i.address + 4:#x}ull; nx::call(c, t); }}"

    def i_br(self, f, i, ops, jt):
        r = self.regop(i, ops[0])
        if i.address in jt:
            cases = " ".join(f"case {t:#x}ull: goto L_{t:X};" for t in jt[i.address])
            for t in jt[i.address]:
                self.labels.add(t)
            return f"switch ({r.r()} - {B}) {{ {cases} default: nx::call(c, {r.r()}); return; }}"
        return f"{{ nx::call(c, {r.r()}); return; }}"

    def i_ret(self, f, i, ops):
        if ops and self.regop(i, ops[0]).n != 30:
            return f"{{ nx::call(c, {self.regop(i, ops[0]).r()}); return; }}"
        return "return;"

    def i_cbz(self, f, i, ops):
        return f"if ({self.regop(i, ops[0]).r()} == 0) {self.goto_or_tail(f, self.target(ops[1]))}"

    def i_cbnz(self, f, i, ops):
        return f"if ({self.regop(i, ops[0]).r()} != 0) {self.goto_or_tail(f, self.target(ops[1]))}"

    def i_tbz(self, f, i, ops):
        return f"if ((({self.regop(i, ops[0]).r()} >> {ops[1].imm}) & 1) == 0) {self.goto_or_tail(f, self.target(ops[2]))}"

    def i_tbnz(self, f, i, ops):
        return f"if ((({self.regop(i, ops[0]).r()} >> {ops[1].imm}) & 1) != 0) {self.goto_or_tail(f, self.target(ops[2]))}"

    # misc
    def i_nop(self, f, i, ops):
        return ";"

    i_hint = i_nop
    i_prfm = i_nop
    i_prfum = i_nop
    i_yield = i_nop

    i_wfe = i_nop
    i_wfi = i_nop
    i_sev = i_nop
    i_sevl = i_nop
    def i_dc(self, f, i, ops):
        if "zva" in i.op_str:  # zero a 64-byte block (DCZID_EL0 reports 64)
            r = Reg(i.op_str.split(",")[1].strip())
            return f"std::memset(reinterpret_cast<void*>(c.x[{r.n}] & ~63ull), 0, 64);"
        return ";"  # cache maintenance

    i_ic = i_nop
    i_autia1716 = i_nop
    i_autib1716 = i_nop
    i_pacia1716 = i_nop
    i_pacib1716 = i_nop

    def crc(self, i, ops, poly, size):
        d, n, m = (self.regop(i, o) for o in ops[:3])
        return d.w(f"nx::crc32<{size}>({n.r()}, {m.r()}, {poly:#x}u)")

    def i_crc32b(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 1)

    def i_crc32h(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 2)

    def i_crc32w(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 4)

    def i_crc32x(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 8)

    def i_crc32cb(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 1)

    def i_crc32ch(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 2)

    def i_crc32cw(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 4)

    def i_crc32cx(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 8)

    def i_ngc(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{u(d.bits)}({u(d.bits)}(~{self.regop(i, ops[1]).r()}) + c.cf)")

    def i_ngcs(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::adds{d.bits}(c, 0, {u(d.bits)}(~{self.regop(i, ops[1]).r()}), c.cf)")

    def i_dmb(self, f, i, ops):
        return "nx::barrier();"

    i_dsb = i_dmb
    i_isb = i_nop

    def i_clrex(self, f, i, ops):
        return "nx::clrex(c);"

    def i_udf(self, f, i, ops):
        return f"nx::unimpl(c, {B} + {i.address:#x}ull, \"udf\");"

    def i_brk(self, f, i, ops):
        return f"nx::unimpl(c, {B} + {i.address:#x}ull, \"brk\");"

    def i_svc(self, f, i, ops):
        return f"nx::svc(c, {ops[0].imm}, {B} + {i.address:#x}ull);"

    def sysreg(self, i):
        word = struct.unpack("<I", bytes(i.bytes))[0]
        return (word >> 5) & 0xFFFF

    def i_mrs(self, f, i, ops):
        d = self.regop(i, ops[0])
        sr = self.sysreg(i)
        v = {SYSREG_TPIDR: "c.tpidr", SYSREG_TPIDRRO: "c.tpidrro", SYSREG_FPCR: "c.fpcr", SYSREG_FPSR: "c.fpsr",
             SYSREG_NZCV: "nx::get_nzcv(c)"}.get(sr, f"nx::mrs(c, {sr:#x})")
        return d.w(v)

    def i_msr(self, f, i, ops):
        word = struct.unpack("<I", bytes(i.bytes))[0]
        if (word >> 20) != 0xD51:
            raise Unhandled("msr pstate")
        sr = self.sysreg(i)
        v = Reg(i.reg_name(ops[-1].reg)).r()
        return {SYSREG_TPIDR: f"c.tpidr = {v};", SYSREG_FPCR: f"c.fpcr = {v};", SYSREG_FPSR: f"c.fpsr = {v};",
                SYSREG_NZCV: f"nx::set_nzcv(c, uint32_t({v} >> 28));"}.get(sr, f"nx::msr(c, {sr:#x}, {v});")

    # ---- loads / stores -----------------------------------------------------------------------------------

    def mem_ops(self, ops):
        """(register operands, mem operand, trailing post-index operand)."""
        regs, mem, after = [], None, None
        for o in ops:
            if o.type == A.ARM64_OP_MEM:
                mem = o
            elif mem is None:
                regs.append(o)
            else:
                after = o
        return regs, mem, after

    def load_into(self, r, addr, size, signed):
        if r.gp:
            if signed:
                return r.w(f"{u(r.bits)}({s(r.bits)}(nx::ld<{s(size * 8)}>({addr})))")
            return r.w(f"nx::ld<{u(size * 8)}>({addr})")
        if size == 16:
            return f"c.v[{r.n}] = nx::ld<nx::V128>({addr});"
        return f"{{ nx::V128 t_ = nx::vzero(); t_.u{size * 8}[0] = nx::ld<{u(size * 8)}>({addr}); c.v[{r.n}] = t_; }}"

    def store_from(self, r, addr, size):
        if r.gp:
            return f"nx::st<{u(size * 8)}>({addr}, {u(size * 8)}({r.r()}));"
        if size == 16:
            return f"nx::st<nx::V128>({addr}, c.v[{r.n}]);"
        return f"nx::st<{u(size * 8)}>({addr}, c.v[{r.n}].u{size * 8}[0]);"

    def ldst(self, i, ops, size=None, signed=False, store=False, pair=False):
        regs, mem, after = self.mem_ops(ops)
        if mem is None:
            # Literal: ldr x0, #addr (the address is an immediate operand, not a register)
            addr = f"{B} + {ops[-1].imm:#x}ull"
            r = self.regop(i, ops[0])
            sz = size or r.bits // 8
            return self.load_into(r, addr, sz, signed)
        rs = [self.regop(i, o) for o in regs]
        sz = size or rs[0].bits // 8
        a, wb = self.address(i, mem, after)
        parts = [f"uint64_t a = {a};"]
        if store:
            for k, r in enumerate(rs):
                parts.append(self.store_from(r, f"a + {k * sz}" if k else "a", sz))
            parts.append(wb)
        else:
            # Load everything first so base-register overlap cannot corrupt the address.
            if pair:
                tmp = []
                for k, r in enumerate(rs):
                    parts.append(f"auto v{k} = nx::ld<{'nx::V128' if sz == 16 else (s(sz * 8) if signed else u(sz * 8))}>(a + {k * sz});")
                    tmp.append(k)
                parts.append(wb)
                for k, r in enumerate(rs):
                    if r.gp:
                        parts.append(r.w(f"{u(r.bits)}({s(r.bits)}(v{k}))" if signed else f"v{k}"))
                    elif sz == 16:
                        parts.append(f"c.v[{r.n}] = v{k};")
                    else:
                        parts.append(f"{{ nx::V128 t_ = nx::vzero(); t_.u{sz * 8}[0] = v{k}; c.v[{r.n}] = t_; }}")
            else:
                parts.append(wb)
                parts.append(self.load_into(rs[0], "a", sz, signed))
                if wb and rs[0].gp and rs[0].n == Reg(i.reg_name(mem.mem.base)).n:
                    raise Unhandled("load writeback to base")
        return "{ " + " ".join(p for p in parts if p) + " }"

    def i_ldr(self, f, i, ops):
        return self.ldst(i, ops)

    i_ldur = i_ldr

    def i_ldrb(self, f, i, ops):
        return self.ldst(i, ops, 1)

    i_ldurb = i_ldrb

    def i_ldrh(self, f, i, ops):
        return self.ldst(i, ops, 2)

    i_ldurh = i_ldrh

    def i_ldrsb(self, f, i, ops):
        return self.ldst(i, ops, 1, signed=True)

    i_ldursb = i_ldrsb

    def i_ldrsh(self, f, i, ops):
        return self.ldst(i, ops, 2, signed=True)

    i_ldursh = i_ldrsh

    def i_ldrsw(self, f, i, ops):
        return self.ldst(i, ops, 4, signed=True)

    i_ldursw = i_ldrsw

    def i_str(self, f, i, ops):
        return self.ldst(i, ops, store=True)

    i_stur = i_str

    def i_strb(self, f, i, ops):
        return self.ldst(i, ops, 1, store=True)

    i_sturb = i_strb

    def i_strh(self, f, i, ops):
        return self.ldst(i, ops, 2, store=True)

    i_sturh = i_strh

    def i_ldp(self, f, i, ops):
        return self.ldst(i, ops, pair=True)

    i_ldnp = i_ldp

    def i_ldpsw(self, f, i, ops):
        return self.ldst(i, ops, 4, signed=True, pair=True)

    def i_stp(self, f, i, ops):
        return self.ldst(i, ops, store=True)

    i_stnp = i_stp

    # acquire / release / exclusive
    def acq(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        d = self.regop(i, regs[0])
        sz = size or d.bits // 8
        return d.w(f"nx::ld_acq<{u(sz * 8)}>(c.x[{Reg(i.reg_name(mem.mem.base)).n}])")

    def rel(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        r = self.regop(i, regs[0])
        sz = size or r.bits // 8
        return f"nx::st_rel<{u(sz * 8)}>(c.x[{Reg(i.reg_name(mem.mem.base)).n}], {u(sz * 8)}({r.r()}));"

    def i_ldar(self, f, i, ops):
        return self.acq(i, ops, None)

    def i_ldarb(self, f, i, ops):
        return self.acq(i, ops, 1)

    def i_ldarh(self, f, i, ops):
        return self.acq(i, ops, 2)

    def i_stlr(self, f, i, ops):
        return self.rel(i, ops, None)

    def i_stlrb(self, f, i, ops):
        return self.rel(i, ops, 1)

    def i_stlrh(self, f, i, ops):
        return self.rel(i, ops, 2)

    def ldx(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        d = self.regop(i, regs[0])
        sz = size or d.bits // 8
        return d.w(f"nx::ldxr<{u(sz * 8)}>(c, c.x[{Reg(i.reg_name(mem.mem.base)).n}])")

    def stx(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        st, r = self.regop(i, regs[0]), self.regop(i, regs[1])
        sz = size or r.bits // 8
        return st.w(f"nx::stxr<{u(sz * 8)}>(c, c.x[{Reg(i.reg_name(mem.mem.base)).n}], {u(sz * 8)}({r.r()}))")

    def i_ldaxr(self, f, i, ops):
        return self.ldx(i, ops, None)

    i_ldxr = i_ldaxr

    def i_ldaxrb(self, f, i, ops):
        return self.ldx(i, ops, 1)

    i_ldxrb = i_ldaxrb

    def i_ldaxrh(self, f, i, ops):
        return self.ldx(i, ops, 2)

    i_ldxrh = i_ldaxrh

    def i_stlxr(self, f, i, ops):
        return self.stx(i, ops, None)

    i_stxr = i_stlxr

    def i_stlxrb(self, f, i, ops):
        return self.stx(i, ops, 1)

    i_stxrb = i_stlxrb

    def i_stlxrh(self, f, i, ops):
        return self.stx(i, ops, 2)

    i_stxrh = i_stlxrh
