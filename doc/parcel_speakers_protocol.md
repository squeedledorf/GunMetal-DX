# Parcel audio through in-world speakers (#892): the `[slspk ...]` tag

Today a parcel's music stream plays everywhere on the parcel at the same level, with no position.
With speakers, Slipstream plays it from objects the parcel's owner has marked as speakers: it gets
a position, falls off with distance, and goes through the room acoustics. This note is the
convention builders use to mark speakers, and how the viewer decides which ones it trusts.

Nothing is added on the server. Other viewers are not affected: they keep playing the stream the
old way and never see the tags.

Viewer settings (`ParcelAudioSpeakers` and `ParcelAudioCapture` default **off** until the output
has been listened to and soak-tested):

| setting | what it does |
|---|---|
| `ParcelAudioSpeakers` | Preferences > Sound & Media, "Play parcel music from in-world speakers when the parcel has them". Finds the speakers of the parcel you are on, checks their owner / group, and plays the stream from them. Turning it on turns `ParcelAudioCapture` on. Off: no discovery, no requests. |
| `ParcelAudioCapture` | The media plugin hands the stream's audio to the viewer, which plays it through OpenAL: one stereo source with no effects when there are no speakers (what the plugin played), else the speakers. A change restarts a playing stream. Off: the plugin plays the stream itself, exactly as before. |
| `ParcelAudioSpeakersOutside` | Out of range of every speaker: 0 = the parcel's `outside=` (default), 1 = always the quiet stereo bed, 2 = always silence. Also in Preferences. |
| `ParcelAudioMaxSpeakers` | Speakers playing at once, 1..32 (default 16): the loudest at your position. Speakers in range beyond it are skipped; the log says so once per change ("N speakers in range, M playing (cap C)"). |
| `ParcelAudioBufferSeconds` | Audio queued in OpenAL, 1.0..2.5 s (default 1.2): rides out frame hitches; the stream starts this much later. A jitter buffer in front of it (the 8 s capture ring) holds the deepest delivery stall seen in the last 2 minutes + 0.25 s (at least 0.15 s). |
| `ParcelAudioDiffuseReverb` | With room acoustics: the speakers' reverb keeps its level over distance (diffuse field). Off (default): it decays with distance like any in-world sound's. Also in Preferences. |
| `ParcelAudioSyncToleranceMs` | Sync guard (default 10): about once a second every speaker's stream position is measured against the timeline (the stereo bed, which OpenAL never doppler-shifts); a speaker further off is re-joined at the next 50 ms chunk boundary. While you move faster than 1 m/s only offsets over 50 ms are fixed (doppler shifts speakers legitimately). 0 = measure and log only. A speaker that leaves the playing set keeps running silently for 20 s, so it returns in step. |
| `AudioInverseDistanceRolloff` | All in-world sounds: a script sound radius (`llSetSoundRadius`) becomes a soft maximum (the last 15 % fades out) instead of a hard cut. Sounds already fall off as real inverse distance. Off (default): as before. Also in Preferences. |

## 1. Marking a speaker

A speaker is any prim whose **hover text** holds a tag. Set it faintly visible (alpha 0.1): the simulator does not send near-invisible hover text to viewers (alpha 0.01 is dropped), and Slipstream does not draw the tag:

```lsl
llSetText("[slspk grp=stage ch=L vol=0.8 r=40]", <1.0, 1.0, 1.0>, 0.1);
```

Why hover text: it is sent to every viewer in range in the normal object updates, it stays on the
prim after the script that set it is deleted (and after a take and re-rez), and it comes back
from the viewer's object cache after a relog. So a one-shot script is enough, and no script needs
to keep running in the speaker.

- The tag starts with `[slspk`, in any case, followed by a space or `]`. It ends at the next `]`.
  Text before and after it is ignored. Only the first tag counts.
- Space-separated `key=value` tokens and bare words. Numbers use `.` as the decimal separator.
- Out-of-range values are clamped.
- Unknown keys are ignored, so later versions can add keys without breaking older viewers.
- A malformed value drops that one token; the rest still applies.
- Hover text holds at most 254 bytes. The longest useful tag is well under 100.

| key | value | meaning | default |
|---|---|---|---|
| `grp` | 1-24 of `a-z 0-9 _ . -` | speaker group. Groups can have different levels and ranges (a stage and a quieter bar). All groups play the same parcel stream. | `main` |
| `ch` | `M`, `L`, `R`, `S` | role: mono `(L+R)/2`, left channel, right channel, sub (low-passed mono). Place `L` and `R` speakers for a stereo stage. | `M` |
| `vol` | 0..2 | speaker level, times each listener's music volume (2 = +6 dB over the stream) | 1 |
| `r` | metres, 5..100 | audible range | 30 |
| `dir` | degrees, 1..360 | optional cone (full angle) around the prim's +X axis, for directional PA boxes; 360 or absent = all round | none |
| `t0` | UTC unix seconds | optional start time of a fixed clip (a non-live URL), so listeners on different viewers hear it within about a second of each other | none |
| `off` | (bare word) | keep the tag but silence this speaker | |

Shortest useful tag: `[slspk]` (a mono speaker of group `main`, full level, 30 m).

## 2. Which speakers count (trust)

Anyone can copy the tag text into their own prim. What they cannot copy is the owner or the
group: the viewer asks the simulator for both (one `RequestObjectPropertiesFamily` per linkset with a tagged prim, for its root; any prim of a linkset can be a speaker and plays from its own position;
about ten per second at most). A tagged object is a speaker only if **at least one** of
these holds:

1. its owner is the parcel's owner;
2. it is deeded to the parcel's group;
3. its group is set to the parcel's group, deeded or not.

Rule 3 exists because builders usually set speakers to the land group rather than deeding them, so
that they are not returned. The worst a group member can do is place one more speaker; it cannot
change or silence the existing ones. (Earlier drafts required a `group-ok` opt-in in the parcel
description for rule 3. That is no longer needed; a `group-ok` word there is ignored.)

The speaker must also stand on the parcel you are on, or on another parcel of the **same owner** in
the same region (for stages that straddle an internal parcel line). For the second case the viewer
looks the parcel up by position once per 4 m cell (`ParcelPropertiesRequest`) and remembers the
answer for two minutes.

Attachments are never speakers. Temporary (temp-on-rez) objects can be.

The owner and group are checked again when the tag changes (at most every 30 s per object) and
every five minutes. When you change parcel, every speaker is checked against the new parcel
without new requests.

## 3. Parcel policy

The parcel **description** (About Land > General > Description) may carry one tag of its own:

```
[slspk outside=ambient]
```

| key | value | meaning | default |
|---|---|---|---|
| `outside` | `silent`, `ambient` | what listeners hear out of range of every speaker: nothing, or the stream as a quiet stereo bed | `silent` |

It can sit next to a weather tag (`[slwx ...]`, see `weather_region_protocol.md`); each viewer
feature reads only its own tag.

## 4. Scripts

Both are shipped with the viewer in `app_settings/slspeakers/` (full permissions; copy, change and
give away):

- `slipstream_speaker_setup.lsl`: edit the tag, drop it in the prim; it sets the tag and deletes
  itself.
- `slipstream_speaker_live.lsl`: stays in the prim and takes owner commands on channel 8920
  (`vol`, `r`, `ch`, `grp`, `on`, `off`, `status`, `clear`), e.g. to lower a speaker during a set.
  Viewers see a change with the next object update, within about a second.

To remove a tag, set empty hover text: `llSetText("", ZERO_VECTOR, 0.0);`.

In Slipstream, right-click the prim > Tools > **Set up as speaker...** does this without writing a
script: pick group, channel, level, range, cone and off; the floater shows the tag and whether the
object qualifies on this parcel, then puts a one-shot script into the prim that sets the tag (or,
with "Remove speaker tag", clears the hover text) and deletes itself
(`indra/newview/slfloaterspeakersetup.cpp`; the script is uploaded through `UpdateScriptAgent` into
one reused "Slipstream speaker setup" item in your Scripts folder and rezzed into the prim).

## 5. What to expect

- **Within one viewer** all speakers play sample-locked.
- **Between viewers**, a live stream is a few seconds apart, as it is today: every listener's
  player connects and buffers on its own, and live streams carry no timestamps. Do not plan for
  "everyone hears the drop at the same moment". A fixed clip with `t0=` is within about a second.
- A prim that is a speaker cannot also show visible hover text.
- Other viewers draw the tag faintly (alpha 0.1). It is harmless; Slipstream strips it from the drawn text.

## 6. Viewer side (for developers)

- **Registry**: `indra/newview/slparcelspeakers.{h,cpp}`. Hover-text changes come from
  `LLViewerObject::processUpdateMessage` (both update paths); owner / group replies are tapped in
  `LLSelectMgr::processObjectPropertiesFamily`; parcel-by-position replies use sequence ids
  -45000..-45063 and are handled at the top of `LLViewerParcelMgr::processParcelProperties`.
  At most 256 tagged objects are tracked; an entry is freed when its object dies or loses the tag.
  `SLParcelSpeakers::speakers()` is the validated set; `generation()` changes with it.
- **Capture**: the parcel stream's media plugin writes 48 kHz stereo S16 PCM into a shared-memory
  ring (`indra/llplugin/llpluginaudioring.h`, created with the same `shm_add` mechanism as video
  frames). Linux: a GStreamer `appsink` as playbin's audio sink. Windows: libvlc audio callbacks.
  Only control messages go over the plugin pipe (`audio_capture`, `audio_capture_shm`,
  `audio_capture_format`, `audio_capture_status`); stream titles (ICY) are unchanged.
  `LLPluginAudioRingReader` is the consumer: PCM chunks, the stream clock, producer drift in ppm and
  underrun counts.
- **Output**: `indra/newview/slparcelspeakeraudio.{h,cpp}` (audio idle, after the acoustics update)
  reads the ring, keeps 1.2 s queued, resamples by the reader's trim ratio (drift correction in the
  feeder, ±0.2 %, never per-source pitch), splits each 50 ms chunk once into the roles (stereo,
  M, L, R, S = M through a 120 Hz Linkwitz-Riley low-pass) and pushes it to the engine's stream
  timeline (`LLAudioEngine::stream*`, `llaudioengine_openal.cpp`). The engine queues the same AL
  buffer on every source of a role, starts the group together (`alSourcePlayAtTimevSOFT`, else one
  `alSourcePlayv`), joins a late speaker at the next chunk boundary at the exact device time
  (`AL_SAMPLE_OFFSET_CLOCK_SOFT` + `alSourcePlayAtTimeSOFT`; without them the group restarts) and
  restarts every source together when any one stops. Falloff: full level out to `r`/4, then real inverse distance
  (−6 dB per doubling: `r` = 100 is −6 dB at 50 m, −10.6 dB at 85 m), `r` a soft maximum: the last 15 % of `r` fades
  to silence; `dir=` is an OpenAL cone (outer cone 90° wider,
  −20 dB behind). Speakers are traced as acoustics extra sources (the ray budget is 64: all 30
  channels + 34 such sources, enough for 32 speakers) and get occlusion, room send, reverb zone and diffraction pan; the
  stereo bed stays dry. Gains: music level, music mute and the teleport fade on every source; master
  and mute are the OpenAL listener gain. `t0=`: one seek per stream / `t0` to
  `(synced UTC − t0 + latency) mod length` when the plugin reports a length (GStreamer now does).
  Log: `ParcelAudio`, every 10 s.
- **macOS**: the mac MIME table names the libvlc plugin, but no VLC package exists for macOS, so
  there is no stream audio path to capture there yet (the same gap as for stream playback today).
