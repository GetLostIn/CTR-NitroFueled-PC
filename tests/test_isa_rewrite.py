"""The ISA rewrite scanner (scripts/isa_rewrite.py): instruction classes and site modes."""
from paths import ROOT
import unittest

import isa_rewrite


@unittest.skipUnless(isa_rewrite.iced_x86 is not None, 'iced_x86 is not installed')
class ScanSegmentTests(unittest.TestCase):
    """scan_segment over isa_rewrite.fixture_code(): blsr; movbe store; bextr (rip-relative);
    lzcnt 16-bit; tzcnt 64-bit; movbe store whose next instruction is a jmp (no relocation)."""

    def scan(self, code, address=0x1000):
        return isa_rewrite.scan_segment(code, address)

    def test_kinds_and_modes(self):
        sites = self.scan(isa_rewrite.fixture_code())
        self.assertEqual([s.kind for s in sites],
                         [isa_rewrite.KIND_BLSR, isa_rewrite.KIND_MOVBE_STORE, isa_rewrite.KIND_BEXTR,
                          isa_rewrite.KIND_LZCNT, isa_rewrite.KIND_TZCNT, isa_rewrite.KIND_MOVBE_STORE])
        self.assertEqual([s.mode for s in sites],
                         [isa_rewrite.MODE_JMP, isa_rewrite.MODE_JMP_RELOCATE, isa_rewrite.MODE_JMP,
                          isa_rewrite.MODE_JMP, isa_rewrite.MODE_JMP, isa_rewrite.MODE_INT3])

    def test_summary_counts_sets_and_modes(self):
        sites = self.scan(isa_rewrite.fixture_code())
        self.assertEqual(isa_rewrite.summary(sites),
                         'ISA: 6 sites (MOVBE 2, BMI1 3, LZCNT 1, F16C 0); 1 relocated, 1 int3, 0 unsupported')

    def test_widths_and_relocated_bytes(self):
        sites = self.scan(isa_rewrite.fixture_code())
        self.assertEqual([(s.kind, s.width) for s in sites if s.width],
                         [(isa_rewrite.KIND_BLSR, 32), (isa_rewrite.KIND_MOVBE_STORE, 32),
                          (isa_rewrite.KIND_BEXTR, 32), (isa_rewrite.KIND_LZCNT, 16),
                          (isa_rewrite.KIND_TZCNT, 64), (isa_rewrite.KIND_MOVBE_STORE, 32)])
        relocated = sites[1]
        self.assertEqual(relocated.relocated, b'\x90')  # the nop the 4-byte site's stub also runs
        self.assertEqual(relocated.length, 4)

    def test_rip_relative_memory_is_absolute(self):
        sites = self.scan(bytes.fromhex('c4e270f7050c000000'))  # bextr eax, [rip+0xc], ecx
        site, = sites
        self.assertEqual((site.kind, site.mem_flags), (isa_rewrite.KIND_BEXTR, 1))
        self.assertEqual(site.disp, 0x1000 + site.length + 0xc)

    def test_movbe_load_without_next_instruction_is_int3(self):
        sites = self.scan(bytes.fromhex('0f38f003'))  # movbe eax, [rbx]
        site, = sites
        self.assertEqual((site.kind, site.mode, site.width),
                         (isa_rewrite.KIND_MOVBE_LOAD, isa_rewrite.MODE_INT3, 32))

    def test_f16c_is_unsupported(self):
        sites = self.scan(bytes.fromhex('c4e3791dc000'))  # vcvtps2ph xmm0, xmm0, 0
        site, = sites
        self.assertEqual((site.kind, site.mode),
                         (isa_rewrite.KIND_VCVTPS2PH, isa_rewrite.MODE_UNSUPPORTED))


if __name__ == '__main__':
    unittest.main()
