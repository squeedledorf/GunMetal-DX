# Region / parcel weather (#783 M2): the `[slwx ...]` parcel tag

Slipstream's weather (rain, snow, wind, lightning) is drawn only in the viewer. By default it is
the user's own choice: Weather floater, local weather. This note covers how a parcel can
publish weather that every opted-in Slipstream viewer on it shows the same way. It also records
the transports that were checked and why this one was picked.

Viewer setting: `WeatherAllowRegion`, default **off** ("Use region / parcel weather" in the
Weather floater). The user's local weather, when on, always wins.

## 1. The convention

The parcel **description** (About Land > General > Description) carries one tag:

```
[slwx rain=0.7 wind=225@8 var=0.5 gust=0.6 storm=0.4 seed=0x5eed]
```

- The tag starts with `[slwx`, in any case, followed by a space or `]`. It ends at the next
  `]`. Text before and after it is left alone. Only the first tag counts.
- The tag holds space-separated `key=value` tokens. Numbers use `.` as the decimal separator.
- Out-of-range values are clamped.
- Unknown keys are ignored, so later versions can add keys without breaking older viewers.
- A malformed value drops that one token. The rest still applies, and the floater says the tag
  has errors.

| key     | value                    | meaning | default |
|---------|--------------------------|---------|---------|
| `rain`  | 0..1                     | rain amount | 0 |
| `snow`  | 0..1                     | snow amount (both together = sleet) | 0 |
| `rainmm` | mm/h 0..200             | #954: the rain RATE (wins over `rain`; see section 2b) | from `rain` |
| `snowcm` | cm/h 0..360             | #954: the snowfall RATE, fresh snow (wins over `snow`) | from `snow` |
| `pvar`  | 0..1                     | #954: how strongly the rate varies over time (bands, a downpour passing) | 0.35 |
| `wind`  | `DIR@SPEED` or `SPEED`   | wind FROM `DIR` degrees (0 north, 90 east), `SPEED` m/s (0..40) | `225@3` |
| `var`   | 0..1                     | wind speed variation (0.5 = 50..150 %) | 0.3 |
| `gust`  | 0..1                     | gustiness (0 slow swells .. 1 sharp 1-2 s gusts) | 0.5 |
| `wdirvar` | degrees 0..180         | wind direction swing: the direction moves smoothly within +- this many degrees of `wind`'s direction (180 = fully variable); added on top of the slight wander `var` brings; old viewers ignore it | 0 |
| `storm` | 0..1                     | lightning / thunder amount (0 = none) | 0 |
| `fog`   | 0..1                     | extra fog | from rain / snow |
| `cloud` | 0..1                     | cloud cover hint for the renderer | max(rain, snow, storm-based) |
| `trans` | seconds 0..600           | how long changes take to settle | 30 |
| `seed`  | decimal or `0x` hex, 32-bit | shared random seed | from region id + parcel local id |
| `depth` | metres 0..10             | shared snow depth on open, level ground at `since` (section 2a) | none: local build-up |
| `rate`  | metres per hour, -100..100 | shared snow accumulation while the tag snows; 0 holds, negative melts | 0 (holds); #954: `snowcm` x 0.7 / 100 when the tag has `snowcm` |
| `since` | unix seconds (UTC), whole | when `depth` applied | none: `depth` holds, `rate` is not applied |
| `off` / `clear` | (bare word)      | explicitly no weather here (wins over any rain/snow/storm/wind in the same tag) | |

The shortest useful tag is `[slwx rain=.6]` (14 bytes). The description is limited to 255 bytes
(`ParcelProperties.ParcelData.Desc` is `Variable 1`), so the tag uses short keys.

## 2. Synchronisation (the seed)

Every random element is a pure function of `(seed, synced UTC time)`. The code is in
`slweather.cpp`:

- **Gusts**: three value-noise octaves (periods ~45 s, ~12 s and ~1.6 s), sampled at UTC
  seconds. The octave frequencies are fixed, and the gustiness only changes how they mix.
  `wdirvar` adds a direction swing from three further noise octaves (own salts, periods ~56 s,
  ~12 s and ~1.6 s, the gustiness mixing in the quick one), +- `wdirvar` degrees at the noise
  extremes. At 0 or absent nothing is added: the speed variation's slight (<= 18 degrees x `var`)
  wander remains the whole behaviour.
- **Lightning**: synced time is cut into 1 s slots. Slot *k* strikes iff
  `hash(seed, k) < rate`, where `rate = 0.15/s × storm × (0.2 + 0.8 × precipitation)`.
  Further hashes of the same slot give:
  - the exact moment;
  - the distance (0.3-10 km);
  - the azimuth (around the upwind side);
  - the elevation;
  - the flicker shape.
- **Synced time** is `LLDate::now() + gUTCOffset`: the local wall clock corrected by the
  server's UTC offset. Viewers whose clocks are synced by NTP agree to within milliseconds.
  `gUTCOffset` is whole seconds, so a badly set clock can still be up to about 1 s off.

What is **not** synced:

- Which thunder recording plays. Each viewer avoids its own recent recordings.
- The first ~20 s of a storm. A per-viewer ease gates part of the strikes while the storm builds.
- Transitions. They start when each viewer saw the tag.

The default seed is derived from the region id and the parcel's local id. Two parcels with
identical tags therefore get different (but still shared) storms. Set `seed=` to share one
storm across several parcels.

## 2b. Precipitation rate (`rainmm=` / `snowcm=` / `pvar=`, #954)

`rain` / `snow` are amounts on a fixed, roughly logarithmic ladder; `rainmm` / `snowcm` are the
physical rates behind them (`SLWeather::amountFromFall` / `fallFromAmount`):

```
rate = r0 x ((1 + top / r0)^amount - 1)    rain: r0 = 100/360 mm/h, top = 100 mm/h
                                           snow: r0 = 15/80 cm/h,   top = 15 cm/h
```

so amount 0.25 / 0.5 / 0.75 / 1 is ~0.9 / 5 / 23 / 100 mm/h of rain and ~0.4 / 1.5 / 4.9 / 15 cm/h
of snow. A viewer that knows `rainmm` / `snowcm` uses them (the amount's 0.01 steps are coarse near
the top); older viewers read `rain` / `snow`. **Set as parcel weather** writes both. Rates above the
ladder's top keep the amount at 1 and still intensify the look on newer viewers.

The rate varies over time (`pvar`, default 0.35; 0 = flat): it is multiplied by
`exp(1.1 x pvar x n(t))`, `n` two value-noise octaves (~4 min band, ~45 s cell) of `(seed, synced
UTC)`, and in some 20-minute windows (chance 0.8 x `pvar`, decided by a hash of the window) by a
bump of 1.6..2.6 x for a minute or two -- a downpour or squall passing. Every viewer on the parcel
sees the same storm. The variation shapes only what falls (and its local build-up and the
visibility): whether the tag snows, the shared level and the lightning use the unvaried rate.

## 2a. Shared snow depth (`depth=` / `rate=` / `since=`, #783)

Without these keys, lying snow builds up from when each viewer arrived, so a visitor who has
been on the parcel for an hour sees deep drifts while a newcomer sees bare ground. With `depth=`
every viewer computes the same open-sky level from the tag and synced UTC alone
(`SLWeather::snowLevelAt`, slweather.cpp):

```
level = clamp(depth + rate x max(now - since, 0) - heat_melt, 0, cap)
```

- `cap` is the viewer's own `WeatherSnowMaxDepth` (0 with `WeatherSnowCoating` off). Viewers with
  different caps therefore differ once the level passes the smaller cap.
- A positive `rate` only counts while the tag snows (`snow` > 0.02). A negative `rate` (melt)
  always counts. If the owner starts the snowfall in an existing tag, they should press **Stamp
  now** first. Otherwise the growth since the old `since` is added at once.
- `since` ahead of the viewer's clock holds the depth until then.
- `heat_melt` is the #949 hook (fires melting snow). It is 0 for now, and whatever fills it in must
  be a deterministic replay on synced time.
- **Arrival** (login, a teleport, a reset): the viewer's snow grids (inner grid and outer ring)
  start at the level. A texel neither grid has seen starts at the global level. Covered areas
  follow the usual shelter / cover rules.
- **Re-sync** (an owner edits the tag, or the agent walks onto a parcel whose tag gives another
  level): the level blends over `WeatherSnowSyncFadeSeconds` (default 8 s), and then follows the
  formula exactly. The renderer's grids follow the global level up and down at the same pace.
- While the level is above zero the snow cover stays on (a held depth does not melt). At zero
  the cover melts at the normal rate.
- The per-second build in the renderer (the grids, the avatars' and objects' snow) uses `rate`
  exactly while it applies. `snow=` still sets how much snow falls.
- **Not synced:** drifts, slump / repose shaping, footprints, mover tracks and compaction. Each
  viewer shapes the shared level from its own geometry, settling within seconds.
- `rate=` without `depth=` only replaces the per-hour build while the tag snows. The level stays
  local.
- #954: with `depth=` / `since=` and `snowcm=` but no `rate=`, the level grows by the MEAN snowfall
  x (1 - 0.3: fresh snow settles) / 100 m/h while the tag snows. Viewers older than #954 hold the
  depth instead, so the floater always writes an explicit `rate=` (by default derived the same way
  from the snowfall: "Rate follows the snowfall").
- Without `depth=` and `rate=`, each viewer's own depth grows by the snowfall rate it shows (the
  bands included) x 0.7.
- `off` / `clear` drops all three keys.
- Like the rest of the tag, the level applies to the whole weather the viewer shows while its
  agent stands on the parcel. The snow grids have no parcel bounds, so in a region split between
  parcels, the parcel the agent is on sets the level everywhere it sees.

The Weather floater's **Share the snow depth with the parcel** writes the three keys with
**Set as parcel weather**. Its Depth / Rate fields are validated (depth 0..your Snow max depth,
rate within +-100 m/h). **Stamp now** sets `since` to the current synced UTC second. If nothing
was stamped, export stamps the current time. Example:

```
[slwx snow=.6 wind=225@8 depth=.25 rate=.05 since=1792656000]
```

With #954 the same tag reads, for example,
`[slwx snow=.89 snowcm=9 wind=225@16 depth=.25 rate=.063 since=1792656000]` (a blizzard).

## 3. EEP-derived weather (`WeatherFromEnvironment`, default off)

When the parcel has no tag (or `WeatherAllowRegion` is off), the shared sky's EEP cloud
coverage (`cloud_shadow`) can still make it rain:

- Synced time is cut into 10-minute windows.
- A window rains iff `hash(region seed, window) < 0.85 × smoothstep(0.5, 0.95, coverage)`.
- The intensity grows with the coverage.
- In every other window with coverage above 0.85 there is a mild storm.

Every viewer in the region agrees on these windows. Nothing about the region's sky is changed.
The weather only adds rain under clouds the region already shows (`cloud` is set to the sky's
own coverage).

Limitation: while the user has a personal environment selected, the shared sky instance may
not be stepped, and its coverage can be stale.

## 4. Transports considered (what was verified)

| candidate | verdict | evidence (this tree) |
|---|---|---|
| **Object chat on a non-zero channel to the avatar** (`llRegionSayTo(av, ch, ...)`) | **Impossible** | `ChatFromSimulator` (scripts/messages/message_template.msg:2930) has no channel field, so a viewer cannot receive or tell apart non-zero channels. Only channel 0 / debug reach viewers. Non-zero `llRegionSayTo` to an avatar is only heard by that avatar's attachments. |
| **Channel 0 `llRegionSayTo` / `llOwnerSay`** | Rejected | Delivered as `CHAT_TYPE_DIRECT` / `CHAT_TYPE_OWNER` (llui/llchat.h:54-55). `process_chat_from_simulator` shows DIRECT exactly like normal chat (llviewermessage.cpp, the `case CHAT_TYPE_DIRECT:` fall-through), so every non-Slipstream user would see the protocol as chat spam. It also needs a script that addresses each avatar, and `llOwnerSay` reaches only the owner. |
| **Custom keys in the EEP settings LLSD** | Rejected | `LLSettingsBase::settingValidation` strips every key not in the validator list ("Stripping setting ...", llinventory/llsettingsbase.cpp:555-571). Every LL-derived viewer drops custom keys on load and on save, so they cannot survive an edit round trip. Whether the simulator keeps them was not tested (it needs a live region). The settings `name` survives, but it is not shown to visitors, and a long name is awkward to edit. |
| **Parcel description** | **Chosen** | `ParcelProperties` (`Desc`, Variable 1 = 255 bytes) already reaches every viewer for the parcel its agent stands on (the viewer's existing parcel tracking; this feature sends nothing) and is parsed into `LLParcel::getDesc()` (llinventory/llparcel.cpp:557). Only the parcel owner or managers can edit it, so authority comes from the land permissions. It needs no script, nothing is sent from the viewer, and other viewers simply show the tag as text in About Land. |
| Parcel media URL / media description | Not used | Same delivery and authority, but the fields drive real media playback and parcel media settings. Overloading them breaks media or shows garbage in other viewers' media controls. |
| **Marked controller prim** (hover text, `llSetText` with alpha 0) | Rejected for now | Hover text arrives in `ObjectUpdate` (`_PREHASH_Text`, llviewerobject.cpp:1494) to every viewer. However, `OwnerID` in that message is only valid with an attached sound or particle system (llviewerobject.cpp:1325 "HACK" comment), so the viewer cannot check who owns the controller: anyone with build rights could rez one. Objects also arrive only within the interest list or draw distance, so region-wide sync is unreliable. Proper owner checks need `RequestObjectPropertiesFamily`, which is a message to the server and is out of scope. |
| Estate covenant | Not used (owner option) | Region-wide and estate-owner-only, but reading it needs `EstateCovenantRequest`, a message to the server. |

## 5. Limits of the chosen convention

- **Parcel scope.** A whole-region parcel (the common case for sims that want weather) covers
  the region. A multi-parcel region needs the tag on each parcel that should have weather. Use
  the same `seed=` to keep them in step.
- **Edits propagate lazily.** Viewers already on the parcel see a changed description when
  they next receive the agent parcel's properties, for example on re-entering the parcel or
  after a teleport. It has not been tested whether the simulator pushes an edit to agents
  already on the parcel. Weather is not time-critical, so this is accepted.
- **The tag is visible** in About Land as plain text, which also documents it.
- **Explicit clear.** `[slwx off]` lets a parcel say "no weather here" even when its
  neighbours have some.

## 6. Options for the owner (not implemented)

1. **Region-wide tag in the estate covenant.** One place for the whole region. It needs a
   (read-only) `EstateCovenantRequest`.
2. **Owner-verified controller prim.** The viewer looks for a prim with a marker hover text,
   then verifies its owner against the parcel or estate owner with
   `RequestObjectPropertiesFamily` (a read-only request). It needs the same server-message
   approval as option 1.
3. **Server-side support** (a region capability or EEP field kept by the simulator). This is the
   robust long-term answer, but it is not available to a third-party viewer.
