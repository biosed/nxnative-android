#!/usr/bin/env python3
# A game's shaders as SPIR-V for the Android host's Vulkan backend (dm/android/src/nvn_vk.c), from the same translation as the Metal libraries.
# The Metal chain (dm/gfx/tools/dmsh_build.py) translates every shader to SPIR-V with dmsh_xlate and keeps only the MSL; this runs that build once more into a scratch cache with the
# translator wrapped (the wrapper keeps each <key>.spv as dmsh_xlate wrote it: the integer-input second pass replaces the first), then writes per shader <out>/<key>.spv and
# <out>/<key>.vk.json: the SPIR-V's resource variables (set 0, a binding each, Program.cs), each with its descriptor type and the replay reflection entry of the same name
# (kind / reg / ... as in <key>_b.json; a combined image sampler also gets its sampler's entry, the MSL's <name>Smplr, as 'samp'), and the same as <key>.vk.txt for the host
# (vk_txt: one line per item, no JSON parser needed on the device).
# The chain version is unchanged (no dmsh_* file is touched); the Metal outputs of the scratch build are thrown away.
# Usage: DM_GAME=<id> [DM_GAMES_DIR=...] spv_android.py <shader package dir> <Metal cache dir> <out dir>
#        spv_android.py --xlate <job.json> <dir>   (the wrapper dmsh_build.py calls; SPV_KEEP names the folder that keeps the SPIR-V)
import collections, glob, json, os, shutil, struct, subprocess, sys, tempfile
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../..'))
XLATE = os.path.join(REPO, 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'); HERE = os.path.dirname(os.path.abspath(__file__))

if sys.argv[1] == '--xlate':
    if sys.argv[2:] == ['--source-sha']: os.execv(XLATE, [XLATE, '--source-sha'])   # dmsh_chain's check of the translator answers for the real one
    job, d = sys.argv[2:4]
    r = subprocess.run([XLATE, job, d])
    keep = os.environ['SPV_KEEP']
    for j in json.load(open(job)):
        p = os.path.join(d, j['key'] + '.spv')
        if os.path.exists(p): shutil.copyfile(p, os.path.join(keep, j['key'] + '.spv'))
    sys.exit(r.returncode)

SC = {0: 'UniformConstant', 2: 'Uniform', 12: 'StorageBuffer'}
def vk_reflect(spv, refl):
    d = open(spv, 'rb').read(); words = struct.unpack(f'<{len(d) // 4}I', d); i, ins = 5, []
    while i < len(words): n = words[i] >> 16; ins.append(words[i:i + n]); i += max(n, 1)
    op = lambda x: x[0] & 0xffff
    names = {x[1]: struct.pack(f'<{len(x) - 2}I', *x[2:]).split(b'\0')[0].decode() for x in ins if op(x) == 5}
    dec = collections.defaultdict(dict)
    for x in ins:
        if op(x) == 71 and len(x) >= 3: dec[x[1]][x[2]] = x[3] if len(x) > 3 else True
    types = {x[1]: x for x in ins if op(x) in (21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 32)}
    consts = {x[2]: x[3] for x in ins if op(x) == 43 and len(x) > 3}
    strip = lambda b: {k: v for k, v in b.items() if k not in ('msl_name', 'msl_type', 'arg_id')}
    by_name = {b['msl_name']: b for b in refl['bindings']}
    out = []
    for x in ins:
        if op(x) != 59 or x[3] not in SC or 33 not in dec[x[2]]: continue
        t = types[types[x[1]][3]]; count = 1
        while op(t) in (28, 29): count = consts.get(t[3], 1) if op(t) == 28 else 0; t = types[t[2]]
        if op(t) == 27: ty = 'tbuf' if types[t[2]][3] == 5 else 'cis'           # sampled image: a combined image sampler, or a buffer texture
        elif op(t) == 25: ty = ('utb' if t[7] == 1 else 'stb') if t[3] == 5 else ('simg' if t[7] == 2 else 'tex')
        elif op(t) == 26: ty = 'samp'
        else: ty = 'ssbo' if x[3] == 12 or 3 in dec[t[1]] else 'ubo'          # BufferBlock (3) in Uniform storage is a storage buffer
        nm = names.get(x[2], '')
        e = {'binding': dec[x[2]][33], 'type': ty, 'count': count, 'name': nm, **strip(by_name.get(nm, {'kind': 'support' if nm == 'support_buffer' else '?'}))}   # SPIRV-Cross drops an unread support buffer from the MSL
        if nm + 'Smplr' in by_name: e['samp'] = strip(by_name[nm + 'Smplr'])
        out.append(e)
    return sorted(out, key=lambda e: e['binding'])

def strip_caps(d):
    """The translator declares capabilities the host told it it has (Metal's answers) whether or not a shader uses them, and a Vulkan driver refuses a module that declares one the GPU
    lacks (Mali: Float64, fragment shader interlock). Those a shader does not use are dropped: Float64 without a 64-bit float type, the interlock (its extension and execution modes)
    without a begin / end interlock instruction"""
    w = list(struct.unpack(f'<{len(d) // 4}I', d)); hdr, i, ins = w[:5], 5, []
    while i < len(w): n = w[i] >> 16; ins.append(w[i:i + n]); i += max(n, 1)
    op = lambda x: x[0] & 0xffff
    f64 = any(op(x) == 22 and x[2] == 64 for x in ins)
    lock = any(op(x) in (5364, 5365) for x in ins)
    drop_caps = set() if f64 else {10}
    if not lock: drop_caps |= {5363, 5372, 5378}   # FragmentShaderSampleInterlockEXT, FragmentShaderPixelInterlockEXT, FragmentShaderShadingRateInterlockEXT
    out = []
    for x in ins:
        if op(x) == 17 and x[1] in drop_caps: continue
        if not lock and op(x) == 10 and b'fragment_shader_interlock' in struct.pack(f'<{len(x) - 1}I', *x[1:]): continue
        if not lock and op(x) == 16 and x[2] in (5366, 5367, 5368, 5369, 5370, 5371): continue   # interlock execution modes
        out.append(x)
    flat = hdr + [v for x in out for v in x]
    return struct.pack(f'<{len(flat)}I', *flat)

VTYPE = {'float': 0, 'uint': 1, 'int': 2}
def vk_txt(j):
    """stage <vs|ps|cs> / tg <x y z> / c1 <hex> / vin <location> <0 float, 1 uint, 2 int> / b <binding> <type> <count> <kind> <reg> <off or bank> <sampler kind> <sampler reg> <sampler off>;
    -1 / - = none. The sampler's byte offset places a samp_ubo handle word, which the host ORs with the texture's (a type-1 handle, nvn_vk.c texu_handle)"""
    L = [f"stage {j['stage']}"]
    if j.get('tg'): L.append('tg ' + ' '.join(str(x) for x in j['tg']))
    if j.get('c1'): L.append(f"c1 {j['c1']}")
    for v in j.get('vertex_inputs', []): L.append(f"vin {v['location']} {VTYPE.get(v['msl_type'].rstrip('1234'), 0)}")
    for b in j['bindings']:
        s_ = b.get('samp', {}); r = b.get('reg'); sr = s_.get('reg')
        L.append(f"b {b['binding']} {b['type']} {b['count']} {b['kind']} {-1 if r is None else r} {b.get('off', b.get('bank', -1))} {s_.get('kind', '-')} {-1 if sr is None else sr} {s_.get('off', -1)}")
    return '\n'.join(L) + '\n'

if sys.argv[1] == '--txt':   # rewrite the .vk.txt files of an output folder from its .vk.json, and strip its SPIR-V's unused capabilities
    for f in glob.glob(os.path.join(sys.argv[2], '*.vk.json')): open(f[:-5] + '.txt', 'w').write(vk_txt(json.load(open(f))))
    for f in glob.glob(os.path.join(sys.argv[2], '*.spv')): d = open(f, 'rb').read(); open(f, 'wb').write(strip_caps(d))
    sys.exit(0)

pkg, cache, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
w = tempfile.mkdtemp(prefix='spv_android_', dir=os.environ.get('WORK'))
keep = os.path.join(w, 'spv'); os.makedirs(keep)
# dmsh_chain checks the translator's files beside XLATE: the wrapper sits in a folder of links to them
xd = os.path.join(w, 'xlate'); os.makedirs(xd)
for f in os.listdir(os.path.dirname(XLATE)):
    if f != 'dmsh_xlate': os.symlink(os.path.join(os.path.dirname(XLATE), f), os.path.join(xd, f))
wrapper = os.path.join(xd, 'dmsh_xlate'); open(wrapper, 'w').write(f'#!/bin/sh\nexec "{sys.executable}" "{os.path.abspath(__file__)}" --xlate "$@"\n'); os.chmod(wrapper, 0o755)
env = dict(os.environ, XLATE=wrapper, SPV_KEEP=keep, DMSH_CAS=os.path.join(w, 'cas'), DMSH_PREFILL=cache, WORK=w)
# DMSH_SKIP_METAL=1: no Metal compile at all (the SPIR-V and the reflection come from the translation alone); for a game whose Mac libraries were built by another chain
# (dmsh_build.py is left as it is, so the chain version stays: an xcrun first on PATH answers the compile and link with a placeholder library and passes every other call on)
if os.environ.get('DMSH_SKIP_METAL') == '1':
    env.pop('DMSH_PREFILL'); env['DM_ALLOW_OTHER_METAL'] = '1'
    bd = os.path.join(w, 'bin'); os.makedirs(bd); xc = os.path.join(bd, 'xcrun')
    open(xc, 'w').write('#!/bin/sh\n# metal -c / metallib with -o: a placeholder output\n'
                        'case " $* " in *" metal -c "*|*" metallib "*) out=; prev=; for a in "$@"; do [ "$prev" = -o ] && out=$a; prev=$a; done\n'
                        '  if [ -n "$out" ]; then printf DMSH_SKIP_METAL > "$out"; exit 0; fi;; esac\nexec /usr/bin/xcrun "$@"\n')
    os.chmod(xc, 0o755); env['PATH'] = bd + os.pathsep + env.get('PATH', '')
# through own_compilers.py, as the NSP script builds them: DM_ALLOW_OTHER_METAL=1 accepts this Mac's Metal compiler when it is not the lock's (the SPIR-V does not depend on it)
# SPV_KEYS=<file>: only the keys it lists (one per line), a partial build (for timing or a rebuild of some shaders)
only = [l.split()[0] for l in open(os.environ['SPV_KEYS']) if l.strip()] if os.environ.get('SPV_KEYS') else []
# with DMSH_SKIP_METAL=1 through spv_boot.py: the placeholder compile in Python instead of the xcrun above (no process per library) and posix_spawn for the rest
boot = [os.path.join(HERE, 'spv_boot.py')] if os.environ.get('DMSH_SKIP_METAL') == '1' else [os.path.join(REPO, 'dm/gfx/tools/own_compilers.py')]
r = subprocess.run([sys.executable, *boot, pkg, os.path.join(w, 'cache'), *only], env=env, capture_output=True, text=True)
print(r.stdout[-3000:], r.stderr[-2000:])
b = glob.glob(os.path.join(w, 'cache', 'b-*'))
if r.returncode or not b: sys.exit('[RED] the shader build failed')
n = bad = 0
for spv in sorted(glob.glob(os.path.join(keep, '*.spv'))):
    key = os.path.basename(spv)[:-4]; rp = os.path.join(b[0], key + '_b.json')
    if not os.path.exists(rp): bad += 1; continue
    refl = json.load(open(rp))
    open(os.path.join(out, key + '.spv'), 'wb').write(strip_caps(open(spv, 'rb').read()))
    json.dump({'key': key, 'stage': refl['stage'], 'tg': refl.get('threadgroup_size'), 'c1': refl.get('c1', ''), 'vertex_inputs': refl.get('vertex_inputs', []),
               'drv_reads': refl.get('drv_reads', {}), 'bindings': vk_reflect(spv, refl)}, open(os.path.join(out, key + '.vk.json'), 'w'))
    open(os.path.join(out, key + '.vk.txt'), 'w').write(vk_txt(json.load(open(os.path.join(out, key + '.vk.json')))))
    n += 1
shutil.rmtree(w, ignore_errors=True)
print(f'{n} shaders written to {out}' + (f', {bad} without a replay reflection (failed in the Metal build)' if bad else ''))
