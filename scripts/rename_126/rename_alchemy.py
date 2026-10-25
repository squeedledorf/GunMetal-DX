#!/usr/bin/env python3
"""#126: rename internal Alchemy names to Slipstream.

Run from the repository root. Each step is idempotent (running it again on an
already-renamed tree changes nothing):

    python3 scripts/rename_126/rename_alchemy.py settings   # Alchemy* settings keys
    python3 scripts/rename_126/rename_alchemy.py sources    # al*.cpp/.h -> sl*, AL* classes
    python3 scripts/rename_126/rename_alchemy.py tokens     # other Alchemy* identifiers (XUI, notifications, ...)

`settings` needs the 'llsd' Python package (pip install llsd) for the importer map.

settings
    Collects every <key>Alchemy...</key> from app_settings/settings.xml and
    settings_per_account.xml, plus the Alchemy* names the code reads through an
    LLCachedControl without a declaration (UNDECLARED below), derives the new
    Slipstream* name (SPECIAL overrides), refuses on a collision with a key that
    already exists, writes the old -> new table to app_settings/settings_renames.xml
    (read by the first-launch migration, SLSettingsMigration) and rewrites every
    reference under indra/, scripts/ and doc/. KEEP_KEYS are left alone: they
    belong to the Photo Studio / Lightbox rename (#444), which migrates them
    together with saved Looks.

    The viewer importer map (viewer_import_map.xml) is edited through llsd: the
    targets of the Firestorm / Black Dragon renames follow our new names, and a
    rename entry AlchemyX -> SlipstreamX (viewers: alchemy) is added for every
    renamed key so importing from an Alchemy install still lands on our settings.
    The source side (other viewers' names) is never touched.

sources
    git mv of our al*.cpp/.h files in indra/newview to sl*, AL* class names to SL*,
    include guards, includes and CMakeLists.txt. SKIP_SOURCES (the llui al* widget
    framework, #443, and the Photo Studio files, #444) are not touched.

tokens
    Remaining Alchemy* identifiers that are ours: notification names, menu
    callbacks, string names, texture names.
"""
import os, re, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
APP = os.path.join(ROOT, 'indra', 'newview', 'app_settings')
NEWVIEW = os.path.join(ROOT, 'indra', 'newview')
RENAMES_XML = os.path.join(APP, 'settings_renames.xml')
IMPORT_MAP = os.path.join(APP, 'viewer_import_map.xml')
THIS = os.path.relpath(os.path.abspath(__file__), ROOT)

# Settings owned by Photo Studio / Lightbox (#444): renamed there with the Looks migration.
KEEP_KEYS = {
    'AlchemyRenderTonemapType',      # carried by every Look (looks/*.xml, getLooksControlNames)
    'AlchemyScopeLayout', 'AlchemyScopeLogScale', 'AlchemyScopeSampleInterval',
    'AlchemyScopeSampleWidth', 'AlchemyScopeSmoothing',
}
# Read through LLCachedControl with a code default but never declared in settings.xml.
# They get a declaration under the new name (so the migration can set them before the
# code first reads them) -- see declare_undeclared().
UNDECLARED = {
    # name: (type, value, comment)
    'AlchemyLookAtDebug': ('Boolean', 0, 'Show look-at debug information (look-at target type and owner) on look-at beacons'),
    'AlchemyMouselookTextOffsetX': ('F32', 0.0, 'Mouselook IFF name label: horizontal offset in pixels from the crosshair'),
    'AlchemyMouselookTextOffsetY': ('F32', -25.0, 'Mouselook IFF name label: vertical offset in pixels from the crosshair'),
    'AlchemyMouselookTextHAlign': ('U32', 2, 'Mouselook IFF name label alignment: 0 = left, 1 = right, 2 = centred'),
    'AlchemyRecentPeopleMaxAge': ('S32', 30, 'Days a person stays in People > Recent (0 = forever)'),
}
# Old name -> new name where the plain prefix swap is wrong.
SPECIAL = {
    # settings.xml declared "MouseLook" but lltoolcomp.cpp reads "Mouselook" (so the declared
    # value was never used). Both spellings migrate to the one the code reads.
    'AlchemyMouseLookZoomTime': 'SlipstreamMouselookZoomTime',
    'AlchemyMouseLookZoomTimeout': 'SlipstreamMouselookZoomTimeout',
    'AlchemyMouselookZoomTime': 'SlipstreamMouselookZoomTime',
    'AlchemyMouselookZoomTimeout': 'SlipstreamMouselookZoomTimeout',
}
# Declared names whose new name is produced by another (alias) entry above: do not
# declare twice. The surviving declaration is the renamed declared key.
TEXT_EXT = ('.cpp', '.h', '.hpp', '.mm', '.inl', '.xml', '.txt', '.py', '.glsl', '.frag', '.vert',
            '.comp', '.md', '.cmake', '.sh', '.json', '.ini', '.yml', '.yaml', '.strings', '.plist', '.rc')
# Never rewritten by the token passes.
EXCLUDE = {
    'indra/newview/app_settings/settings_renames.xml',
    'indra/newview/app_settings/viewer_import_map.xml',   # edited through llsd (source side must stay)
    THIS,
}


def git_files(*roots):
    out = subprocess.check_output(['git', 'ls-files', *roots], cwd=ROOT).decode()
    return [f for f in out.split('\n') if f]


def text_files():
    for f in git_files('indra', 'scripts', 'doc'):
        if f in EXCLUDE or not f.endswith(TEXT_EXT):
            continue
        p = os.path.join(ROOT, f)
        if os.path.isfile(p) and not os.path.islink(p):
            yield f, p


def read(p):
    with open(p, 'rb') as fh:
        return fh.read()


def write(p, data):
    with open(p, 'wb') as fh:
        fh.write(data)


def replace_tokens(mapping, prefix=b'Alchemy'):
    """Replace whole alphanumeric runs starting with prefix (stop at '_' so XUI names like
    AlchemyChatCommandPos_txt follow the setting). Returns {file: count}."""
    rx = re.compile(rb'(?<![A-Za-z0-9])' + prefix + rb'[A-Za-z0-9]*')
    bmap = {k.encode(): v.encode() for k, v in mapping.items()}
    changed = {}
    for f, p in text_files():
        data = read(p)
        if prefix not in data:
            continue
        n = 0

        def sub(m):
            nonlocal n
            new = bmap.get(m.group(0))
            if new is None:
                return m.group(0)
            n += 1
            return new
        out = rx.sub(sub, data)
        if n:
            write(p, out)
            changed[f] = n
    return changed


# ---------------------------------------------------------------------------- settings

def declared_keys():
    keys = {}
    for name in ('settings.xml', 'settings_per_account.xml', 'settings_crash_behavior.xml',
                 'settings_minimal.xml'):
        p = os.path.join(APP, name)
        if not os.path.exists(p):
            continue
        for k in re.findall(r'<key>([A-Za-z0-9_]+)</key>\s*\n\s*<map>', read(p).decode('utf-8')):
            keys.setdefault(k, name)
    return keys


def build_settings_map():
    keys = declared_keys()
    if os.path.exists(RENAMES_XML):
        # Already applied: the table is the record of what was renamed.
        import llsd
        return dict(llsd.parse(read(RENAMES_XML))['renames'])
    old = sorted(k for k in keys if k.startswith('Alchemy') and k not in KEEP_KEYS)
    old += sorted(k for k in list(UNDECLARED) + list(SPECIAL) if k not in old)
    mapping = {}
    for k in old:
        mapping[k] = SPECIAL.get(k, 'Slipstream' + k[len('Alchemy'):])
    collisions = [(o, n) for o, n in mapping.items() if n in keys]
    if collisions:
        sys.exit('collision with existing keys: %s' % collisions)
    return mapping


def write_renames(mapping):
    import llsd
    doc = {
        'version': 1,
        'notes': '#126: settings renamed from Alchemy* to Slipstream* in 1.2. Old name -> new name. '
                 'Read at startup by SLSettingsMigration: values saved under an old name in '
                 'user_settings/settings.xml, the per-account settings_per_account.xml and saved '
                 'graphics / camera presets are carried over to the new name. Generated by '
                 'scripts/rename_126/rename_alchemy.py; do not edit by hand.',
        'renames': dict(sorted(mapping.items())),
    }
    write(RENAMES_XML, llsd.format_pretty_xml(doc))


def declare_undeclared(mapping):
    """Give the LLCachedControl-only settings a declaration under the new name."""
    p = os.path.join(APP, 'settings.xml')
    data = read(p).decode('utf-8')
    add = []
    for old, (typ, val, comment) in UNDECLARED.items():
        new = mapping[old]
        if '<key>%s</key>' % new in data:
            continue
        if typ == 'Boolean' or typ in ('U32', 'S32'):
            v = '<integer>%d</integer>' % int(val)
        else:
            v = '<real>%s</real>' % repr(float(val))
        add.append('''    <key>%s</key>
    <map>
      <key>Comment</key>
      <string>%s</string>
      <key>Persist</key>
      <integer>1</integer>
      <key>Type</key>
      <string>%s</string>
      <key>Value</key>
      %s
    </map>
''' % (new, comment, typ, v))
    if not add:
        return 0
    tail = '</map>\n</llsd>'
    i = data.rstrip().rfind(tail)
    if i < 0:
        sys.exit('settings.xml: unexpected tail')
    data = data[:i] + ''.join(add) + data[i:]
    write(p, data.encode('utf-8'))
    return len(add)


def update_import_map(mapping):
    import llsd
    m = llsd.parse(read(IMPORT_MAP))
    s = m['settings']

    def tgt(name):
        return mapping.get(name, name)
    n = 0
    for src, r in s['rename'].items():
        if r.get('to') in mapping:
            r['to'] = tgt(r['to']); n += 1
        cond = r.get('convert', {}).get('if_source')
        if cond and cond.get('setting') in mapping:
            pass  # if_source names a setting of the *source* viewer: leave it
    for key in ('ranges', 'enums', 'validate'):
        if key in s:
            s[key] = {tgt(k): v for k, v in s[key].items()}
    for key in ('render_allow', 'deny_exact', 'restart'):
        if key in s:
            s[key] = [tgt(k) for k in s[key]]
    # Importing from an Alchemy install: its settings still carry the old names.
    for old, new in sorted(mapping.items()):
        if old not in s['rename']:
            s['rename'][old] = {'to': new, 'viewers': ['alchemy'],
                                'note': 'Alchemy name of this setting (renamed in Slipstream 1.2, #126)'}
    write(IMPORT_MAP, llsd.format_pretty_xml(m))
    return n


def step_settings():
    mapping = build_settings_map()
    write_renames(mapping)
    changed = replace_tokens(mapping)
    added = declare_undeclared(mapping)
    n_import = update_import_map(mapping)
    print('settings renamed: %d (incl. %d undeclared/alias names)' % (len(mapping), len(UNDECLARED) + 2))
    print('kept for #444: %s' % ', '.join(sorted(KEEP_KEYS)))
    print('declarations added: %d; import map targets updated: %d' % (added, n_import))
    print('references rewritten: %d in %d files' % (sum(changed.values()), len(changed)))
    for f in sorted(changed):
        print('  %4d %s' % (changed[f], f))


# ---------------------------------------------------------------------------- sources

# Our al*.cpp/.h in indra/newview -> sl*. Photo Studio / Lightbox files (#444) and the
# llui al* widget framework + llcommon/alparamtype (#443, to be removed) are not touched.
SOURCES = ['alassetblocklist', 'alavataractions', 'alchatbar', 'alchatcommand', 'alfloatergenerictext',
           'alfloaterparticleeditor', 'alfloaterregiontracker', 'alpanelmusicticker', 'alpanelradaralert',
           'alpanelstreaminfo', 'alstreaminfo', 'altoolalign', 'altpscamera', 'alunzip', 'alviewermenu']
SKIP_SOURCES = ['alcolorwheelctrl', 'alcolorwheelmodel', 'alcurveeditorctrl', 'alcurvemodel',
                'aldaycyclelandmarks', 'alfloaterlightbox', 'alfloaterscopes', 'algradehistory',
                'allightboxdirectory', 'alscopedata', 'altoolscenepicker', 'alwhitebalancesolver',
                'llui/al*', 'llcommon/alparamtype']
XUI_FILES = {'floater_al_chatbar.xml': 'floater_sl_chatbar.xml',
             'menu_al_chatbar_gear.xml': 'menu_sl_chatbar_gear.xml'}
CLASSES = ['ALChatBar', 'ALChatBarGestureObserver', 'ALAvatarActions', 'ALTPSCamera', 'ALToolTPS',
           'ALFloaterParticleEditor', 'ALFloaterRegionTracker', 'ALPanelMusicTicker', 'ALToolAlign',
           'ALAssetBlocklist', 'ALBlockedAsset', 'ALUnZip', 'ALStreamInfo', 'ALChatCommand',
           'ALPanelStreamInfo', 'ALPanelRadarAlert', 'ALViewerMenu', 'ALToggleLocationBar',
           'ALCheckLocationBar', 'ALCinematicMode']
GUARDS = {'AL_ALAVATARACTIONS_H': 'SL_SLAVATARACTIONS_H', 'AL_LLVIEWERMENU_H': 'SL_SLVIEWERMENU_H',
          'AL_ALCHATCOMMAND_H': 'SL_SLCHATCOMMAND_H', 'AL_UNZIP_H': 'SL_UNZIP_H',
          'AL_ALTOOLALIGN_H': 'SL_SLTOOLALIGN_H', 'AL_TPSCAMERA_H': 'SL_TPSCAMERA_H',
          'AL_PANELSTREAMINFO_H': 'SL_PANELSTREAMINFO_H', 'AL_STREAMINFO_H': 'SL_STREAMINFO_H',
          'AL_FLOATERPARTICLEEDITOR_H': 'SL_FLOATERPARTICLEEDITOR_H',
          'AL_PANELMUSICTICKER_H': 'SL_PANELMUSICTICKER_H',
          'ALUNZIP': 'SLUNZIP'}  # ALUNZIP: log tag


def sub_words(mapping, files=None):
    rx = re.compile(rb'\b(' + b'|'.join(re.escape(k.encode()) for k in sorted(mapping, key=len, reverse=True)) + rb')\b')
    bmap = {k.encode(): v.encode() for k, v in mapping.items()}
    changed = {}
    for f, p in (files if files is not None else text_files()):
        data = read(p)
        out, n = rx.subn(lambda m: bmap[m.group(1)], data)
        if n:
            write(p, out)
            changed[f] = n
    return changed


def step_sources():
    moved = 0
    for base in SOURCES:
        for ext in ('.cpp', '.h'):
            src = os.path.join('indra', 'newview', base + ext)
            dst = os.path.join('indra', 'newview', 'sl' + base[2:] + ext)
            if os.path.exists(os.path.join(ROOT, src)):
                subprocess.check_call(['git', 'mv', src, dst], cwd=ROOT)
                moved += 1
    xui = os.path.join('indra', 'newview', 'skins', 'default', 'xui')
    for lang in sorted(os.listdir(os.path.join(ROOT, xui))):
        for old, new in XUI_FILES.items():
            src = os.path.join(xui, lang, old)
            if os.path.exists(os.path.join(ROOT, src)):
                subprocess.check_call(['git', 'mv', src, os.path.join(xui, lang, new)], cwd=ROOT)
                moved += 1
    words = {}
    for base in SOURCES:
        for ext in ('.cpp', '.h'):
            words[base + ext] = 'sl' + base[2:] + ext
    words.update(XUI_FILES)
    words.update({c: 'SL' + c[2:] for c in CLASSES})
    words.update(GUARDS)
    changed = sub_words(words)
    print('files moved: %d; references rewritten: %d in %d files' % (moved, sum(changed.values()), len(changed)))
    for f in sorted(changed):
        print('  %4d %s' % (changed[f], f))


# ---------------------------------------------------------------------------- tokens

TOKENS = {
    # notifications
    'AlchemyUnpoweredWizard': 'SlipstreamUnpoweredWizard',
    'AlchemyExplosions': 'SlipstreamExplosions',
    'AlchemyDestroyObject': 'SlipstreamDestroyObject',
    # menu callbacks (Object.AlchemyX)
    'AlchemyExplode': 'SlipstreamExplode',
    'AlchemyDestroy': 'SlipstreamDestroy',
    'AlchemyForceDelete': 'SlipstreamForceDelete',
    # strings.xml
    'AlchemyTPSCameraHint': 'SlipstreamTPSCameraHint',
    # About-floater logos (textures.xml)
    'AlchemyRelease128': 'SlipstreamRelease128',
    'AlchemyBeta128': 'SlipstreamBeta128',
    'AlchemyProject128': 'SlipstreamProject128',
    'AlchemyTest128': 'SlipstreamTest128',
    # Windows minidump file name prefix
    'AlchemyException': 'SlipstreamException',
    # C++ locals named after their setting
    'AlchemyRejectTeleportOffers': 'SlipstreamRejectTeleportOffers',
    'AlchemyRejectFriendshipRequests': 'SlipstreamRejectFriendshipRequests',
}


def step_tokens():
    changed = replace_tokens(TOKENS)
    # Menu enable callback "Alchemy.PowerfulWizardObject" (dot: not a token run).
    extra = sub_words({'Alchemy.PowerfulWizardObject': 'Slipstream.PowerfulWizardObject'})
    for f, n in extra.items():
        changed[f] = changed.get(f, 0) + n
    print('tokens rewritten: %d in %d files' % (sum(changed.values()), len(changed)))
    for f in sorted(changed):
        print('  %4d %s' % (changed[f], f))


if __name__ == '__main__':
    steps = {'settings': step_settings, 'sources': step_sources, 'tokens': step_tokens}
    if len(sys.argv) != 2 or sys.argv[1] not in steps:
        sys.exit('usage: %s {%s}' % (sys.argv[0], '|'.join(steps)))
    os.chdir(ROOT)
    steps[sys.argv[1]]()
