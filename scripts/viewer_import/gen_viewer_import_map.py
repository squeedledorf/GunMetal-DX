#!/usr/bin/env python3
"""Regenerate indra/newview/app_settings/viewer_import_map.xml (#231).

The importer reads that LLSD file at runtime; this script is how it is maintained:
edit the tables below (renames, deny lists, renderer allowlist, toolbar / key
command renames), then run from the repository root:

    python3 scripts/viewer_import/gen_viewer_import_map.py

Ranges and enums are scraped from our preference panels (skins/default/xui/en:
slider / spinner min_val..max_val, combo / radio item values) so they follow the UI.
Every rename target and allowlisted name is checked against our settings*.xml.
Needs the 'llsd' Python package (pip install llsd).

Mapping research: Firestorm phoenix-firestorm 4ae31a7ad69d (2026-10-01),
Black Dragon Black-Dragon-Viewer Release_5.6.3 b2ca434b39bc.
"""
import glob, os, re, sys
import xml.etree.ElementTree as ET
import llsd

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
APP = os.path.join(ROOT, 'indra', 'newview', 'app_settings')
XUI = os.path.join(ROOT, 'indra', 'newview', 'skins', 'default', 'xui', 'en')

def load_settings(name):
    with open(os.path.join(APP, name), 'rb') as f:
        return llsd.parse(f.read())

ours = set(load_settings('settings.xml')) | set(load_settings('settings_per_account.xml'))

def scrape_xui():
    ranges, enums = {}, {}
    for f in glob.glob(os.path.join(XUI, '*.xml')):
        try:
            root = ET.parse(f).getroot()
        except ET.ParseError:
            continue
        for el in root.iter():
            cn = el.get('control_name')
            if not cn:
                continue
            if el.tag in ('slider', 'slider_bar', 'spinner', 'multi_slider'):
                try:
                    mn, mx = float(el.get('min_val')), float(el.get('max_val'))
                except (TypeError, ValueError):
                    continue
                r = ranges.setdefault(cn, [mn, mx])
                r[0], r[1] = min(r[0], mn), max(r[1], mx)
            elif el.tag in ('combo_box', 'radio_group') and el.get('allow_text_entry') != 'true':
                vals = [it.get('value') for it in el if it.tag in ('combo_box.item', 'radio_item', 'item') and it.get('value') is not None]
                for v in vals:
                    enums.setdefault(cn, [])
                    if v not in enums[cn]:
                        enums[cn].append(v)
    return {'ranges': ranges, 'enums': enums}

rng = scrape_xui()
FS = 'firestorm'; BD = 'blackdragon'

def r(to, viewers=None, convert=None, note=None):
    d = {'to': to}
    if viewers: d['viewers'] = viewers
    if convert: d['convert'] = convert
    if note: d['note'] = note
    return d

slash = {'type': 'prefix', 'prefix': '/'}
rename = {
    # Firestorm chat-bar command line -> our chat commands (ours keep the leading '/').
    'FSCmdLine': r('SlipstreamChatCommandEnable', [FS]),
    'FSCmdLineAO': r('SlipstreamChatCommandAnimationOverride', [FS], slash),
    'FSCmdLineCalc': r('SlipstreamChatCommandCalc', [FS], slash),
    'FSCmdLineClearChat': r('SlipstreamChatCommandClearNearby', [FS], slash),
    'FSCmdLineDrawDistance': r('SlipstreamChatCommandDrawDistance', [FS], slash),
    'FSCmdLineGround': r('SlipstreamChatCommandGround', [FS], slash),
    'FSCmdLineHeight': r('SlipstreamChatCommandHeight', [FS], slash),
    'FSCmdLineMapTo': r('SlipstreamChatCommandMapto', [FS], slash),
    'FSCmdLinePos': r('SlipstreamChatCommandPos', [FS], slash),
    'FSCmdLineRezPlatform': r('SlipstreamChatCommandRezPlat', [FS], slash),
    'FSCmdLinePlatformSize': r('SlipstreamChatCommandRezPlatSize', [FS]),
    'FSCmdLineTeleportHome': r('SlipstreamChatCommandHome', [FS], slash),
    'FSCmdTeleportToCam': r('SlipstreamChatCommandTeleportToCam', [FS], slash),
    # Exodus / FS look-at and mouselook IFF (same code paths, renamed by Alchemy).
    'ExodusMouselookIFF': r('SlipstreamMouselookIFF', [FS]),
    'ExodusMouselookIFFRange': r('SlipstreamMouselookIFFRange', [FS]),
    'ExodusLookAtLines': r('SlipstreamLookAtLines', [FS]),
    'DebugLookAtShowNames': r('SlipstreamLookAtNames', [FS], note='Same values: 0 none, 1 display (user.name), 2 display, 3 First Last, 4 user.name'),
    'DebugLookAt': r('SlipstreamLookAtShow', [FS], {'type': 'bool'}, 'FS stores 0/1 per account; ours is a global on/off'),
    'DebugLookAtHideOwn': r('SlipstreamLookAtHideSelf', [FS]),
    # NaCl LSL preprocessor (FS) -> our port of it (by way of Alchemy).
    '_NACL_LSLPreprocessor': r('SlipstreamLSLPreprocessor', [FS]),
    '_NACL_PreProcEnableHDDInclude': r('SlipstreamPreProcEnableHDDInclude', [FS]),
    '_NACL_PreProcHDDIncludeLocation': r('SlipstreamPreProcHDDIncludeLocation', [FS]),
    '_NACL_PreProcLSLLazyLists': r('SlipstreamPreProcLSLLazyLists', [FS]),
    '_NACL_PreProcLSLOptimizer': r('SlipstreamPreProcLSLOptimizer', [FS]),
    '_NACL_PreProcLSLSwitch': r('SlipstreamPreProcLSLSwitch', [FS]),
    '_NACL_PreProcLSLTextCompress': r('SlipstreamPreProcLSLTextCompress', [FS]),
    # Minimap.
    'MiniMapChatRing': r('SlipstreamMinimapChatRings', [FS, BD]),
    'MiniMapCollisionParcels': r('SlipstreamMiniMapCollisionParcels', [FS]),
    'MiniMapForSaleParcels': r('SlipstreamMiniMapForSaleParcels', [FS]),
    'MiniMapObjects': r('SlipstreamMinimapRenderObjects', [FS]),
    # Chat / UI.
    'OnlineOfflinetoNearbyChat': r('SlipstreamOnlineOfflineToChat', [FS]),
    'FSStatusBarShowFPS': r('ShowStatusBarFPS', [FS]),
    'ShowFPSCounter': r('ShowStatusBarFPS', [BD], note='Black Dragon shows its FPS counter in the navigation bar; ours is in the status bar'),
    'FSShowMouselookInstructions': r('SlipstreamMouselookInstructions', [FS]),
    'FSUseStandaloneGroupFloater': r('ShowGroupFloaters', [FS]),
    'FSCopyObjKeySeparator': r('SlipstreamCopySeperator', [FS]),
    'FSStreamList': r('StreamList', [FS], note='Same layout: {"audio": [urls]}'),
    'PlayModeUISndTyping': r('PlayTypingSound', [FS]),
    'PlayModeUISndTeleportOut': r('PlayTeleportSound', [FS]),
    # Movement / camera / building.
    'RezUnderLandGroup': r('SlipstreamRezUnderLandGroup', [FS]),
    'FSIgnoreFinishAnimation': r('SlipstreamNimble', [FS], note='Both skip the pre-jump / landing wait'),
    'FSResetCameraOnMovement': r('SlipstreamMotionResetsCamera', [FS]),
    'FSSelectCopyableOnly': r('SelectCopyOnly', [FS]),
    'UseRealisticMouselook': r('SlipstreamRealisticMouselook', [BD]),
    'MouseWarpMode': r('DisableMouseWarp', [FS, BD], {'type': 'enum', 'map': {'0': False, '1': False, '2': True}},
                       'FS/BD: 0 automatic, 1 on, 2 off -> ours: off = disabled'),
    'FSFocusPointFollowsPointer': r('RenderFocusPointFollowsPointer', [FS]),
    'RenderOtherAttachedLights': r('RenderAttachedLights', [BD], note='Black Dragon splits own / other avatars; ours has one switch (follows "other")'),
    'FSRenderFriendsOnly': r('RenderAvatarFriendsOnly', [FS]),
    # Per-account: stored camera (camera tools), autoresponse, rejection.
    'FSStoredCameraPos': r('SlipstreamStoredCameraPos', [FS]),
    'FSStoredCameraFocus': r('SlipstreamStoredCameraFocus', [FS]),
    'FSStoredCameraFocusObjectId': r('SlipstreamStoredCameraFocusObjectId', [FS]),
    'ALStoredCameraRoll': r('SlipstreamStoredCameraRoll', [FS]),
    'FSAutorespondMode': r('SlipstreamAutoresponseEnable', [FS]),
    # Chat port (fschat): Firestorm's IM timestamp toggle is our IMShowTime.
    'FSShowTimestampsIM': r('IMShowTime', [FS]),
    'FSAutorespondModeResponse': r('SlipstreamAutoresponse', [FS]),
    'FSAutoResponseChanged': r('SlipstreamAutoresponseChanged', [FS]),
    'FSAutorespondNonFriendsMode': r('SlipstreamAutoresponseNotFriendEnable', [FS]),
    'FSAutorespondNonFriendsResponse': r('SlipstreamAutoresponseNotFriend', [FS]),
    'FSAutoResponseNonFriendsChanged': r('SlipstreamAutoresponseNotFriendChanged', [FS]),
    'FSRejectTeleportOffersMode': r('SlipstreamRejectTeleportOffersMode', [FS]),
    'FSRejectTeleportOffersResponse': r('SlipstreamRejectTeleportOffersResponse', [FS]),
    'FSRejectTeleportOffersResponseChanged': r('SlipstreamRejectTeleportOffersResponseChanged', [FS]),
    'FSRejectFriendshipRequestsMode': r('SlipstreamRejectFriendshipRequestsMode', [FS]),
    'FSRejectFriendshipRequestsResponse': r('SlipstreamRejectFriendshipRequestsResponse', [FS]),
    'FSRejectFriendshipRequestsResponseChanged': r('SlipstreamRejectFriendshipRequestsChanged', [FS]),
    'FSDontRejectTeleportOffersFromFriends': r('SlipstreamDontRejectTeleportOffersFromFriends', [FS]),
    'FSIgnoreAdHocSessions': r('SlipstreamIgnoreAdHocSessions', [FS],
        {'type': 'enum', 'map': {'0': 0, '1': 2},
         'if_source': {'setting': 'FSDontIgnoreAdHocFromFriends', 'value': True, 'map': {'0': 0, '1': 1}}},
        'FS on/off (+ "allow friends") -> ours 0 off, 1 allow friends, 2 reject all'),
    'FSReportIgnoredAdHocSession': r('SlipstreamReportIgnoredAdHocSession', [FS]),
}
# Importing from an Alchemy install (#126): our Alchemy* settings were renamed to
# Slipstream*; the Alchemy viewer still saves the old names. The table is the one the
# first-launch migration uses (app_settings/settings_renames.xml).
for _old, _new in sorted(load_settings('settings_renames.xml')['renames'].items()):
    rename.setdefault(_old, {'to': _new, 'viewers': ['alchemy'],
                             'note': 'Alchemy name of this setting (renamed in Slipstream 1.2, #126)'})
missing = [v['to'] for v in rename.values() if v['to'] not in ours]
if missing: sys.exit('rename targets missing: %s' % missing)

render_allow = sorted(set([
    'RenderFarClip', 'RenderVolumeLODFactor', 'RenderAvatarLODFactor', 'RenderAvatarPhysicsLODFactor',
    'RenderTerrainLODFactor', 'RenderTreeLODFactor', 'RenderFlexTimeFactor',
    'RenderAvatarMaxComplexity', 'RenderAvatarMaxNonImpostors', 'RenderAvatarComplexityMode',
    'RenderAvatarFriendsOnly', 'RenderMaxPartCount', 'RenderShadowDetail', 'RenderDeferredSSAO',
    'RenderDepthOfField', 'RenderScreenSpaceReflections', 'RenderFSAAType', 'RenderFSAASamples',
    'RenderAnisotropic', 'RenderCASSharpness', 'RenderMaxTextureResolution', 'RenderVSyncEnable',
    'RenderAttachedLights', 'RenderAttachedParticles', 'RenderNameShowSelf', 'RenderNameFadeDuration',
    'RenderNameShowTime', 'RenderHideGroupTitle', 'RenderHUDInSnapshot', 'RenderUIInSnapshot',
    'RenderBalanceInSnapshot', 'RenderAutoMuteRenderWeightLimit', 'RenderAutoMuteSurfaceAreaLimit',
    'RenderHighlightSelections', 'RenderHiddenSelections', 'RenderHoverGlowEnable', 'RenderParcelSelection',
    'RenderTrackerBeacon', 'RenderFocusPointFollowsPointer',
]))
bad = [k for k in render_allow if k not in ours]
if bad: sys.exit('render_allow not ours: %s' % bad)

deny_exact = sorted(set([
    'WindowX', 'WindowY', 'WindowWidth', 'WindowHeight', 'WindowMaximized', 'FullScreen', 'FullScreenWidth',
    'FullScreenHeight', 'FullScreenWindow', 'UserLoginInfo', 'RememberPassword', 'RememberUser',
    'CurrentGrid', 'LastConnectedGrid', 'LoginPage', 'NextLoginLocation', 'ForceLoginURL',
    'InstantMessageLogPath', 'CacheLocation', 'CacheLocationTopFolder', 'NewCacheLocation',
    'NewCacheLocationTopFolder', 'CacheValidateCounter', 'PurgeCacheOnNextStartup', 'PurgeCacheOnStartup',
    'CrashSubmitBehavior', 'VersionChannelName', 'ForceAddressSize', 'ForceMandatoryUpdate',
    'InstallLanguage', 'TextureMemory', 'ProbeHardwareOnStartup', 'GPUClass', 'QAMode', 'QAModeEventHostPort',
    'NoAudio', 'NoVerifySSLCert', 'BrowserIgnoreSSLCertErrors', 'FirstLoginThisInstall', 'FirstRunThisInstall',
    'PresetGraphicActive', 'PresetCameraActive', 'PresetLooksActive', 'PresetLooksLastApplied',
    'ClientSettingsFile', 'MainloopTimeoutDefault', 'WatchdogEnabled', 'WindLightUseAtmosShaders',
    'VertexShaderEnable', 'WLSkyDetail', 'UseOcclusion', 'MaxHeapSize', 'TextureLoadFullRes',
    'DisableVerticalSync', 'SlipstreamWorldFrozen', 'LastLogoff', 'SlipstreamLegacyChatMerged',
    'RLVaLoginLastLocation', 'PreviousScreenshotForReport', 'ALLightboxState', 'SkinCurrent',
    'SkinCurrentTheme', 'CameraPreset', 'DiskCacheSize', 'TextureCacheSize', 'CacheSize',
    'AvatarSitRotation',
    # The other viewer's open IMs at its last logout (FSRestoreOpenIMs): session state.
    'FSLastOpenIMs',
    # Their voice backend's device identifiers do not name our WebRTC devices.
    'VoiceInputAudioDevice', 'VoiceOutputAudioDevice',
    # <GMDX> GunMetal-DX files bug reports through its own report window; another viewer's tracker link does not apply.
    'ReportBugURL',
]))
deny_regex = [
    '^Window(X|Y|Width|Height)$', '^FullScreen', 'SettingsFile$', '^(New)?CacheLocation',
    '^Last.*(Version|GPU|SystemInfo|Grid)', '^Updater', '^CmdLine', '^Debug', '^QAMode', '^SkinCurrent',
    '^FSInternal', '^Render(VK|GL)', '^RenderVK', '^MainloopTimeout', '^Watchdog', '^Crash',
    '^LoginLocation$', '^(Show|Open)Debug', '^Session', '^GridList',
]
deny_contains = ['password', 'token', 'apikey', 'secret', 'credential', 'oauth', 'nsid', 'logininfo']

# Ranges / enums for exactly the settings our preference panels expose (scraped from
# skins/default/xui/en: slider/spinner min_val/max_val, combo/radio item values).
ranges = {k: v for k, v in rng['ranges'].items() if k in ours}
enums = {k: v for k, v in rng['enums'].items() if k in ours}
# Hand additions where the panel is built in code or the value has a hard bound.
ranges.update({
    'RenderVolumeLODFactor': [0.5, 8.0], 'RenderAvatarLODFactor': [0.0, 1.0],
    'RenderTreeLODFactor': [0.0, 1.0], 'RenderTerrainLODFactor': [1.0, 2.0],
    'RenderAvatarPhysicsLODFactor': [0.0, 1.0], 'RenderFlexTimeFactor': [0.0, 1.0],
    'RenderMaxPartCount': [0, 8192], 'RenderAvatarMaxNonImpostors': [0, 66],
    'RenderAvatarMaxComplexity': [0, 2147483647], 'SlipstreamMouselookIFFRange': [0.0, 4096.0],
    'NearMeRange': [5.0, 4096.0], 'FSFontSizeAdjustment': [-2.0, 4.0], 'UIScaleFactor': [0.75, 4.0],
    'FontScreenDPI': [48.0, 300.0], 'ChatBubbleOpacity': [0.0, 1.0], 'PieMenuOpacity': [0.0, 1.0],
    'ActiveFloaterTransparency': [0.0, 1.0], 'InactiveFloaterTransparency': [0.0, 1.0],
    'AudioLevelMaster': [0.0, 1.0], 'AudioLevelMusic': [0.0, 1.0], 'AudioLevelMedia': [0.0, 1.0],
    'AudioLevelSFX': [0.0, 1.0], 'AudioLevelUI': [0.0, 1.0], 'AudioLevelAmbient': [0.0, 1.0],
    'AudioLevelVoice': [0.0, 1.0], 'MouseSensitivity': [0.0, 15.0], 'ZoomTime': [0.0, 30.0],
})
enums.update({'RenderShadowDetail': ['0', '1', '2'], 'SlipstreamLookAtNames': ['0', '1', '2', '3', '4'],
              'RenderFSAAType': ['0', '1', '2', '3']})
ranges = {k: v for k, v in ranges.items() if k in ours}
enums = {k: v for k, v in enums.items() if k in ours}
validate = {'FSFontSettingsFile': 'fontset'}

# Why common Firestorm / Black Dragon settings have no counterpart here; shown as the
# skip reason (prefix match).
unmappable = {
    'FSAO': 'Firestorm AO sets in #Firestorm/#AO are used by the Slipstream AO as they are; set its options in the AO editor',
    'UseAO': 'Turn the Slipstream AO on in its control window or with /ao on',
    'PauseAO': 'Pause the Slipstream AO in its control window or with /ao pause',
    'LockAOFolders': 'The Slipstream AO does not lock its folders',
    'UseLSLBridge': 'No LSL client bridge in Slipstream', 'FSRadarEnhanceByBridge': 'No LSL client bridge',
    'UseMoveLock': 'No LSL client bridge (movelock)',
    'FSEnableGrowl': 'No Growl / desktop notifications', 'FSGrowl': 'No Growl / desktop notifications',
    'Omnifilter': 'No omnifilter', 'FSBeam': 'No custom selection beams',
    'FSColorClienttags': 'No client tags', 'FSUseLegacyClienttags': 'No client tags',
    'FSClientTagsVisibility': 'No client tags', 'FSEnableDiscord': 'No Discord integration',
    'ShowDiscord': 'No Discord integration', 'EnableDiscord': 'No Discord integration',
    'FSPrimfeed': 'No Primfeed integration (tokens are never imported)',
    'ExodusFlickr': 'No Flickr integration (tokens are never imported)',
    'FSAreaSearch_': 'Area search keeps no saved filters here',
    'QuickPrefs': 'Quick preferences here is the hover-height floater; FS quick_preferences.xml has no equivalent',
    'Exodus': 'Black Dragon / Exodus post-processing is GL-only; the Vulkan renderer has its own (Lightbox)',
    'Render': 'Renderer-specific (GL) setting; the Vulkan renderer does not use it',
    'PlayModeUISnd': 'Per-sound play switches are not implemented (sounds always play when set)',
    'FSBuildPrefs_': 'New-object build defaults are not implemented',
    'FSCmdLine': 'Chat command not implemented here',
    'Prefs': 'Black Dragon preferences-panel layout state', 'Joystick': 'Joystick button mapping differs (Black Dragon)',
    'WL': 'Black Dragon Windlight tuning (GL)', 'FSLegacy': 'Legacy Firestorm UI mode not implemented',
    'FSShowConvoAndRadarInML': 'Mouselook UI hiding differs',
    'FSSnapshot': 'Snapshot framing options are not implemented', 'FSStatusBar': 'Status bar layout differs',
    'FSContactList': 'Contact list layout differs', 'FSFriendList': 'Contact list layout differs',
    'DebugStatMode': 'Statistics floater layout differs',
}

toolbar_commands = {
    'quickprefs': 'quick_prefs', 'snapshot_to_disk': 'snapshottodisk', 'statistics': 'stats',
    'phototools': 'lightbox', 'radar': 'people', 'windlight': 'myenvironments',
    'sky': 'personal_lighting', 'water': 'personal_lighting', 'teleport_history': 'teleport_history',
    'animationoverride': 'ao',
}
toolbar_notes = {
    'areasearch': 'Area search has no toolbar button here (World menu)',
    'contact_sets': 'Contact sets are in People > Friends',
    'pose_stand': 'No pose stand (use the Poser)', 'discord': 'No Discord integration',
    'primfeed': 'No Primfeed integration', 'flickr': 'No Flickr integration',
}
key_commands = {'toggle_crouch': 'toggle_down'}
core_commands = ['push_forward', 'push_backward', 'slide_left', 'slide_right', 'turn_left', 'turn_right',
                 'jump', 'push_down', 'toggle_fly', 'start_chat', 'walk_to']

m = {
    'version': 1,
    'notes': 'Viewer importer map (#231/#232). Generated by scripts/viewer_import/gen_viewer_import_map.py; see doc/viewer_import.md. '
             'Sources: Firestorm phoenix-firestorm 4ae31a7ad69d, Black Dragon Release_5.6.3 b2ca434b39bc.',
    'settings': {
        'rename': rename, 'render_allow': render_allow, 'deny_exact': deny_exact,
        'deny_regex': deny_regex, 'deny_contains': deny_contains, 'ranges': ranges, 'enums': enums,
        'validate': validate, 'unmappable': unmappable,
        'restart': ['Language', 'FontScreenDPI', 'UIScaleFactor', 'RenderVSyncEnable', 'FSFontSizeAdjustment',
                    'FSFontLineSpacingAdjustment'],
    },
    'toolbars': {'commands': toolbar_commands, 'notes': toolbar_notes},
    'key_bindings': {'commands': key_commands, 'core': core_commands,
                     'bd_modes': {'0': 'first_person', '1': 'third_person', '3': 'edit_avatar', '4': 'sitting'},
                     'max_per_command': 3},
}
out = llsd.format_pretty_xml(m)
with open(os.path.join(APP, 'viewer_import_map.xml'), 'wb') as f:
    f.write(out)
print('renames', len(rename), 'render_allow', len(render_allow), 'deny', len(deny_exact), 'ranges', len(ranges), 'enums', len(enums))
