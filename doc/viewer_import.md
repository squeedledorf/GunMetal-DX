# Import from another viewer

**Me > Import from Another Viewer...** (or Preferences > Advanced) brings settings
and data over from **Firestorm**, **Black Dragon**, **Alchemy** or the **Second Life**
viewer installed on the same computer. Tickets: #131 (original importer), #231
(import everything compatible), #232 (key bindings are merged, not replaced).

Nothing in the other viewer is changed. Every row is reviewed before it is applied,
every import is backed up first, and the last import can be undone.

## Where the other viewer's data is looked for

| Viewer | Windows (`%APPDATA%`) | Linux (`~`) | macOS (`~/Library/Application Support`) |
|---|---|---|---|
| Firestorm (64-bit) | `Firestorm_x64` | `.firestorm_x64` | `Firestorm_x64` |
| Firestorm (32-bit) | `Firestorm` | `.firestorm` | `Firestorm` |
| Black Dragon | `BlackDragon` | `.blackdragon` | `BlackDragon` |
| Alchemy | `AlchemyViewer`, `AlchemyNext` | `.alchemyviewer`, `.alchemynext` | same as Windows |
| Second Life | `SecondLife` | `.secondlife` | `SecondLife` |

Inside it: `user_settings/` holds the global data, and one folder per account
(`first_last`) holds the per-account data. Accounts on other grids (`name.grid`)
are not listed. The account you pick is imported into the account you are logged
in as; per-account rows need you to be logged in.

## What is imported

| Category | From | How | Takes effect |
|---|---|---|---|
| Settings | `user_settings/settings.xml` | every compatible setting (rules below) | live, saved at once |
| Per-account settings | `<account>/settings_per_account.xml` | same rules; a setting goes to whichever group holds it here | live |
| Notifications | `user_settings/ignorable_dialogs.xml` | your "don't show again" choices, for dialogs we have | live |
| Colours (off by default) | `user_settings/colors.xml` | colours that exist in our skin; references to their skin's colours are skipped | restart |
| Window positions (off by default) | `floater_rect_*` / `floater_pos_*` in settings.xml | only for windows that exist here | next time the window opens; restart recommended |
| Toolbars | `<account>/toolbars.xml` | their layout replaces ours; commands renamed (e.g. FS `quickprefs` -> `quick_prefs`, `statistics` -> `stats`, `phototools` -> `lightbox`, `radar` -> `people`, BD `windlight` -> `myenvironments`), unknown ones dropped and listed, duplicates removed | live |
| Key bindings | `user_settings/key_bindings.xml` (Black Dragon: `controls.xml`) | **merged** (see below) | live |
| Contact sets | `<account>/settings_friends_groups.xml` | sets we don't have are added; missing members and aliases are added; ours win on conflicts | live |
| Teleport history | `<account>/teleport_history.txt` | places we don't have are added by date; nothing removed | live |
| Graphics presets | `user_settings/presets/graphic/*.xml` | only the renderer settings with a Vulkan meaning are kept | live (pick it in the presets list) |
| Camera presets | `user_settings/presets/camera/*.xml`; Black Dragon `user_settings/camera/*.xml` | compatible keys; Black Dragon's `camera_offset` / `focus_offset` become `CameraOffsetRearView` / `FocusOffsetRearView`; names that clash with built-in presets get " (Black Dragon)" | live |
| Poser poses | Firestorm / Alchemy `user_settings/poses/**` | copied when the name is free (Black Dragon poses use another format: not imported) | live |
| Font sets | `user_settings/fonts/*` | copied when the name is free | pick in Preferences |
| Windlight presets | `user_settings/windlight/{skies,water,days}` | copied to our windlight folder; load them with Import in the environment editors | - |
| Auto-replace | `user_settings/autoreplace.xml` | new lists added; missing words added to lists we have | live |
| Spelling | `user_settings/dictionaries/user_custom.dic`, `user_ignore.dic` | missing words added | live |
| Chat transcripts | the account's chat folder (`InstantMessageLogPath` honoured) | merged entry by entry (#131); a changed file keeps a `.pre-import` copy | live |

Re-importing is a no-op: rows that already match show as "same".

## Settings rules

A setting from their file is offered when:

1. it exists here under the same name, or `viewer_import_map.xml` renames it
   (68 renames for Firestorm / Black Dragon, 14 with a value conversion: e.g. Firestorm chat commands gain our
   leading `/`, `MouseWarpMode` 2 -> `DisableMouseWarp`, Firestorm's ad-hoc
   ignore + "allow friends" -> `SlipstreamIgnoreAdHocSessions` 0 / 1 / 2);
   from Alchemy, every setting Slipstream renamed from `Alchemy*` to `Slipstream*` in
   1.2 (#126) is mapped back through the same table as the first-launch migration,
   `app_settings/settings_renames.xml`;
2. it is not denied: credentials and tokens (never), install / machine / session
   values (window size and position, cache locations, grids and login pages,
   version and GPU stamps, update channel, `*SettingsFile`, `Debug*`, voice device
   ids, ...);
3. a `Render*` setting is on the renderer allowlist (draw distance, LOD factors,
   avatar complexity and impostors, particle count, shadows on/off, SSAO, SSR, depth
   of field, anti-aliasing, anisotropic, CAS sharpness, max texture size, vsync and a
   few UI-level ones). GL-only and renderer-tuning settings are left out;
4. the type matches ours (S32 / U32 interchange when the value fits);
5. the value is valid here: numbers in our preference panels' slider ranges
   (clamped, with a note), combo / radio values among the ones we offer, font sets
   installed;
6. it differs from our current value.

Measured against the two viewers' declared settings (all keys they can write;
a user's `settings.xml` only holds what they changed):

| | Firestorm (2546 keys) | Black Dragon (1713 keys) |
|---|---|---|
| same name, importable | 1218 | 1140 |
| renamed / converted | 64 | 5 |
| denied (install, machine, credentials) | 139 | 139 |
| renderer-specific `Render*` | 171 | 172 |
| type differs | 2 | 1 |
| no equivalent here | 951 | 256 |

Rows that are skipped show why (tick "Show unchanged and skipped"), and the log
(`ViewerImport` tag) records every imported, refused and skipped row.

## Key bindings (#232)

The old importer copied their `key_bindings.xml` over ours and reloaded it; a file
we could not fully read left every mode empty (no WASD). Now:

- their file is read leniently (Black Dragon's `controls.xml` too, without its
  build-mode bindings; `toggle_crouch` becomes `toggle_down`);
- each `(mode, key / mouse, modifiers) -> action` is compared with ours: an action we
  don't have is skipped; one we already have is "same"; a key that is free here is
  **added**; a key we use for another action is a **conflict**, listed but unticked
  (ticking it replaces only that binding, and never one that would leave a movement
  action without a key); an action with three keys already is skipped;
- **nothing of ours is removed**, and modes their file doesn't define are untouched;
- after writing, the bindings are reloaded and checked: if any movement / core action
  that had a key before lost it, the previous file is put back and nothing changes;
- at startup, a `key_bindings.xml` that leaves walk forward / back unbound in third
  person is moved to `key_bindings.xml.unusable` and the defaults are loaded, with a
  notice (recovery for anyone hit by the old importer).

## Backup and undo

Before applying anything the importer creates
`user_settings/import_backups/<date_time>/` with copies of the files it will change,
reference copies of the settings and an `undo.xml` journal (each setting's previous
value, files to restore, files it created, teleport places it added). **Undo last
import** replays it: settings get their old values, changed files (key bindings,
toolbars, contact sets, auto-replace) are restored and reloaded, colours are put
back (restart to see them everywhere), dictionary files are restored (restart), added presets / poses / fonts / Windlight files are removed, added teleport
places are taken out. Chat transcripts are not undone (each changed transcript keeps
its `.pre-import` copy). Undo needs the same account logged in for per-account data.

## Not imported, and why

- **Animation overrider** (FS AO): Slipstream has none; FS keeps the AO sets in
  inventory (`#AO`) anyway.
- **LSL client bridge**, movelock, flight assist, Growl, Discord, Primfeed / Flickr
  (their tokens are never read), client tags, omnifilter, custom selection beams:
  no such features here. (Keyword alerts and Firestorm's other chat options are
  ported under their Firestorm names and import as-is; `FSShowTimestampsIM` maps to
  `IMShowTime`; `FSLastOpenIMs` is session state and is not imported.)
- **Quick preferences** (`quick_preferences.xml`): our Quick Preferences window is the
  hover-height floater; there is nothing to map to.
- **Area search filters**: Firestorm keeps them in `FSAreaSearch_*`; our area search
  does not persist filters.
- **Black Dragon poses**: same folder name as the poser's, different file format.
- **Favorites bar order**: stored on the server with the inventory, so it is already
  shared between viewers.
- **Saved logins and passwords**, grid lists (`grids.user.xml`): credentials and
  grid definitions are never imported.
- **Block / mute list**: server-side. Firestorm's asset blacklist / derender list uses
  a different format from ours.
- Per-group options, muted groups, media autoplay choices, inventory filters, typed
  location history: session or UI state.
- **Renderer settings** outside the allowlist: GL pipeline tuning that the Vulkan
  renderer does not use (Black Dragon's Exodus post-processing, shadow resolutions,
  SSR tuning vectors, ...).

## Maintaining the map

`indra/newview/app_settings/viewer_import_map.xml` is generated by
`scripts/viewer_import/gen_viewer_import_map.py` (renames, conversions, deny tiers,
renderer allowlist, toolbar and key command renames, plus ranges / enums scraped from
our preference panels). Edit the script and re-run it from the repository root
(`pip install llsd`); it refuses to write a map whose targets don't exist in our
`settings*.xml`.

The mapping was researched against Firestorm `phoenix-firestorm` master at
`4ae31a7ad69db710973ef37070ab36486b4deb61` (2026-10-01) and Black Dragon
`Black-Dragon-Viewer` `Release_5.6.3` at `b2ca434b39bc` (2026-07-01).
