#!/usr/bin/env python3
# Every raster graphic of the app is made at APK build time from the SVG masters in app/res_svg, the only source (nothing raster is kept in git):
#   - the on-screen controls (res_svg/*.svg) -> assets/controls/atlas.png + atlas.txt: the game's overlay (app/front_android.c, drawn by the Vulkan backend) and the
#     layout editor (ControlsActivity) draw them;
#   - the launcher's and the editor's icons (res_svg/ui/*.svg) -> assets/ui/atlas.png + atlas.txt (UiIcons.java);
#   - backgrounds (res_svg/bg/*.svg) -> assets/ui/<name>.png, each rendered whole.
# An atlas nests every sprite into one SVG, a grid of cell x cell cells, rasterized once with rsvg-convert (Homebrew's librsvg); atlas.txt names each cell.
#   controls_atlas.py <out dir> [--cell 384]   (the controls' atlas only; make_apk.py calls build_all)
import argparse, os, re, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); SRC = os.path.join(os.path.dirname(HERE), 'app', 'res_svg')

def rsvg_path():
    rsvg = shutil.which('rsvg-convert') or '/opt/homebrew/bin/rsvg-convert'
    if not os.path.exists(rsvg): sys.exit('[RED] rsvg-convert missing: brew install librsvg (it rasterizes the app\'s graphics from app/res_svg)')
    return rsvg

def build(out, cell=384, src=SRC, cols=4):
    rsvg = rsvg_path()
    names = sorted(f[:-4] for f in os.listdir(src) if f.endswith('.svg'))
    rows = (len(names) + cols - 1) // cols; W, H = cols * cell, rows * cell
    parts = []
    for i, n in enumerate(names):
        s = open(os.path.join(src, n + '.svg')).read()
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
        f.write(f'# sprite atlas (made from the SVG masters in app/res_svg): {W}x{H}, cells of {cell}, {cols} columns; name and cell index (row-major)\ncell {cell} {cols} {W} {H}\n')
        for i, n in enumerate(names): f.write(f'{n} {i}\n')
    return os.path.join(out, 'atlas.png'), os.path.join(out, 'atlas.txt')

def render(svg, png, w, h):   # one SVG rendered whole (a background)
    subprocess.run([rsvg_path(), '-w', str(w), '-h', str(h), '-o', png, svg], check=True)

def build_all(work):   # {path in the APK: file} for every graphic
    out = {}
    png, txt = build(os.path.join(work, 'controls'), 384, SRC, 4); out['assets/controls/atlas.png'] = png; out['assets/controls/atlas.txt'] = txt
    png, txt = build(os.path.join(work, 'ui'), 192, os.path.join(SRC, 'ui'), 6); out['assets/ui/atlas.png'] = png; out['assets/ui/atlas.txt'] = txt
    for f in sorted(os.listdir(os.path.join(SRC, 'bg'))):
        if f.endswith('.svg'): p = os.path.join(work, 'ui', f[:-4] + '.png'); render(os.path.join(SRC, 'bg', f), p, 1600, 1000); out['assets/ui/' + f[:-4] + '.png'] = p
    return out

if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('out'); ap.add_argument('--cell', type=int, default=384); a = ap.parse_args()
    png, txt = build(a.out, a.cell); print(png, txt)
