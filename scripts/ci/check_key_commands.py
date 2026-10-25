#!/usr/bin/env python3
"""Checks that every keyboard shortcut is a registered, rebindable command (#863).

Fails when
  * a `shortcut=` attribute sits in a XUI file that SLKeyCommands does not scan (menu
    accelerators are picked up per file, see menu_files[] in indra/newview/slkeycommands.cpp),
  * code asks SLKeyCommands::matches() about a command that key_commands.xml does not declare,
  * a command declared in key_commands.xml is never used by code (a dead entry in the
    Controls panel),
  * code compares a key with a modifier (Ctrl/Alt/Shift) directly instead of going through a
    command, and the comparison is not in scripts/ci/key_compare_allowlist.txt.

The allowlist holds the comparisons that deliberately stay hard-coded (standard text
editing and clipboard keys, dialog Enter/Escape, list navigation, ...), one per line:
    <file name> | <the line, stripped> | <reason>

Usage: scripts/ci/check_key_commands.py [--list-raw]   (run from anywhere in the checkout)
"""

import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
NEWVIEW = os.path.join(ROOT, 'indra', 'newview')
XUI = os.path.join(NEWVIEW, 'skins', 'default', 'xui', 'en')
COMMANDS_XML = os.path.join(XUI, 'key_commands.xml')
REGISTRY_CPP = os.path.join(NEWVIEW, 'slkeycommands.cpp')
ALLOWLIST = os.path.join(os.path.dirname(__file__), 'key_compare_allowlist.txt')

# Raw comparisons of a key with a modifier. Letter keys on their own are flagged too.
LETTER_KEY = re.compile(r"\bkey\s*==\s*'[A-Za-z0-9]'|'[A-Za-z0-9]'\s*==\s*key")
KEY_NAME = r"(?:KEY_\w+|'[^']+')"
KEY_CMP = re.compile(r"\bkey\s*==\s*%s|%s\s*==\s*key" % (KEY_NAME, KEY_NAME))
MODIFIER = re.compile(r"MASK_(?:CONTROL|ALT|SHIFT)")


def scanned_menu_files():
    with open(REGISTRY_CPP, encoding='utf-8') as f:
        text = f.read()
    return set(re.findall(r'\{\s*"([\w.]+\.xml)"\s*,', text))


def check_shortcut_files(errors):
    scanned = scanned_menu_files()
    for name in sorted(os.listdir(XUI)):
        if not name.endswith('.xml') or name == 'key_commands.xml':
            continue
        path = os.path.join(XUI, name)
        with open(path, encoding='utf-8') as f:
            text = f.read()
        if re.search(r'\bshortcut\s*=\s*"', text) and name not in scanned:
            errors.append('%s has shortcut= attributes but is not in menu_files[] of slkeycommands.cpp'
                          ' (its shortcuts would not be rebindable)' % name)


def declared_commands():
    root = ET.parse(COMMANDS_XML).getroot()
    return {c.get('name') for c in root.findall('command')}


def source_files():
    for dirpath, _dirs, files in os.walk(NEWVIEW):
        if os.sep + 'skins' + os.sep in dirpath + os.sep or 'vkgfx' in dirpath:
            continue
        for name in files:
            if name.endswith(('.cpp', '.h')):
                yield os.path.join(dirpath, name)


def logical_lines(path):
    """Source lines, a line that continues with && or || joined to the next one."""
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    i = 0
    while i < len(lines):
        start = i
        text = lines[i]
        while (re.search(r'(&&|\|\|)\s*$', text) or (i + 1 < len(lines) and re.match(r'\s*(&&|\|\|)', lines[i + 1]))) and i + 1 < len(lines) and i - start < 4:
            i += 1
            text += ' ' + lines[i].strip()
        yield start + 1, text
        i += 1


def load_allowlist():
    allowed = set()
    if not os.path.exists(ALLOWLIST):
        return allowed
    with open(ALLOWLIST, encoding='utf-8') as f:
        for raw in f:
            raw = raw.strip()
            if not raw or raw.startswith('#'):
                continue
            # the code part can contain '|' itself
            if raw.count(' | ') >= 2:
                base, rest = raw.split(' | ', 1)
                code, _reason = rest.rsplit(' | ', 1)
                allowed.add((base.strip(), code.strip()))
    return allowed


def check_code(errors, list_raw):
    declared = declared_commands()
    used = set()
    allowed = load_allowlist()
    raw = []
    for path in source_files():
        base = os.path.basename(path)
        if base in ('slkeycommands.cpp', 'slkeycommands.h'):
            continue
        for lineno, text in logical_lines(path):
            stripped = text.strip()
            if stripped.startswith('//'):
                continue
            for m in re.finditer(r'matches\(\s*"([\w:./ -]+)"', stripped):
                used.add(m.group(1))
                if m.group(1) not in declared:
                    errors.append('%s:%d asks for the key command "%s", which key_commands.xml does not declare'
                                  % (base, lineno, m.group(1)))
            code = re.sub(r'//.*$', '', stripped)
            flagged = LETTER_KEY.search(code) or (KEY_CMP.search(code) and MODIFIER.search(code))
            if flagged and (base, re.sub(r'\s+', ' ', code)) not in allowed:
                raw.append((base, lineno, re.sub(r'\s+', ' ', code)))
    for name in sorted(declared - used):
        errors.append('key command "%s" is declared in key_commands.xml but no code uses it' % name)
    if list_raw:
        for base, lineno, code in raw:
            print('%s | %s | reason' % (base, code))
    for base, lineno, code in raw:
        errors.append('%s:%d compares a key directly (%s); make it a command in key_commands.xml and use'
                      ' SLKeyCommands::instance().matches(), or list it in key_compare_allowlist.txt with a reason'
                      % (base, lineno, code))


def main():
    errors = []
    check_shortcut_files(errors)
    check_code(errors, '--list-raw' in sys.argv)
    if errors:
        for e in errors:
            print('ERROR: ' + e)
        print('%d problem(s)' % len(errors))
        return 1
    print('key commands OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
