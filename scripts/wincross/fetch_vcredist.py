#!/usr/bin/env python3
"""Extract the x64 Visual C++ runtime DLLs (msvcp140*, vcruntime140*) from
Microsoft's redistributable vc_redist.x64.exe, for the Windows cross build
(no Visual Studio install to copy them from). Needs 7z.

usage: fetch_vcredist.py <out_dir>     (e.g. ~/.cache/wincross/crt)
"""
import glob, os, shutil, struct, subprocess, sys, tempfile, urllib.request

URL = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'
out = os.path.expanduser(sys.argv[1]); os.makedirs(out, exist_ok=True)
work = tempfile.mkdtemp(prefix='vcredist-')
exe = os.path.join(work, 'vc_redist.x64.exe')
urllib.request.urlretrieve(URL, exe)
data = open(exe, 'rb').read()
# The burn bundle appends its payload container as cabinet archives: carve them out.
i = n = 0
while (i := data.find(b'MSCF\0\0\0\0', i)) >= 0:
    size = struct.unpack_from('<I', data, i + 8)[0]
    if 0 < size <= len(data) - i:
        cab = os.path.join(work, f'c{n}.cab'); open(cab, 'wb').write(data[i:i + size]); n += 1
        subprocess.run(['7z', 'x', '-y', f'-o{cab}.d', cab], stdout=subprocess.DEVNULL, check=False)
    i += 4
# Nested cabs inside the payload hold the DLLs as <name>.dll_amd64.
for inner in glob.glob(os.path.join(work, '*.d', '*')):
    if open(inner, 'rb').read(4) == b'MSCF':
        subprocess.run(['7z', 'x', '-y', f'-o{inner}.d', inner], stdout=subprocess.DEVNULL, check=False)
got = []
for f in glob.glob(os.path.join(work, '**', '*.dll_amd64'), recursive=True):
    name = os.path.basename(f)[:-len('_amd64')]
    if name.startswith(('msvcp140', 'vcruntime140')):
        shutil.copy2(f, os.path.join(out, name)); got.append(name)
shutil.rmtree(work)
print('CRT DLLs:', sorted(got))
sys.exit(0 if 'msvcp140.dll' in got and 'vcruntime140.dll' in got else 1)
