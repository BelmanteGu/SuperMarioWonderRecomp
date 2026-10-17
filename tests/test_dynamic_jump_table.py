"""Compile and execute a synthetic jump table at runtime-selected module bases; no game data."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from recomp.emit import Emitter
from recomp.analysis import Function


class DynamicJumpTableTest(unittest.TestCase):
    def test_relocated_targets_and_fallback(self):
        compiler = shutil.which("clang++") or shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        # adr x1, .; br x11; mov x0, #1; ret; mov x0, #2; ret
        code = struct.pack("<6I", 0x10000001, 0xD61F0160, 0xD2800020, 0xD65F03C0, 0xD2800040, 0xD65F03C0)
        function = Function(0x100, 0x118)
        emitter = Emitter(SimpleNamespace(module=SimpleNamespace(image=bytes(0x100) + code),
                                         functions=[function], jump_tables={0x104: [0x108, 0x110]}, plt={}))
        statement = emitter.function(function)
        source = r'''
#include <cstdint>
uint64_t runtime_base, fallback, base_reads;
uint64_t selected_base() { ++base_reads; return runtime_base; }
#define NXB selected_base()
#define NX_RESTRICT __restrict
namespace nx {
struct CPU { uint64_t x[32]{}; };
void call(CPU&, uint64_t address) { fallback = address; }
}
STATEMENT
int main() {
    for (uint64_t base : {0x300000000ull, 0x105800000ull, 0x4000000000ull, 0x7e70000000ull}) {
        runtime_base = base;
        for (uint64_t offset : {0x108ull, 0x110ull}) {
            nx::CPU c; c.x[11] = base + offset; fallback = 0; base_reads = 0;
            sub_00000100(c);
            if (c.x[0] != (offset == 0x108 ? 1 : 2) || fallback || base_reads != 1 || c.x[1] != base + 0x100) return 1;
        }
        for (uint64_t target : {uint64_t(0), base + 0x130, ~uint64_t(0)}) {
            nx::CPU c; c.x[11] = target; fallback = 1; base_reads = 0;
            sub_00000100(c);
            if (c.x[0] || fallback != target || base_reads != 1 || c.x[1] != base + 0x100) return 2;
        }
    }
}
'''.replace("STATEMENT", statement).replace("#include <cstdint>", "#include <cstdint>\n#include <initializer_list>")
        with tempfile.TemporaryDirectory() as work:
            cpp, binary = Path(work) / "branch.cpp", Path(work) / "branch"
            cpp.write_text(source)
            built = subprocess.run([compiler, "-std=c++20", "-O2", str(cpp), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr)


if __name__ == "__main__":
    unittest.main()
