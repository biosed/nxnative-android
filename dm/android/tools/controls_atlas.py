#!/usr/bin/env python3
# The on-screen controls' sprite atlas, made at APK build time from the SVG masters in app/res_svg (never kept in git): every sprite is nested into one SVG,
# a grid of CELL x CELL cells, and rasterized once with rsvg-convert (Homebrew's librsvg) into atlas.png; atlas.txt names each cell. The game's overlay
# (app/front_android.c, drawn by the Vulkan backend) and the launcher's layout editor (app/java/.../ControlsActivity.java) both read these two files from assets/controls/.
#   controls_atlas.py <out dir> [--cell 384]
import argparse, os, re, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); SRC = os.path.join(os.path.dirname(HERE), 'app', 'res_svg')

def build(out, cell=384):
    rsvg = shutil.which('rsvg-convert') or '/opt/homebrew/bin/rsvg-convert'
    if not os.path.exists(rsvg): sys.exit('[RED] rsvg-convert missing: brew install librsvg (it rasterizes the on-screen controls from app/res_svg)')
    names = sorted(f[:-4] for f in os.listdir(SRC) if f.endswith('.svg'))
    cols = 4; rows = (len(names) + cols - 1) // cols; W, H = cols * cell, rows * cell
    parts = []
    for i, n in enumerate(names):
        s = open(os.path.join(SRC, n + '.svg')).read()
        m = re.search(r'<svg\b([^>]*)>(.*)</svg>', s, re.S)
        if not m: sys.exit(f'[RED] {n}.svg: no <svg> element')
        vb = re.search(r'viewBox="([^"]+)"', m.group(1))
        x, y = (i % cols) * cell, (i // cols) * cell
        parts.append(f'<svg x="{x}" y="{y}" width="{cell}" height="{cell}" viewBox="{vb.group(1) if vb else "0 0 256 256"}">{m.group(2)}</svg>')   # ids are unique per sprite (prefixed in each file)
    os.makedirs(out, exist_ok=True)
    svg = os.path.join(out, 'atlas.svg')
    open(svg, 'w').write(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">' + ''.join(parts) + '</svg>')
    subprocess.run([rsvg, '-w', str(W), '-h', str(H), '-o', os.path.join(out, 'atlas.png'), svg], check=True)
    os.remove(svg)
    with open(os.path.join(out, 'atlas.txt'), 'w') as f:
        f.write(f'# on-screen controls atlas: {W}x{H}, cells of {cell}, {cols} columns; name and cell index (row-major)\ncell {cell} {cols} {W} {H}\n')
        for i, n in enumerate(names): f.write(f'{n} {i}\n')
    return os.path.join(out, 'atlas.png'), os.path.join(out, 'atlas.txt')

if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('out'); ap.add_argument('--cell', type=int, default=384); a = ap.parse_args()
    png, txt = build(a.out, a.cell); print(png, txt)
