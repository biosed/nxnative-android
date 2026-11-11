#!/usr/bin/env python3
"""Ampere S2UR special register 0x84 through the offline translator.

Bytes 1 and 2 are the variable-rate-shading fragment width and height.  The
host shades one pixel per fragment, so the register must be 0x00010100 and its
width-times-height product must fold to one.  Needs DMSH_TEST_XLATE and the
SPIR-V tools on PATH.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


EXIT = (0x000fea0003800000, 0x000000000000794d)
S2UR = (0x000ea20000008400, 0x00000000000479c3)       # S2UR UR4, SR 0x84
S2UR_UNKNOWN = (0x000ea20000008500, S2UR[1])          # S2UR UR4, SR 0x85
S2R = (0x000e260000008400, 0x0000000000067919)        # S2R R6, SR 0x84
BYTE2 = (0x004fe2000800003f, 0x0000777204057896)      # UPRMT UR5, UR4, 0x7772
BYTE1 = (0x000fc4000800003f, 0x0000777104047896)      # UPRMT UR4, UR4, 0x7771
PRODUCT = (0x000fe2000f8e023f, 0x00000004050472a4)    # UIMAD UR4, UR5, UR4, URZ
MOV_UR4 = (0x000fe20000000f00, 0x0000000400067c02)    # MOV R6, UR4
STORE = (0x000fe8000000c800, 0x0000080609008388)      # STS.U32 [R9 * 16 + 8], R6


def inst(hi, lo):
    return struct.pack('<QQ', lo, hi)


def waiting(i):
    return (i[0] | 0x3f << 52, i[1])


def s2r_tid(rd):
    return (0x000e260000002100, 0x0000000000007919 | rd << 16)


@unittest.skipUnless(
    os.environ.get('DMSH_TEST_XLATE') and shutil.which('spirv-val') and shutil.which('spirv-dis'),
    'set DMSH_TEST_XLATE; needs spirv-val and spirv-dis',
)
class FragmentSize(unittest.TestCase):
    def translate(self, *body, move_uniform=True):
        with tempfile.TemporaryDirectory(prefix='ampere_fragment_size_') as d:
            work = Path(d)
            code = inst(*s2r_tid(9))
            tail = (MOV_UR4, STORE) if move_uniform else (STORE,)
            code += b''.join(inst(*waiting(i)) for i in (*body, *tail))
            code += inst(*EXIT)
            (work / 'k.bin').write_bytes(code)
            job = [dict(
                key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'),
                tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], shared=0x1000,
                debug=True, nvn2_cs=[0, 0, 3, 0x40, 0],
            )]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run(
                [os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d],
                capture_output=True, text=True,
            )
            self.log = r.stdout + r.stderr
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            spv = work / 'k.spv'
            self.assertTrue(spv.exists(), r.stdout + r.stderr)
            self.result = json.loads((work / 'k.res.json').read_text())
            v = subprocess.run(
                ['spirv-val', '--target-env', 'vulkan1.3', str(spv)],
                capture_output=True, text=True,
            )
            self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
            return subprocess.run(
                ['spirv-dis', '--raw-id', str(spv)],
                capture_output=True, text=True, check=True,
            ).stdout

    def test_register_value(self):
        dis = self.translate(S2UR)
        self.assertRegex(dis, r'OpConstant %\d+ 65792\b')

    def test_nonuniform_register_value(self):
        dis = self.translate(S2R, move_uniform=False)
        self.assertRegex(dis, r'OpConstant %\d+ 65792\b')

    def test_one_pixel_fragment(self):
        dis = self.translate(S2UR, BYTE2, BYTE1, PRODUCT)
        one = re.search(r'^\s*(%\d+) = OpConstant %\d+ 1$', dis, re.MULTILINE)
        self.assertIsNotNone(one, dis)
        self.assertRegex(dis, rf'OpStore %\d+ {re.escape(one.group(1))}\b')

    def test_unknown_register_is_diagnostic(self):
        self.translate(S2UR_UNKNOWN)
        message = 'S2R of special register 0x85, which the translator does not know: read as 0'
        self.assertIn(message, json.dumps(self.result), self.result)


if __name__ == '__main__':
    unittest.main()
