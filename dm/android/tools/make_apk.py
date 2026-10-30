#!/usr/bin/env python3
# The Android app of one game: the menu (app/java), the host library (make -C dm/android GAME=<id>), the game's icon, and optionally a save to start from and a cheat file,
# signed with a development key. The game files are not in the APK (several GB): the deploy step copies them into the app's external folder over adb.
#   make_apk.py <id> <NSP build folder> <out.apk> [--save <folder>] [--cheats <file>] [--label <name>] [--bundle]
#     --bundle: the game files go inside the APK too (assets/game/, deflated, listed in assets/game/files.txt) and the menu unpacks them on the first start: one file, no
#     computer needed. Every file is stored as pieces, each piece once (assets/game/p/<hash>): archives of (hash, start, end) entries, such as Metroid Dread's .pkg files, are cut at
#     their entries, which repeat across archives (Dread's 4.7 GB deflated becomes 3.7 GB), and whole files that repeat are stored once. --test: package <package>.prueba, label "(prueba)": installs beside the real app without touching it. Android takes APKs below 4 GB only; past 2 GB the ZIP stays without ZIP64 records (apksigner and the installer read it as a plain ZIP)
#   make_apk.py --deploy <id> <NSP build folder> [--from-device <folder on the device that already has romfs/dm/vk/font>]
# Tools: Homebrew's android-commandlinetools (build-tools 34, platform 33) and openjdk@21; the key is <NSP folder>/../android-debug.keystore (made once, kept so updates install over)
import argparse, glob, hashlib, os, re, shutil, struct, subprocess, sys, tempfile, zipfile
HERE = os.path.dirname(os.path.abspath(__file__)); ANDROID = os.path.dirname(HERE)
SDK = os.environ.get('ANDROID_SDK', '/opt/homebrew/share/android-commandlinetools')
BT = os.path.join(SDK, 'build-tools', '34.0.0'); JAR = os.path.join(SDK, 'platforms', 'android-33', 'android.jar')
JAVA = os.environ.get('JAVA_HOME', '/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home')
ENV = dict(os.environ, JAVA_HOME=JAVA, PATH=os.path.join(JAVA, 'bin') + os.pathsep + os.environ['PATH'])
def run(*a, **k): subprocess.run([str(x) for x in a], check=True, env=ENV, **k)
def package(gid, test=False): return 'com.nxnative.t' + gid.lower() + ('.prueba' if test else '')   # test: installs beside the real app, its own data

MANIFEST = '''<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{pkg}" android:versionCode="{vc}" android:versionName="{vn}">
  <uses-sdk android:minSdkVersion="30" android:targetSdkVersion="33"/>
  <uses-feature android:name="android.hardware.vulkan.version" android:version="0x401000" android:required="true"/>
  <application android:label="{label}" android:icon="@mipmap/icon" android:hasCode="true" android:extractNativeLibs="true" android:isGame="true"
               android:appCategory="game" android:theme="@android:style/Theme.DeviceDefault.NoActionBar.Fullscreen">
    <activity android:name="com.nxnative.MenuActivity" android:exported="true" android:screenOrientation="sensorLandscape"
              android:configChanges="orientation|screenSize|keyboard|keyboardHidden|navigation|uiMode|screenLayout|smallestScreenSize">
      <intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter>
    </activity>
    <activity android:name="android.app.NativeActivity" android:process=":game" android:exported="false" android:screenOrientation="sensorLandscape"
              android:configChanges="orientation|screenSize|keyboard|keyboardHidden|navigation|uiMode|screenLayout|smallestScreenSize|density">
      <meta-data android:name="android.app.lib_name" android:value="dmhost"/>
    </activity>
  </application>
</manifest>
'''

def build(a):
    lib = os.path.join(ANDROID, 'gen', a.id, 'libdmhost.so')
    if not os.path.exists(lib): sys.exit(f'[RED] {lib} missing: make -C dm/android GAME={a.id} first')
    w = tempfile.mkdtemp(prefix='apk_', dir=os.environ.get('WORK'))
    label = (a.label or a.id) + (' (prueba)' if a.test else '')
    vc = int(subprocess.run(['date', '+%y%m%d%H'], capture_output=True, text=True).stdout.strip())   # each build installs over the last
    open(os.path.join(w, 'AndroidManifest.xml'), 'w').write(MANIFEST.format(pkg=package(a.id, a.test), vc=vc, vn='0.1.' + str(vc), label=label.replace('&', '&amp;')))
    res = os.path.join(w, 'res', 'mipmap'); os.makedirs(res)
    icon = (glob.glob(os.path.join(a.build, 'extract', '*', 'icon.jpg')) or [None])[0]
    if icon: run('sips', '-s', 'format', 'png', '-Z', '192', icon, '--out', os.path.join(res, 'icon.png'), stdout=subprocess.DEVNULL)
    else: sys.exit('[RED] no icon.jpg under the build folder\'s extract/')
    run(os.path.join(BT, 'aapt2'), 'compile', '--dir', os.path.join(w, 'res'), '-o', os.path.join(w, 'res.zip'))
    base = os.path.join(w, 'base.apk')
    run(os.path.join(BT, 'aapt2'), 'link', '-o', base, '-I', JAR, '--manifest', os.path.join(w, 'AndroidManifest.xml'), os.path.join(w, 'res.zip'))
    cls = os.path.join(w, 'classes'); os.makedirs(cls)
    run(os.path.join(JAVA, 'bin', 'javac'), '--release', '11', '-cp', JAR, '-d', cls, *glob.glob(os.path.join(ANDROID, 'app', 'java', '**', '*.java'), recursive=True))
    run(os.path.join(BT, 'd8'), '--release', '--min-api', '33', '--lib', JAR, '--output', w, *glob.glob(os.path.join(cls, '**', '*.class'), recursive=True))
    with zipfile.ZipFile(base, 'a', zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(w, 'classes.dex'), 'classes.dex')
        z.write(lib, 'lib/arm64-v8a/libdmhost.so')
        if a.cheats: z.write(a.cheats, 'assets/cheats.txt')
        if a.save:
            for root, _, fs in os.walk(a.save):
                for f in sorted(fs):
                    if not f.startswith('.'): p = os.path.join(root, f); z.write(p, 'assets/save/' + os.path.relpath(p, a.save))
        if a.bundle:
            listing = []; stored = set(); raw = 0
            for rel, src in game_files(a.build):
                d = open(src, 'rb').read(); names = []
                for off, n in pieces(d):
                    k = hashlib.sha1(d[off:off + n]).hexdigest()[:24]; names.append(k)
                    if k not in stored: stored.add(k); z.writestr(zipfile.ZipInfo('assets/game/p/' + k, (2020, 1, 1, 0, 0, 0)), d[off:off + n], zipfile.ZIP_DEFLATED); raw += n
                listing.append(f'{rel}\t{len(d)}\t' + ' '.join(names))
            z.writestr('assets/game/files.txt', '\n'.join(listing) + '\n')
            print(f'game: {len(listing)} files, {len(stored)} pieces, {raw / 1e9:.2f} GB stored once')
    if os.path.getsize(base) >= 0xFFFFFFFF: sys.exit(f'[RED] the APK would be {os.path.getsize(base) / 1e9:.1f} GB: Android installs APKs below 4 GB only')
    aligned = os.path.join(w, 'aligned.apk')
    run(os.path.join(BT, 'zipalign'), '-f', '-p', '4', base, aligned)
    ks = os.path.join(os.path.dirname(os.path.abspath(a.build)), '..', 'android-debug.keystore'); ks = os.path.abspath(ks)
    if not os.path.exists(ks):
        run(os.path.join(JAVA, 'bin', 'keytool'), '-genkeypair', '-keystore', ks, '-storepass', 'android', '-keypass', 'android', '-alias', 'nxnative', '-keyalg', 'RSA', '-keysize', '2048',
            '-validity', '10000', '-dname', 'CN=nxnative development', stdout=subprocess.DEVNULL)
    run(os.path.join(BT, 'apksigner'), 'sign', '--ks', ks, '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--ks-key-alias', 'nxnative', '--out', a.out, aligned)
    shutil.rmtree(w, ignore_errors=True)
    print(f'{a.out}: {package(a.id, a.test)} "{label}", {os.path.getsize(a.out) / 1e6:.1f} MB' + (f', save from {a.save}' if a.save else '') + (f', cheats {os.path.basename(a.cheats)}' if a.cheats else ''))

def pieces(d):   # (offset, length) ranges that cover the file: an archive of (u64 hash, u32 start, u32 end) entries after (u32 header size past this word, u32 data size, u32 count)
    if len(d) >= 12:   # is cut at its entries
        hs, ds, n = struct.unpack_from('<III', d, 0); hs += 4
        if 0 < n and 12 + 16 * n <= hs and hs + ds == len(d):
            ents = sorted(struct.unpack_from('<QII', d, 12 + 16 * i)[1:] for i in range(n)); out = []; pos = 0
            if all(hs <= s <= e <= len(d) for s, e in ents):
                for s, e in ents:
                    if s < pos: break   # overlapping entries: kept whole
                    if s > pos: out.append((pos, s - pos))
                    if e > s: out.append((s, e - s))
                    pos = e
                else:
                    if pos < len(d): out.append((pos, len(d) - pos))
                    return out
    return [(0, len(d))]

def game_files(b):   # (path in the app's external folder, source) for every game file
    vk = glob.glob(os.path.join(b, 'shader', 'vk'))[0]; out = []
    for f in sorted(glob.glob(os.path.join(b, 'patched_android', '*.patched.bin')) + glob.glob(os.path.join(b, 'patched_android', '*.tsv'))): out.append(('dm/' + os.path.basename(f), f))
    for f in sorted(glob.glob(os.path.join(vk, '*.spv')) + glob.glob(os.path.join(vk, '*.vk.txt'))): out.append(('vk/' + os.path.basename(f), f))
    for f in sorted(glob.glob(os.path.join(b, 'font', '*'))): out.append(('font/' + os.path.basename(f), f))
    romfs = glob.glob(os.path.join(b, 'extract', '*', 'RomFS'))[-1]
    for root, _, fs in os.walk(romfs):
        for f in sorted(fs): p = os.path.join(root, f); out.append(('romfs/' + os.path.relpath(p, romfs), p))
    return out

def deploy(a):
    ext = f'/sdcard/Android/data/{package(a.id)}/files'
    if a.from_device:   # copied on the device: much faster than over USB
        run('adb', 'shell', f'mkdir -p {ext} && for d in romfs dm vk font; do rm -rf {ext}/$d; cp -r {a.from_device}/$d {ext}/$d; done')
    else:   # adb cannot make folders inside Android/data (Android 13: secure_mkdirs is refused): pushed to /data/local/tmp, then copied on the device
        b = a.build; vk = glob.glob(os.path.join(b, 'shader', 'vk'))[0]; tmp = '/data/local/tmp/nx_deploy'
        st = tempfile.mkdtemp()
        for d in ('dm', 'vk'): os.makedirs(os.path.join(st, d))
        for f in glob.glob(os.path.join(b, 'patched_android', '*.patched.bin')) + glob.glob(os.path.join(b, 'patched_android', '*.tsv')): os.symlink(f, os.path.join(st, 'dm', os.path.basename(f)))
        for f in glob.glob(os.path.join(vk, '*.spv')) + glob.glob(os.path.join(vk, '*.vk.txt')): os.symlink(f, os.path.join(st, 'vk', os.path.basename(f)))
        os.symlink(os.path.join(b, 'font'), os.path.join(st, 'font')); os.symlink(glob.glob(os.path.join(b, 'extract', '*', 'RomFS'))[-1], os.path.join(st, 'romfs'))
        run('adb', 'shell', f'rm -rf {tmp}; mkdir -p {tmp} {ext}')
        for d in ('dm', 'vk', 'font', 'romfs'):
            run('adb', 'push', os.path.realpath(os.path.join(st, d)) if d in ('font', 'romfs') else os.path.join(st, d), f'{tmp}/{d}', stdout=subprocess.DEVNULL)
            run('adb', 'shell', f'rm -rf {ext}/{d} && cp -r {tmp}/{d} {ext}/{d} && rm -rf {tmp}/{d}')
        run('adb', 'shell', f'rm -rf {tmp}'); shutil.rmtree(st)
    print(f'game files in {ext}')

ap = argparse.ArgumentParser()
if len(sys.argv) > 1 and sys.argv[1] == '--deploy':
    ap.add_argument('--deploy', action='store_true'); ap.add_argument('id'); ap.add_argument('build'); ap.add_argument('--from-device')
    deploy(ap.parse_args())
else:
    ap.add_argument('id'); ap.add_argument('build'); ap.add_argument('out'); ap.add_argument('--save'); ap.add_argument('--cheats'); ap.add_argument('--label'); ap.add_argument('--bundle', action='store_true'); ap.add_argument('--test', action='store_true')
    zipfile.ZIP64_LIMIT = 0xFFFFFFFE   # Python writes ZIP64 records from 2 GB on; a plain ZIP holds offsets and sizes up to 4 GB
    build(ap.parse_args())
