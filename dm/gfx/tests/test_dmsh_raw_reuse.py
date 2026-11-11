#!/usr/bin/env python3
"""Raw-global lookups use the current hint ABI and vectorize wide loads.

The first access tries the tagged 2 MiB-chunk hint before a looped table search.
Later accesses first check the entry cached for this invocation. LDG.64 and
LDG.128 resolve once and perform one aligned vector load, including zeroing all
follower words when the address misses every pool.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_mem import EXIT, Translated, inst  # noqa: E402

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
STORE = (0x000fe8000000c800, 0x0000080609008388)
LDG = (0x000ea200081e890f, 0x0000000405067981)
VEC128 = ((LDG[0] & ~(7 << 9)) | (6 << 9),
          (LDG[1] & ~0xff0000) | (12 << 16))


def waiting(instruction):
    """Make the instruction wait for every scoreboard dependency."""
    return (instruction[0] | 0x3f << 52, instruction[1])


def s2r_tid(register):
    """Read SR_TID.X into *register* using the fixture's known SM86 form."""
    return (0x000e260000002100,
            0x0000000000007919 | register << 16)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'),
                     'set DMSH_TEST_XLATE to the offline translator')
@unittest.skipUnless(shutil.which('spirv-dis'), 'needs spirv-dis')
class RawReuse(Translated):
    def spv_dis(self, code):
        sys.path.insert(0, str(TOOLS))
        import dmsh_geometry

        with tempfile.TemporaryDirectory(prefix='raw_reuse_') as directory:
            work = Path(directory)
            (work / 'k.bin').write_bytes(
                b''.join(inst(*waiting(op)) for op in code) + inst(*EXIT))
            job = [dict(key='k', stage='cs', arch='sm86',
                        code=str(work / 'k.bin'), tex={}, img={}, attr={},
                        fout={}, local=[32, 1, 1], debug=True,
                        nvn2_cs=[0, 0, 3, 0x40, 0], shared=0x1000)]
            (work / 'jobs.json').write_text(json.dumps(job))
            run = subprocess.run(
                [os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'),
                 directory], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            dmsh_geometry.flatten_sets(work / 'k.spv')
            if shutil.which('spirv-val'):
                valid = subprocess.run(
                    ['spirv-val', '--target-env', 'vulkan1.3',
                     str(work / 'k.spv')], capture_output=True, text=True)
                self.assertEqual(valid.returncode, 0,
                                 valid.stdout + valid.stderr)
            return subprocess.run(
                ['spirv-dis', str(work / 'k.spv')], capture_output=True,
                text=True, check=True).stdout

    @staticmethod
    def functions(disassembly):
        return {match.group(1): match.group(0) for match in re.finditer(
            r'(%\w+) = OpFunction %int None .*?OpFunctionEnd',
            disassembly, re.S)}

    @staticmethod
    def main_function(disassembly):
        return re.search(
            r'%\w+ = OpFunction %void None .*?OpFunctionEnd',
            disassembly, re.S).group(0)

    def test_loads_share_one_zero_initialized_last_entry(self):
        second = (LDG[0], LDG[1] | 4 << 40)
        disassembly = self.spv_dis(
            (s2r_tid(9), s2r_tid(5), LDG, STORE, second, STORE))
        self.assertEqual(len(re.findall(
            r'OpVariable \S+ Private %\w+', disassembly)), 1)
        main = self.main_function(disassembly)
        calls = re.findall(r'OpFunctionCall %int (%\w+)', main)
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0], calls[1])
        full = self.functions(disassembly)[calls[0]]
        self.assertIn('OpLoopMerge', full)
        self.assertIn('OpConstant %int 8191', disassembly)
        self.assertRegex(full, r'OpShiftRightLogical %int \S+ %int_21')
        self.assertRegex(full, r'OpBitwiseAnd %int \S+ %int_8191')
        self.assertRegex(full, r'OpShiftRightLogical %int \S+ %int_13')

    def test_128_bit_load_uses_one_vector_helper(self):
        stores = tuple((STORE[0], (STORE[1] & ~0xff000000) |
                        (register << 24) | (offset << 40))
                       for register, offset in
                       ((12, 0), (13, 4), (14, 16), (15, 20)))
        disassembly = self.spv_dis(
            (s2r_tid(9), s2r_tid(5), VEC128, *stores))
        main = self.main_function(disassembly)
        calls = re.findall(r'OpFunctionCall %int (%\w+)', main)
        self.assertEqual(len(calls), 1)
        functions = self.functions(disassembly)
        helper = functions[calls[0]]
        self.assertIn('OpLoopMerge', helper)
        self.assertIn('%int_15', helper,
                      'the resolver must check the 128-bit load\'s last byte')
        self.assertRegex(helper, r'OpLoad %v4uint \S+ Aligned 4')
        self.assertGreaterEqual(
            len(re.findall(r'OpStore \S+ %uint_0', helper)), 4,
            'a miss must zero all three follower words and the last-entry cache')

    def test_adjacent_vectors_do_not_share_follower_words(self):
        first = (VEC128[0] & ~0xf, VEC128[1])
        second = (first[0],
                  (first[1] & ~(0xff << 16) & ~(0xffffff << 40)) |
                  (16 << 16) | (0x40 << 40))
        keep_first = (STORE[0], (STORE[1] & ~0xff000000) | (12 << 24))
        keep_second = (STORE[0],
                       (STORE[1] & ~0xff000000 & ~(0xffffff << 40)) |
                       (18 << 24) | (4 << 40))
        disassembly = self.spv_dis(
            (s2r_tid(9), s2r_tid(5), first, second,
             keep_first, keep_second))
        calls = re.findall(
            r'OpFunctionCall %int (%\w+)', self.main_function(disassembly))
        self.assertEqual(len(calls), 2)
        functions = self.functions(disassembly)
        self.assertEqual(sum('OpLoad %v4uint' in functions[call]
                             for call in set(calls)), 1)


if __name__ == '__main__':
    unittest.main()
