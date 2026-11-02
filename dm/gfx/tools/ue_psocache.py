#!/usr/bin/env python3
# The game's own pipeline list as a prewarm list: Unreal Engine games ship the pipelines their testers met in a bundled pipeline cache (<project>/Content/PipelineCaches/<platform>/
# *.upipelinecache, in a pak), which names each pipeline's shaders by the SHA-1 of their code. Those hashes are the shader libraries' own (the version 2 archive's shader hash table, one per
# shader entry), so each maps to the content key the shader split gave that entry (shader_map.tsv, source column "<library>#UE<entry>"). The list written is in the prewarm format
# (docs/development.md, Prewarm lists): C <key>_b for a compute pipeline, S <key>_b <stage> for each shader of a graphics one; the host reads it beside the shader map as
# pso_engine.tsv (nvn_mtl.m warm_start), after the profile's pso_manifest.tsv. FINAL FANTASY RESONANCE's cache holds 757 compute pipelines (its Niagara and post-process kernels) and no
# graphics ones. A graphics pipeline's render state is the engine's, not NVN's, so only its shaders are listed (the host builds its pipelines when they are first drawn).
# File layout (FPipelineCacheFile, version 29, UE 5.6): header (magic, version, game version, platform byte, GUID, TOC offset, GC time), the pipeline records, then the TOC: magic, a byte,
# the GUID, a word, the entry count, and per entry: PSO hash u32, file offset u64, size u64, file GUID, stats (first / last frame i64, create count u64, bind count i64, PSO hash u32),
# the shader hash set (count i32, 20 bytes each), usage mask u64, engine flags u16, 8 more bytes; the file ends with EOF-MARK. A record starts with its type (0 compute, 1 graphics, 2 ray tracing).
# Usage: ue_psocache.py <RomFS> <ue_shaders dir> <shader_map.tsv> <out .tsv>   (environment DM_OODLE_HOST / DM_OODLE_MODULES: the Mac host and modules for an Oodle-compressed pak)
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uepak

MAGIC_TOC = b'2RATSCOT'
STAGE = {'vertex': 'vertex', 'pixel': 'pixel', 'compute': 'compute', 'geometry': 'geometry'}


def parse(b):
    """[(record type, [shader SHA-1])] of one pipeline cache file"""
    if b[:8] != b'HCACEPIP': raise ValueError('not an Unreal pipeline cache (magic)')
    toc = b.rfind(MAGIC_TOC)
    if toc < 0: raise ValueError('no TOC in the pipeline cache')
    p = toc + 8 + 1 + 16 + 4
    n = struct.unpack_from('<I', b, p)[0]; p += 4
    out = []
    for _ in range(n):
        _, off, size = struct.unpack_from('<IQQ', b, p); p += 20 + 16 + 36
        nsh = struct.unpack_from('<i', b, p)[0]; p += 4
        if not 0 <= nsh <= 16 or off + size > toc: raise ValueError(f'pipeline cache TOC entry at {p:#x} does not parse')
        shas = [bytes(b[p + 20 * k: p + 20 * k + 20]) for k in range(nsh)]; p += 20 * nsh + 18
        out.append((struct.unpack_from('<I', b, off)[0], shas))
    if b[p:p + 8] != b'KRAM-FOE': raise ValueError(f'pipeline cache TOC ends at {p:#x}, not at its end mark')
    return out


def caches(romfs, decode=None):
    """{name: bytes} of the bundled pipeline caches in the RomFS's paks"""
    out = {}
    for pk in uepak.paks(romfs):
        pak = uepak.Pak(pk)
        for name in pak.files:
            if name.endswith('.upipelinecache'): out[name] = pak.read(name, decode)
    return out


def shader_hashes(libdir):
    """{SHA-1: (library file name, entry)} from the decoded version 2 archives"""
    out = {}
    for name in sorted(os.listdir(libdir)):
        if not name.endswith('.ushaderbytecode'): continue
        a = open(os.path.join(libdir, name), 'rb').read()
        r = uepak.R(a, what=name)
        if r.take('<I') != uepak.ARCHIVE_VERSION: raise ValueError(f'{name}: not a version 2 shader code archive')
        r.raw(20 * r.take('<I'))
        for i in range(r.take('<I')): out.setdefault(r.raw(20), (name, i))
    return out


def lines(entries, hashes, shader_map):
    keys = {}
    for ln in open(shader_map):
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4 and '#UE' in c[4]: keys[(c[4].rsplit('/', 1)[-1].split('#')[0], int(c[4].rsplit('#UE', 1)[1]))] = (c[3], c[2])
    out, miss = {}, 0
    for kind, shas in entries:
        for s in shas:
            k = keys.get(hashes.get(s))
            if not k: miss += 1; continue
            key, stage = k
            if kind == 0 and stage == 'compute': out[f'C\t{key}_b'] = 1
            elif stage in STAGE: out[f'S\t{key}_b\t{STAGE[stage]}'] = 1
    return list(out), miss


def main():
    romfs, libdir, shader_map, dst = sys.argv[1:5]
    decode = None
    if os.environ.get('DM_OODLE_HOST'): decode = uepak.OodleHost(os.environ['DM_OODLE_HOST'], os.environ['DM_OODLE_MODULES'])
    try: cs = caches(romfs, decode)
    finally:
        if decode: decode.close()
    entries = []
    for name, b in sorted(cs.items()): e = parse(b); entries += e; print(f'{name}: {len(e)} pipelines')
    out, miss = lines(entries, shader_hashes(libdir), shader_map) if entries else ([], 0)
    tmp = dst + '.tmp'
    with open(tmp, 'w') as f:
        f.write('# The game\'s bundled Unreal pipeline cache as a prewarm list (dm/gfx/tools/ue_psocache.py; generated, do not edit)\n')
        f.writelines(l + '\n' for l in out)
    os.replace(tmp, dst)
    print(f'{len(out)} prewarm lines ({sum(l[0] == "C" for l in out)} compute pipelines); {miss} shader hashes not in the libraries or the map')


if __name__ == '__main__': main()
