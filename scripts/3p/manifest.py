#!/usr/bin/env python3
"""Maintain the hand-edited 3p manifest (indra/cmake/3p_manifest.json).

The manifest is the source of truth for the third-party prebuilt packages that
indra/cmake/Prebuilt.cmake (use_prebuilt_binary) downloads, verifies and unpacks.
Per package: version, license, license_file, copyright, description and, per platform
(linux64, windows64, darwin64, common), url + sha256 + hash/hash_algorithm.

    manifest.py list [--platform P]
    manifest.py add|update NAME --platform P --url URL [--version V] [--license L]
                [--license-file F] [--copyright C] [--description D]
    manifest.py remove NAME [--platform P]
    manifest.py verify [NAME...]

add/update downloads the archive (or reuses it from the cache), records its SHA256 and
BLAKE2b ('hash' + 'hash_algorithm', the identity Prebuilt.cmake uses to decide on a
reinstall), and rewrites the file with sorted keys and 2-space indent.
verify re-hashes every archive (cache: $SL_3P_CACHE or ~/.cache/slipstream-3p, files
named <basename> or <name>-<basename>; downloaded into the cache when absent).

Standard library only.
"""
import argparse
import hashlib
import json
import os
import sys
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir, os.pardir))
MANIFEST = os.path.join(ROOT, 'indra', 'cmake', '3p_manifest.json')
CACHE = os.environ.get('SL_3P_CACHE') or os.path.join(os.path.expanduser('~'), '.cache', 'slipstream-3p')
PKG_FIELDS = ('version', 'license', 'license_file', 'copyright', 'description')


def basename(url):
    return os.path.basename(url.split('?', 1)[0])


def digest(path, algo):
    h = hashlib.new(algo)
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def download(url, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    req = urllib.request.Request(url, headers={'Accept': 'application/octet-stream',
                                               'User-Agent': 'slipstream-3p-manifest'})
    print('download %s' % url, flush=True)
    with urllib.request.urlopen(req, timeout=300) as r, open(dest + '.part', 'wb') as f:
        for block in iter(lambda: r.read(1 << 20), b''):
            f.write(block)
    os.replace(dest + '.part', dest)


def cached(name, url):
    base = basename(url)
    for cand in (base, name + '-' + base):
        p = os.path.join(CACHE, cand)
        if os.path.isfile(p):
            return p
    return None


def load():
    with open(MANIFEST) as f:
        return json.load(f)


def save(doc):
    with open(MANIFEST + '.tmp', 'w') as f:
        json.dump(doc, f, indent=2, sort_keys=True)
        f.write('\n')
    os.replace(MANIFEST + '.tmp', MANIFEST)


def cmd_list(a):
    for name, pkg in sorted(load()['packages'].items()):
        plats = pkg.get('platforms', {})
        if a.platform and a.platform not in plats:
            continue
        print('%-28s %-16s %s' % (name, pkg.get('version', '?'), ','.join(sorted(plats))))


def cmd_add(a):
    doc = load()
    if a.cmd == 'update' and a.name not in doc['packages']:
        sys.exit('%s is not in the manifest (use add)' % a.name)
    pkg = doc['packages'].setdefault(a.name, {})
    path = cached(a.name, a.url)
    if path is None:
        path = os.path.join(CACHE, basename(a.url))
        download(a.url, path)
    pkg.setdefault('platforms', {})[a.platform] = {
        'url': a.url,
        'sha256': digest(path, 'sha256'),
        'hash': digest(path, 'blake2b'),
        'hash_algorithm': 'blake2b',
    }
    for field in PKG_FIELDS:
        v = getattr(a, field)
        if v is not None:
            pkg[field] = v
    save(doc)
    print('%s %s [%s]: sha256 %s' % (a.cmd, a.name, a.platform, pkg['platforms'][a.platform]['sha256']))


def cmd_remove(a):
    doc = load()
    pkg = doc['packages'].get(a.name)
    if pkg is None:
        sys.exit('%s is not in the manifest' % a.name)
    if a.platform:
        if pkg.get('platforms', {}).pop(a.platform, None) is None:
            sys.exit('%s has no platform %s' % (a.name, a.platform))
        if not pkg['platforms']:
            del doc['packages'][a.name]
    else:
        del doc['packages'][a.name]
    save(doc)


def cmd_verify(a):
    bad = 0
    for name, pkg in sorted(load()['packages'].items()):
        if a.names and name not in a.names:
            continue
        for plat, arc in sorted(pkg.get('platforms', {}).items()):
            path = cached(name, arc['url'])
            if path is None:
                path = os.path.join(CACHE, basename(arc['url']))
                try:
                    download(arc['url'], path)
                except Exception as e:
                    print('FAIL %s/%s: cannot download: %s' % (name, plat, e))
                    bad += 1
                    continue
            problems = []
            if arc.get('sha256') and digest(path, 'sha256') != arc['sha256'].lower():
                problems.append('sha256')
            if arc.get('hash') and digest(path, arc['hash_algorithm']) != arc['hash'].lower():
                problems.append(arc['hash_algorithm'])
            if not arc.get('sha256') and not arc.get('hash'):
                problems.append('no hash recorded')
            print('%s %s/%s%s' % ('FAIL' if problems else 'ok  ', name, plat,
                                  ' (' + ', '.join(problems) + ' mismatch)' if problems else ''))
            bad += bool(problems)
    sys.exit(1 if bad else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('list')
    p.add_argument('--platform')
    p.set_defaults(fn=cmd_list)
    for c in ('add', 'update'):
        p = sub.add_parser(c)
        p.add_argument('name')
        p.add_argument('--platform', required=True, choices=('linux64', 'windows64', 'darwin64', 'common'))
        p.add_argument('--url', required=True)
        p.add_argument('--version')
        p.add_argument('--license')
        p.add_argument('--license-file', dest='license_file')
        p.add_argument('--copyright')
        p.add_argument('--description')
        p.set_defaults(fn=cmd_add)
    p = sub.add_parser('remove')
    p.add_argument('name')
    p.add_argument('--platform')
    p.set_defaults(fn=cmd_remove)
    p = sub.add_parser('verify')
    p.add_argument('names', nargs='*')
    p.set_defaults(fn=cmd_verify)
    a = ap.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
