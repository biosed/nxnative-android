#!/usr/bin/env python3
# own_compilers.py (so dmsh_build.py) in this process with the Metal compile answered in Python (DMSH_SKIP_METAL=1 of spv_android.py): xcrun metal -c / metallib with -o get a placeholder
# output without a process, and the other processes the build starts (spirv-cross, the translator) use posix_spawn instead of a fork of this large, threaded process (close_fds=False:
# Python only takes posix_spawn without it). The SPIR-V and the reflection are what the fake xcrun on PATH gave; 73k shaders took ~2.3 h that way, mostly in fork and exec.
# Usage: spv_boot.py <own_compilers.py arguments>
import os, runpy, subprocess, sys
_run = subprocess.run
def run(args, *a, **k):
    if os.environ.get('DMSH_SKIP_METAL') == '1' and isinstance(args, (list, tuple)) and args and args[0] == 'xcrun':
        s = ' ' + ' '.join(map(str, args)) + ' '
        if (' metal -c ' in s or ' metallib ' in s) and '-o' in args:
            with open(args[list(args).index('-o') + 1], 'w') as f: f.write('DMSH_SKIP_METAL')
            return subprocess.CompletedProcess(args, 0, '' if k.get('text') else b'', '' if k.get('text') else b'')
    if isinstance(args, (list, tuple)) and args and os.sep not in str(args[0]):   # posix_spawn needs a path: look the program up once
        import shutil; p = shutil.which(str(args[0]))
        if p: args = [p, *args[1:]]
    k.setdefault('close_fds', False)
    return _run(args, *a, **k)
subprocess.run = run
own = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../gfx/tools/own_compilers.py')
sys.argv = [own, *sys.argv[1:]]
sys.path.insert(0, os.path.dirname(own))
runpy.run_path(own, run_name='__main__')
