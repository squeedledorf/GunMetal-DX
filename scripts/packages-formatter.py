#!/usr/bin/env python3
"""\
This module formats the package version and copyright information for the
viewer and its dependent packages (packages-info.txt, shown in the About box).

It reads the bookkeeping that indra/cmake/Prebuilt.cmake keeps for every
installed 3p package (<install_dir>/.3p/<name>.installed, plus the package's own
autobuild-package.xml metadata saved as <name>.xml) and the 3p
manifest (indra/cmake/3p_manifest.json) for the viewer's own copyright line --
the same data `autobuild install --versions/--copyrights` used to print.

$LicenseInfo:firstyear=2014&license=viewerlgpl$
Second Life Viewer Source Code
Copyright (C) 2014, Linden Research, Inc.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation;
version 2.1 of the License only.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
$/LicenseInfo$
"""
import argparse
import glob
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '3p'))
import llsd_xml  # noqa: E402

parser = argparse.ArgumentParser(description='Format dependency version and copyright information for the viewer About box content')
parser.add_argument('channel', help='viewer channel name')
parser.add_argument('version', help='viewer version number')
parser.add_argument('install_dir', help="install dir of packages")
parser.add_argument('--manifest', default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                      os.pardir, 'indra', 'cmake', '3p_manifest.json'),
                    help='3p manifest (for the viewer copyright and fallback package data)')
args = parser.parse_args()

with open(args.manifest) as f:
    manifest = json.load(f)
track = os.path.join(args.install_dir, '.3p')

versions, copyrights = {}, {}


def add(name, desc):
    # First one seen wins, top-level packages before the dependencies their
    # metadata lists (e.g. dullahan -> cef-bin), like autobuild's --versions.
    if name in versions:
        return
    versions[name] = (desc.get('version') or '').strip()
    copyrights[name] = (desc.get('copyright') or '').strip()


nested = []
for stamp in sorted(glob.glob(os.path.join(track, '*.installed'))):
    name = os.path.basename(stamp)[:-len('.installed')]
    meta_file = os.path.join(track, name + '.xml')
    if os.path.isfile(meta_file):
        meta = llsd_xml.parse(meta_file)
        add(name, meta.get('package_description', {}))
        nested.append(meta.get('dependencies', {}))
    else:
        add(name, manifest['packages'].get(name, {}))
while nested:
    deps = nested.pop(0)
    for name in sorted(deps):
        add(name, deps[name].get('package_description', {}))
        nested.append(deps[name].get('dependencies', {}))

missing = [name for name in versions if not copyrights[name]]
if missing:
    sys.exit("No copyright for %s" % ", ".join(sorted(missing)))

print("%s %s" % (args.channel, args.version))
print(manifest['viewer']['copyright'].strip())
print()
for pkg in sorted(versions):
    print(': '.join([pkg, versions[pkg]]))
    print(copyrights[pkg])
    print()
