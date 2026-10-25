# Client-side AO (animation overrider)

Ticket: #496. Related: #428 / #500 (the `#Slipstream Helper` bridge), #463.

Slipstream replaces your avatar's built-in stand, walk, sit and other state
animations from the viewer. It needs no worn object, uses no script time and
works in no-script regions. It reads the common ZHAO-II / Oracul notecard
formats and the client-AO folder layout other viewers use, so existing sets work
without conversion.

Code: `indra/newview/slaoset.*` (states, set model, notecard parser),
`slaosets.*` (inventory, options, import), `slaoengine.*` (runtime),
`slfloaterao.*` (UI).

## Using it

- **Toolbar button "AO"** opens the compact control; the button is lit while the
  AO is on. **Me > Animation Overrider** (also in the Avatar menu) switches it on
  and off; **AO Control...** and **AO Editor...** open the windows.
- **Compact control**: on/off, set picker, previous/next stand, override sits,
  pause/resume, and a status line that says what plays or why the AO is paused
  ("Standing paused: 'OpenCollar' is animating you", "Sit paused: the seat
  animates you", ...). The gear opens the editor. A notecard or folder dropped on
  it is imported.
- **Editor**: sets (double-click or *Use This Set* to activate), states (bold =
  has animations, `>` = playing now, "(uses Standing)" = borrows another state's
  list), animations (order with Up/Down, *Preview* plays it locally on your avatar
  only, "failed" marks animations that would not load and are skipped). Per state:
  cycle, random, interval. Per set: override sits, smart sit, no stands in
  mouselook, no stands while typing. Global behaviour switches at the bottom.
- **Adding animations**: select a state, drag animations from inventory onto the
  editor. A link is created; the animation itself is never moved or copied.
- **Chat**: `/ao` (name set in Preferences > Chat commands):
  `/ao on|off|pause|resume|next|prev|set <name>|sets|sit [on|off]|status|edit`.
  Plain `/ao` toggles.

## Where sets live

| What | Where | Travels to other computers |
|---|---|---|
| Set and its animations | `#Slipstream AO/<set>/<state>/` in My Inventory, links to your animations | yes (inventory) |
| Other viewers' client-AO sets | `#Firestorm/#AO/<set>/<state>/`, used in place | yes |
| Options, order, active set snapshot | `sl_ao.xml` in the per-account settings folder, keyed by folder UUID | no (copy the file) |
| On/off, active set | per-account settings `SLAOEnabled`, `SLAOCurrentSet` | no |

Why options are per-account and not in a notecard: a notecard would need an
asset upload on every toggle, races between saves, and an asset fetch before the
AO can start after login. Folder UUIDs are stable across machines, so a copied
`sl_ao.xml` applies anywhere. Options are never encoded in folder names, so
folders shared with other viewers are never renamed; option flags found in
folder names are ignored, not imported. State folder names are the ZHAO-II section
names (`Standing`, `Walking`, `Sitting On Ground`, `Flying Up`, ...), matched
case-insensitively, ignoring anything after a `:`.

The active set is snapshotted (asset ids per state) so it plays right after
login, before inventory finishes loading; the real contents replace it once the
inventory fetch completes.

## Import

- **ZHAO-II**: `[ Standing ]anim one|anim two`, one section per line, repeated
  sections append, `#` comments. Sections: Standing, Walking, Running, Sitting,
  Sitting On Ground, Crouching, Crouch Walking, Landing, Standing Up, Falling,
  Flying, Flying Slow, Flying Up, Flying Down, Hovering, Jumping, Pre Jumping,
  Turning Left, Turning Right, Typing, Floating, Swimming Forward/Up/Down.
- **Oracul**: `[Stand.1]anim,anim`, slots such as `Stand.1`-`Stand.3`, `Stand.U`
  (hard landing), `Sit.N`, `Sit.G`, `Walk.N`, `Run.N`, `Turn.L/R`, `Jump.P`,
  `Jump.N`, `Fall`, `Land.N`, `Fly.N`, `Hover.N/U/D`, `Swim`. Tokens are matched
  loosely (case, spaces, dots and trailing slot numbers ignored), so similar
  vendor variants resolve too.
  Both `|` and `,` separators are accepted on any line (a line containing `|` is
  split on `|` only, so ZHAO names with commas survive).
- Unknown sections (vendor extensions such as settings blocks) are reported and
  skipped.
- Names resolve in the notecard's folder first, then the whole inventory; exact
  match first, then case-insensitive. Missing names are listed.
- Drop a notecard on either window, or select it (or the AO's folder) in the
  inventory window and press **Import...**. A dropped folder imports its AO
  notecard; with no notecard, state-named subfolders are copied as a set; with
  neither, its animations are added to the selected state.

## Taking animations out of a worn AO HUD or object

For moving from a scripted AO HUD to this one (code: `slaoobjectimport.*`).

- **AO Editor > Import from Worn HUD...** lists worn attachments with
  animations, HUDs first, with how many are no-copy and whether a notecard is
  inside (contents are read with `requestInventory`). **Import** copies the
  copyable animations into a new set `#Slipstream AO/<attachment name>/`; the
  copies live in its `Animations` subfolder and the states link to them.
- **States**: notecards inside (up to 6, preferring AO-looking names) are read
  straight from the object; the one naming the most of its animations maps them
  to states (ZHAO-II / Oracul, as in notecard import). The rest are placed by
  name ("OP Stand 3", "walk_f", "HoverUp", ...); whatever is not obvious appears
  under **Unsorted copies** in the state list, where *Assign* puts it in a state.
  The `Animations` folder can also be dragged from in the inventory window.
- **Drag from Contents**: an animation dragged from an object's Contents (Build
  floater or object inventory) onto a state is copied and linked into it; a
  notecard dragged from Contents imports the whole object using that notecard.
- **The scripted AO prompt** has *Import its animations*, which opens the import
  window on that HUD.
- **No-copy**: never taken (that would remove them from the HUD). They are
  counted and reported: "N animations are no-copy and stay in the HUD".
- **Copy path**: `LLViewerObject::moveInventory()` (MoveTaskInventory with the
  destination folder), the same request as Contents > Copy to Inventory. For a
  copyable item the region leaves the original and creates a copy; it is only
  ever sent for items that pass `allowCopyBy`. Requests go out 6 per 0.25 s; new
  items in the destination folder are matched to the requests by name (items
  that were there before are ignored). The window and the editor show
  "Copying from '<HUD>': n of N".
- **Detached mid-import**: nothing more is sent; copies that already arrived are
  kept and linked; the rest is reported as not copied. A job also ends after 20 s
  without a new copy.
- Deleting a set that holds copies asks first and sends the copies to the Trash
  with it (the originals are still in the object).

## Always, Idle and the derived states

- **Always** (folder `Always`; notecard `[ Always ]` or `[ Deformer ]`): body
  fixes (hand/foot poses, deformers). All of them play together the whole time
  the AO is on and are re-requested if missing from the server's list for 2 s
  (teleport, crossing, relog, a script stopping everything). Takeovers, sits,
  collars and typing do not stop them; switching the AO or *Always animations*
  off, or a full `@slao=n` hold, does. They are never treated as foreign.
- **Idle** (folder `Idle`): while the stand plays undisturbed (and the sit, if
  *also when sitting* is on), an idle replaces it after a random wait
  between the set's min and max (default 30-90 s), for its own length (from the
  keyframe data) or the cap (default 15 s; looped idles always use the cap), then
  the stands carry on. Which idle plays follows the Idle state's options, each
  idle counting as an entry: Cycle off, the first; Cycle on, the next in order,
  or with Random a random one never repeating (default: Cycle and Random on, as
  before; older files keep that). "Change only when entering the state" and the
  interval are greyed out for Idle: every idle is already an entry, and the
  min/max wait and the cap set the timing. The swap in and out is one message. No idles while
  paused (takeover, `@slao:stand=n`), moving or turning, in mouselook, or while
  typing when *No stands while typing* is on.
- **Derived states** the server has no animation for. Each falls back to its base
  state's list when the set has none:

| State (folder) | When | Falls back to |
|---|---|---|
| Walking Backwards | walk built-in + back key, no forward key, not flying | Walking |
| Striding | the server's `stride` built-in (walking into an obstacle) | Walking |
| Taking Off | hover-up within 1.5 s of going from a ground state to the air | Flying Up (then Hovering) |
| Away / Busy | standing while away (AFK) / do not disturb | Standing |
| Turning Left / Right | standing and turning: turn key, turn built-in, or the avatar's own rotation | Standing |
| Flying Left / Right | flying and turning (same three signals) | Flying |
| Running Left / Right | running and turning (same three signals) | Running |
| Swimming Surface | hovering within 0.5 m above to 0.75 m below the water height | Floating (Swimming Idle), then Hovering |
| Swimming Forward | flying (or moving at the surface) under water | Flying |
| Swimming Fast | flying under water with run on or the fast key | Swimming Forward |
| Floating (Swimming Idle) | hovering more than 0.75 m under water | Hovering |
| Swimming Up / Down | hover up / down under water | Hovering Up / Down |
| Sea Walk / Sea Run | walking / running more than 1 m under water | Walking / Running |

Turning from rotation (mouselook steering): the agent's yaw rate is measured
every frame; above 45 deg/s it counts as turning (positive is left), and it
stops after 0.25 s below 20 deg/s. Walking while turning stays Walking.
The up key on the ground starts Pre Jumping one round trip early when the set
has a pre-jump (or jump) animation.

Water depth is the region's water height minus the agent position (about hip
height). Striding was kept as the server's own state rather than guessed from
speed: the viewer cannot tell a "slow walk" from a walk, but the server does
signal `stride`.

### Notecard section names (checked by `/ao selftest`)

`/ao selftest` runs a real AO HUD notecard (OP AO, verbatim, quirks included:
commas with and without spaces, a trailing comma, trailing blanks, empty
sections, a name with a space, the same animation in several sections) through
the parser and checks every section. Mapping:

| Section | State |
|---|---|
| Standing / Walking / Running / Crouching / CrouchWalking | Standing / Walking / Running / Crouching / Crouch Walking |
| Falling Down / Jumping / PreJumping / Standing Up | Falling / Jumping / Pre Jumping / Standing Up |
| Landing, Soft Landing | Landing (both lists merge) |
| Flying / FlyingSlow / Hovering / Hovering Up / Hovering Down | Flying / Flying Slow / Hovering / Flying Up / Flying Down |
| Sitting / Sitting on Ground / Turning Left / Turning Right / Typing | same |
| Walking Backward | Walking Backwards |
| Striding / Taking Off / Away / Busy | Striding / Taking Off / Away / Busy |
| Flying Left / Right, Running Left / Right | Flying Left / Right, Running Left / Right |
| Swimming Up / Forward / Fast / Down / Surface | Swimming Up / Forward / Fast / Down / Surface |
| Swimming Idle | Floating |
| Sea Walk / Sea Run | Sea Walk / Sea Run (empty in that card) |
| Deformer | Always |

Unrecognised sections are listed in the import result.

## Walking backwards facing forward

Setting `SLWalkBackwardsFacingForward` (default on; compact control and editor:
*Walk backwards facing forward*). It works with the AO off.

Facing is decided on every viewer, for every avatar, by
`LLVOAvatar::updateOrientation`: the body follows the velocity, flipped to face
forward when moving backwards only while the server lists the walk built-in
(`ANIM_AGENT_WALK`) for that avatar. The simulator does not turn the avatar. So:

- **Walking backwards** already faces forward everywhere, as long as the walk
  built-in stays listed. The AO therefore never stops `ANIM_AGENT_WALK`
  (exception to "Stop system animations"); stopping it turned the avatar round
  for everyone.
- **Running backwards**: the server lists "run", not "walk", so other viewers
  turn the avatar round; our own view flips the velocity for self regardless.
  The viewer used to request the walk built-in as well while running backwards
  so other viewers kept facing forward, but walk and run then played together
  (two blended locomotion cycles on every viewer, and the AO, having stopped
  "run", fell back to Walking Backwards mid-run; #520), so it no longer does.
- Not applied while flying, sitting or on autopilot (click-to-walk, sit
  targets); mouselook keeps its own facing logic.
- The AO plays the set's Walking Backwards animations (falling back to Walk).

## Simulated swimming ("Allow swimming")

Setting `SLSimulatedSwimming` (global, default off; compact control and
editor). Global because it changes how the avatar moves, not which animations a
set has; it works with the AO off (then without swim animations). Code:
`slswim.*`, one cheap check per frame before the agent update is sent.

- **Entering**: walking (not flying, not on autopilot) where the water is over
  1.3 m deep and the hips are 0.2 m under the surface (heights from the agent's
  region's water height and the ground under the avatar, so neighbouring
  regions with other water heights work) turns flying on.
- **Floating**: with no vertical key held, slow vertical nudges (the "nudge up/
  down" controls) bring the hips to 0.6 m under the surface, with a 0.12 m dead
  band: from deep down this is a gentle rise (buoyancy), at the surface it holds.
- **Diving**: crouch / down (C, Page Down) descends; **E / Page Up** swims up;
  rising more than 1.5 m above the water leaves it flying (your choice to fly).
- **Leaving**: where the water gets shallower than 1 m (wading onto a beach)
  the flying we turned on is turned off and you walk.
- **Sea floor**: diving down until flight lands on the bottom walks there (Sea
  Walk / Sea Run) until you jump, which floats you up again.
- **Never fights you**: your own flight above or into water is left alone; if
  you switch flying off while floating, it waits until you are out of the
  water; sitting, teleports, crossings and autopilot pause it.
- **No-fly areas** (parcel, region, RLV `@fly`): no floating; a notice says so
  once per region and you walk on the bottom.
- **Animations**: the AO's water states (Swimming Surface / Forward / Fast /
  Idle / Up / Down) apply; the controller's nudges count as floating, not as
  swimming up or down.
- **Limits**: it is flight, so others see a flying avatar held at the water
  surface by the server's physics; there is no drag or current, and "nudge"
  speed is the server's.

## Keep head still

Setting `SLAOKeepHeadStill` (global, default on; editor). The viewer's
procedural head motion (`LLHeadRotMotion`, medium priority) turns the head,
neck and torso toward the look-at point on top of any animation that leaves
those joints alone, and every other viewer does the same to our avatar from the
look-at target we broadcast. A priority-4 replacement that animates the head
already wins on those joints (per-joint priority), so the bleed is from
replacements that leave the head free. While a replacement plays and the AO is
not paused, the motion is stopped on our avatar, the look-at point is dropped,
the current look-at target is cleared for everyone, and `LLAgentCamera::setLookAt`
sends no target (as the existing private look-at setting does). Off, paused or
AO off: the head motion starts again and look-at targets are sent from the next
one the camera sets. Eye motion (`ANIM_AGENT_EYE`) is left alone; it moves only
the eyes.

## How a state becomes a replacement

1. The server's `AvatarAnimation` message for self lists every playing animation
   with the object that started it. The built-in state animations give the
   posture state (highest rank wins: ground sit > sit > landings > jumps > fall >
   hover/fly > crouch > run/walk > turn > stand). Typing is a separate overlay.
2. For that state the active set's animation is requested with `AgentAnimation`
   (the same request gestures use). Start of the new animation and stop of the
   previous one go in **one message**, so observers never see a gap.
3. **System animations.** A client AO cannot replace a state server-side the
   way `llSetAnimationOverride` does: the server always plays its built-in and
   ours plays on top, so the built-in shows on joints ours leaves alone and on
   priority ties. With the set's **Stop system animations** (default on) every
   posture built-in in the list is stopped while a replacement plays (stand,
   walk, run, crouch, turns, jumps, landings, fly, hover, object sit, and the
   built-ins under the derived states), the built-in typing animation under a
   Typing replacement, and the away slump under an Away replacement. If the
   server lists a stopped built-in again (it re-asserts walk or stand on some
   updates) it is stopped again, at most 6 times per 10 s. When the AO lets go
   (pause, takeover, AO off, no animation) the built-in is given back at once,
   so the avatar is never left without one. Exceptions:
   - **Walk**: `ANIM_AGENT_WALK` is never stopped, see "Walking backwards facing
     forward".
   - **Ground sit**: removing its built-in makes the viewer stand the avatar up
     (`LLVOAvatar::sitDown(false)`), so it is never stopped.
   - **Do not disturb**: the viewer re-asserts that animation itself.
   - **Seats that animate you**: the built-in sit is not given back (poseballs
     stop it themselves; giving it back would fight them).
   Replacements whose priority is below 3 are marked "low priority" in the
   editor and logged once: they rely on the built-in being stopped.
   The clean server-side alternative, calling `llSetAnimationOverride` with the
   set's animations, would run in the `#Slipstream Helper` (#500); not done yet.
4. Fallbacks: no turn animations keeps the stand playing; no hover-up/down uses
   hover; no medium landing uses landing; no fly-slow uses fly; water states use
   the hover/fly lists.
5. **Which animation plays** (per state, any state): with **Cycle** off, always
   the first animation of the list. With Cycle on, it changes every *interval*
   seconds while the state lasts, or, with *Change only when entering the
   state*, once each time the state starts (then that one loops). *Random*
   (cycling only) picks at random, never the same twice in a row; otherwise
   the list order. Defaults: only Standing cycles (on the timer); everything
   else plays its first animation until you turn Cycle on. The position in the
   list lasts for the session. *Next/previous* switch by hand.
6. **Furniture (AVsitter / multi-pose)**: sit, switch poses, swap seats; stands
   never fight. Furniture without animations: AO sit after 0.75 s.
7. **Dance HUD**: start a dance: stands pause, walking works; stop: resume.
   Hand-pose/facial HUD running all the time: stands keep playing.
8. **RLV**: `@sit:<uuid>=force` onto animated furniture with `@unsit=n`: the
   seat's animation wins even with smart sit off; `@clear` releases `@slao`
   holds from the same object (`@clear=<filter>` only those whose name contains
   the filter, e.g. `@clear=sit` only `@slao:sit`); `/ao resume` releases every
   hold; RLV off: `@slao` commands still work and are not
   shown in chat.
9. **`@slao` commands**: from a worn test script: `@slao:stand=n` pauses stands
   ("Paused by '<name>'"); `@slao:stand=y` resumes; detach while held: released
   after 15 s; `@slao=4711` replies; `@slao:set:<name>=force` switches set;
   mixed line `@slao=n,detach=n` pauses and RLVa still applies `detach=n`.
10. **Scripted AO**: wear a ZHAO-II HUD and walk/stand a few times: the prompt
    appears; *Pause mine while it's worn*: status "deferring"; detach: AO
    resumes. Wear a HUD using `llSetAnimationOverride`: those states are not
    replaced by ours.
11. **No-script region**: everything above that does not need a script works:
    the AO itself, smart sit on scripted-free sits, `/ao`.
12. **Import**: a ZHAO-II notecard in a folder with its animations: set created
    with links only (originals untouched, including no-copy ones), order kept;
    missing names reported; an Oracul `SET` notecard; a set in
    `#Firestorm/#AO` shows up with that folder's viewer name and plays.
13. **Failures**: add an animation whose asset is missing: marked "failed" after
    45 s, skipped in cycling.
