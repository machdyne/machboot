#!/usr/bin/env python3
"""
Gather a machboot release (`make release` builds everything first):

  build/release/machboot-<version>/     the files, flat, as they go on GitHub
  build/release/machboot-<version>.zip  the same in one archive

The version is fw/mb.h's MB_VERSION_STR. docs/quickstart.md goes in as
QUICKSTART.md and is also the release's description
(tools/gh-release.sh).

Usage: mkrelease.py [build-dir]
"""
import hashlib, os, re, shutil, sys, zipfile

B = sys.argv[1] if len(sys.argv) > 1 else 'build'
VER = re.search(r'MB_VERSION_STR\s+"([^"]+)"', open('fw/mb.h').read()).group(1)
NAME = 'machboot-%s' % VER

FILES = (
    ['machboot-sv.uf2'] +
    ['machboot-%s_ml%d.uf2' % (c, m) for c in ('sergei', 'mozart') for m in (0, 1, 2)] +
    ['machboot-%s_ml%d-sram.bit' % (c, m) for c in ('sergei', 'mozart') for m in (0, 1, 2)] +
    ['machboot-%s%s' % (b, s) for b in ('lakritz', 'konfekt')
     for s in ('-install.bin', '.bin', '-sram.bit')]
)
EXTRA = [('docs/quickstart.md', 'QUICKSTART.md'), ('LICENSE.md', 'LICENSE.md'),
         ('tools/mksys.py', 'mksys.py')]

def main():
    missing = [f for f in FILES if not os.path.exists(os.path.join(B, f))]
    if missing:
        sys.exit('mkrelease: not built: %s (make release builds them all)' % ', '.join(missing))
    out = os.path.join(B, 'release', NAME)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    for f in FILES:
        shutil.copy(os.path.join(B, f), out)
    for src, dst in EXTRA:
        shutil.copy(src, os.path.join(out, dst))
    names = sorted(os.listdir(out))
    with open(os.path.join(out, 'SHA256SUMS'), 'w') as s:
        for n in names:
            s.write('%s  %s\n' % (hashlib.sha256(open(os.path.join(out, n), 'rb').read()).hexdigest(), n))
    z = os.path.join(B, 'release', NAME + '.zip')
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for n in sorted(os.listdir(out)):
            zf.write(os.path.join(out, n), os.path.join(NAME, n))
    print('%s: %d files' % (out, len(os.listdir(out))))
    print('%s: %d bytes' % (z, os.path.getsize(z)))

if __name__ == '__main__':
    main()
