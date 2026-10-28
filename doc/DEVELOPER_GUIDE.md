# Slipstream developer guide

Welcome. This guide is for contributors who work on their own branches and open pull requests
into `main`. It covers the layout of the repo, how to build, the daily workflow, commit and
engineering conventions, and how to get the most out of Claude Code on this codebase. It is
written so that your Claude follows the same conventions the owner's Claude does: point it at
this file (and `doc/CODING_RULES.md`) at the start of a session, and put the pointer in your own
`CLAUDE.md` or memory.

Slipstream is a Second Life viewer forked from the Linden Lab / Alchemy line, with its own Vulkan
renderer. The legacy OpenGL pipeline is retired. Where this guide and the source disagree, the
source wins; please fix the guide in the same PR.

Contents: 1 Repo layout, 2 Building, 3 Daily workflow, 4 Commit conventions, 5 Engineering
rules, 6 Testing and releases, 7 Working with Claude Code, 8 Who to ask.

---

## 1. Repo layout essentials

- `indra/` is all the viewer C++ (Linden's "indra" naming). Almost everything outside it is
  build, packaging or CI plumbing.
  - `indra/newview/` is the viewer application: UI, world objects, and the renderer glue.
  - `indra/newview/vkgfx/` is the Vulkan renderer: device/swapchain/pipeline plumbing
    (`vk_*.cpp`), and the "clean" GPU-driven pipeline (`llclean*vk.cpp`: clustered geometry,
    ray tracing, probes, weather, post effects, ...).
  - `indra/vkclean/shaders/` holds the GLSL for the Vulkan pipeline (`clean_*.comp`, `.frag`,
    `.vert`, `.glsl` includes). It is compiled to SPIR-V and embedded **at build time** by the
    CMake target `vk_shaders` (`indra/vkclean/regen_world_spv.py`). The generated `vk_*_spv.h`
    headers land in the build directory. **Never commit generated `vk_*_spv.h` files.**
  - `indra/newview/app_settings/` is the settings and legacy shader data; the Vulkan pipeline does
    NOT use the GLSL under `app_settings/shaders` for world rendering.
  - `indra/cmake/` is CMake includes. `00-Common.cmake` has compiler flags; `Prebuilt.cmake` plus
    `3p_manifest.json` fetch the third-party prebuilt libraries.
  - `indra/CMakePresets.json` defines the configure presets (`linux-releaseos`, `linux-release`,
    `windows-cross`).
- `scripts/` is helper scripts: `wincross/` (Windows cross-build), `ci/` (versioning, release,
  lints), `3p/manifest.py` (third-party manifest tool), `testctl/` (automated validation),
  `perf/regprobe/` (shader register counts), `packaging/`.
- `doc/` has `CODING_RULES.md`, protocol notes, and `doc/testplans/`.
- `.forgejo/workflows/` is CI (`release.yml` is the authoritative description of a clean build
  and release).
- Research and design notes live in a **separate private repository**, `sky_line/slipstream-research`
  (the owner will add you). They are not in this repo and are not visible inside worktrees; when
  you brief an agent on a ticket, search the research repo for that ticket number and give the
  agent the absolute paths of the relevant notes.

Two quirks worth knowing early:

- The Vulkan world shaders in `indra/vkclean/shaders` are the only ones that matter for the
  Vulkan viewer. Editing a GLSL file under `app_settings/shaders` and seeing no change is the
  classic symptom of editing the wrong set.
- `CLAUDE.md` in the repo root is the owner's file and has historical notes (autobuild era, a
  retired GL bug). Treat its "Build system" banner (plain CMake + Ninja) as current, and
  ignore the GL-only material.

## 2. Setting up a build

### 2.1 Linux

Supported and what CI uses: x86_64, Arch/CachyOS locally, Debian/Ubuntu-family for packaging.
You need CMake >= 3.21, Ninja, a recent GCC (the default; Clang works for the Windows cross),
Python 3, git, the Vulkan loader and headers, and a SPIR-V compiler (`glslc`).

Arch / CachyOS:

    sudo pacman -S --needed base-devel cmake ninja git python pkgconf libpulse libunwind \
        libxinerama libxcursor libxfixes libxft libpipewire dbus vlc gstreamer \
        gst-plugins-base vulkan-headers vulkan-icd-loader shaderc glslang

Debian / Ubuntu (24.04 or newer; package names are the usual ones, adjust if one is missing):

    sudo apt install build-essential cmake ninja-build git python3 python3-venv pkg-config \
        libpulse-dev libunwind-dev libxinerama-dev libxcursor-dev libxfixes-dev libxft-dev \
        libpipewire-0.3-dev libdbus-1-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
        libvlc-dev libvulkan-dev glslc glslang-tools libdecor-0-0 libdecor-0-plugin-1-cairo

Then, from the repo root:

    make                 # full build: venv, configure, compile, package (default preset linux-releaseos)
    make configure       # configure only (this also downloads and unpacks the 3p packages)
    make run             # launch the packaged viewer (build/newview/packaged/slipstream)
    make clean           # remove build-linux-x86_64 (keeps caches)
    make help            # all targets and variables

`make` runs the source checks (`make check`) first, then `cmake --build`, then the package
checks (`make check-package`); see "Checks and regression guards" in section 6.

Incremental work: after the first full build, iterate with

    ninja -C build-linux-x86_64 slipstream-bin

(no environment setup needed). Re-run `make` when you need the packaged tree to be refreshed
(launcher, app_settings, libs) before `make run`.

Useful variables: `make PRESET=linux-release` (with proprietary packages, rarely needed),
`make EXTRA_CMAKE_FLAGS=...`, `make VIEWER_CHANNEL="..." BUILD_ID=...` (cosmetic: title bar and
`build_data.json`). Local builds identify as "Slipstream Test x.y.z.0". The Makefile passes
`-DGCC_DISABLE_FATAL_WARNINGS=ON` so a newer GCC does not stop on new warning categories.

The 3p download cache: third-party prebuilt libraries (Boost, SDL3, OpenSSL, ...) are
downloaded, hash-verified and unpacked by CMake itself (`indra/cmake/Prebuilt.cmake`, driven by
`indra/cmake/3p_manifest.json`) into `build-linux-x86_64/packages/`. The downloaded archives are
kept in a cache directory, default `~/.cache/slipstream-3p`; override it with the `SL_3P_CACHE`
environment variable or `make SL_3P_CACHE=/path`. Keep it: it is about a gigabyte, and shared
between build directories, so a second checkout or a clean rebuild does not re-download.
Do not hand-edit `3p_manifest.json`; use `python3 scripts/3p/manifest.py list|add|update|remove|verify`.

Disk and time: plan for 30+ GB across the packages and build directory, a few minutes on a
many-core machine and well over half an hour on a laptop. Running a build in the background
while you keep working is fine; do not take performance measurements while one is running.

The viewer needs a recent Vulkan-capable GPU and driver (some features, such as ray tracing,
need more capable hardware and are feature-tiered). If it does not start, the log is
`~/.slipstreamviewer/logs/Slipstream.log`.

### 2.2 Windows

**Finding (from the presets and CMake in this repo):** there is no native Windows (MSVC) build
configuration. `indra/CMakePresets.json` has only `linux-releaseos`, `linux-release` and
`windows-cross`, and the Windows preset is conditioned on a Linux host and uses a clang-cl
toolchain file pointing at the xwin-splatted MSVC sysroot. The top-level `Makefile` is a
bash/Linux wrapper. A few `if (MSVC)` branches survive in `00-Common.cmake` from upstream, and
`viewer_manifest.py` knows Windows, but nothing tests a native build, and the project's own
rule (`doc/CODING_RULES.md`) is that **the clang-cl cross-build from Linux is the only Windows
compiler we test with before CI**. So: do not expect Visual Studio to work, and do not spend
time trying to make it. Build on Linux and run on Windows.

Recommended setup for a Windows machine: WSL2.

1. Install WSL2 with Ubuntu 24.04 (`wsl --install -d Ubuntu-24.04`) or an Arch image. Arch is
   the closest to the owner's environment and has current clang; on Ubuntu you will want clang,
   lld and llvm 22 from apt.llvm.org because the CI and the toolchain file are written against
   LLVM 22 (`clang-cl`, `lld-link`, `llvm-rc`, `llvm-mt`, `llvm-lib`, `llvm-dlltool`).
2. **Keep the checkout inside the WSL filesystem** (for example `~/src/slipstream`), not under
   `/mnt/c`. Builds on the Windows drive are slow and break case sensitivity and permissions.
   You can read the output from Windows via `\\wsl$\Ubuntu-24.04\home\<you>\src\slipstream\...`.
3. Install the Linux packages from 2.1, plus the cross-build prerequisites: `clang lld llvm`
   (22), `7zip` (the `7z` command), `nsis` (`makensis`, for the installer), the Vulkan loader and
   headers, and `xwin` (`cargo install xwin`, or a release binary).
4. Splat the MSVC CRT and Windows SDK once (this accepts Microsoft's license; read it):

       xwin --accept-license splat --output ~/.xwin

   `XWIN_ROOT` overrides the location (default `~/.xwin`).
5. Build and package:

       make windows

   This runs the macro lint, then `scripts/wincross/build.sh`, which fixes case-sensitivity
   links in the sysroot, fetches the VC redistributable DLLs into `~/.cache/wincross/crt`
   (override with `WINCROSS_CRT`), generates a Vulkan import library, configures the
   `windows-cross` preset into `build-windows-x86_64/`, builds with Ninja, and packages. Output
   is in `build-windows-x86_64/package/`: `Slipstream_<label>_win64.zip`, plus
   `Slipstream_<label>_Setup.exe` when `makensis` is installed.
6. Copy or open the zip/installer from Windows and run it **on the Windows host**. GPU testing
   happens on the host: WSL2 exposes a Vulkan device only through the Mesa `dzn` layer, which is
   not what the viewer targets. Treat WSL2 purely as the compiler.
7. Incremental cross-builds, as the coding rules describe:

       bash scripts/wincross/configure.sh
       source scripts/wincross/env.sh
       ninja -C build-windows-x86_64 slipstream-bin

   (`env.sh` puts the repo venv's Python on `PATH`; `make venv` creates it.) Use a full
   `make windows` when you need the packaged zip.

Windows-specific notes:

- The Windows cross-build is also the check for anything touching `#ifdef LL_WINDOWS`,
  `llwindow*`, `llfilesystem`, audio/media plugins, or code that includes `<windows.h>`: run it
  before you open a PR.
- Windows SDK macros are the usual cause of "compiles on Linux, breaks on Windows": see
  `doc/CODING_RULES.md` and section 5.
- Precompiled headers are intentionally OFF for the Windows cross-build (clang-cl loses
  exception copy-constructors through a PCH, which corrupts memory at login). Do not turn
  them on.
- If you cannot or do not want to use WSL2, a plain Linux VM or any Linux machine does the same
  job; only the final run needs Windows.

## 3. Daily workflow

Work is tracked as issues on Forgejo: https://git.slipstreamviewer.org/sky_line/Slipstream
(web UI) and `https://git.slipstreamviewer.org/api/v1` (API, with your own token; see 7.3).
The git remote for the project is conventionally named `forgejo`.

1. **Pick a ticket.** Look for open issues, preferably `status/up-next` ones, or something the
   owner hands you. Labels: type (`bug`, `feature`, `perf`, `crash`, `epic`), area (`rendering`,
   `rt`, `ui`, `avatars`, `platform`, `amd`), workflow (`status/up-next`, `status/in-progress`,
   `status/testing`, `triage`, `cannot-reproduce`, `known-limitation`) and `priority/critical`,
   `priority/high`, `priority/medium`, `priority/low`. List them with `GET .../labels`.
2. **Claim it before you start.** Check that the issue has no assignee and no `status/in-progress`
   label. If someone has it, talk to them or pick another. Then assign yourself and add the
   `status/in-progress` label. This is what stops two people (or two people's agents) colliding on the
   same ticket. **The assignee field is authoritative:** a ticket assigned to someone else is theirs
   -- do not work on it, edit it or relabel it without asking them. New reports are not
   auto-assigned; whoever picks a ticket up assigns it at that moment.
3. **Branch from `main`**, named `<ticket>-<slug>`, for example `1052-cluster-lod-seam`:

       git fetch forgejo
       git checkout -b 1052-cluster-lod-seam forgejo/main

   Keep branches small and about one ticket. Rebase on `main` before opening the PR.
4. **Commit** following section 4, building and testing as you go.
5. **Open a pull request into `main`** with the ticket linked (`Fixes #1052` or `Refs #1052` in
   the PR description). Say what you tested and on what hardware, and quote the `Test-Plan`
   text. You do not have push access to `main`; the owner reviews and merges.
6. **Review.** The owner reviews like agent work: builds it once and tests it. Expect review
   comments on config-space growth, per-object CPU work in render paths, and missing failure
   handling (section 5). Push follow-up commits to the same branch.
7. **After merge**, swap `status/in-progress` for `status/testing`: the change is on `main` and Jetstream
   builds (section 6) carry it to testers.
8. **Closing: close-on-ship.** A ticket is closed only after the fix is in a published release,
   even if it is confirmed working in-world. The owner sweeps and closes tickets after each
   release; do not close tickets yourself on merge.

Do not push to `release/*` branches or tag anything; releases are the owner's.

### 3.1 Triage and ticket ownership

- **Triage** (new `[viewer]` reports from the in-viewer Report Bug relay, and Discord #help): reproduce
  or ask the reporter for what is missing (logs via Help > Report Bug, never a public paste of
  Help > About), find duplicates, and set area and `priority/*` labels. Then either fix it (assign
  yourself, `status/in-progress`, PR), escalate it, or close it (duplicate, cannot reproduce,
  works as intended -- with a short, friendly comment).
- **Escalate to the owner** (assign them, add a triage note: repro steps, logs, suspected area)
  for: device lost / GPU hangs, ray-tracing / GI / shadow correctness, crash root causes,
  vendor-specific performance (AMD / Intel / older NVIDIA), anything that writes inventory to the
  server, and anything security-related (report security issues privately, never in a public ticket).
- **Priority:** `priority/critical` = crash or data loss with no workaround; `high` = common
  breakage; `medium` / `low` otherwise. The owner may override.
- Both the owner and you fix bugs; ownership of a ticket is simply its assignee.

## 4. Commit conventions

- **Subject:** `#NNN: what changed`, imperative, one line, for example
  `#1052: cluster LOD -- stop the seam at the 2->3 transition`. Non-ticket housekeeping uses a
  topic prefix such as `doc:`, `perf:` or `ci:`.
- **`#` subjects and git:** git treats lines starting with `#` as comments in the editor
  message. Commit with the comment character changed so subjects survive, on every command that
  opens or reuses a message:

      git -c core.commentChar=';' commit
      git -c core.commentChar=';' cherry-pick -x <sha>
      git -c core.commentChar=';' rebase --continue

  Passing `-m "#1052: ..."` on the command line is also safe, but `rebase --continue` and
  `cherry-pick --continue` open the editor and will otherwise drop the line. You can set it
  permanently for this repo with `git config core.commentChar ';'`.
- **Body:** why, not just what; root cause for bug fixes; measurements for perf changes.
- **Test trailers.** Any commit that changes something a tester can see or check ends its body
  with trailers:

      Test-Plan: <what to do and what should happen; continuation lines are indented>
      Test-Settings: SettingA = 1, SettingB = 3

  CI collects these for each build and posts the plan to the testers' test-plans channel (per
  Jetstream build and per beta, with `scripts/ci/test_plan.py`; preview locally with
  `python3 scripts/ci/test_plan.py <tag> --dry-run`). A commit without trailers lands under
  "Also changed", which testers skim. Pure refactors and docs need no trailers.
- **AI-assisted commits** carry the co-author trailer your Claude Code session gives you, for
  example `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. Keep it at the end.
- Never commit generated files (`vk_*_spv.h`), build output, tokens, logs, or personal data.
- Never name other viewers in commit messages, PR text or tickets (see section 5).

## 5. Engineering rules

Each of these exists because breaking it hurt before. Reviewers will hold you to them.

**Coding rules (`doc/CODING_RULES.md`).** Windows SDK macro names are banned as identifiers:
`far`, `near`, `FAR`, `NEAR`, `interface`, `PASCAL`, `CALLBACK`, `WINAPI`, `CONST`, `min`, `max`,
`small`, `ERROR`, `OPAQUE`, `TRANSPARENT`, `DELETE`, `IN`, `OUT`, `OPTIONAL`, `ABSOLUTE`,
`RELATIVE`, `DIFFERENCE`, and function names that collide with the Win32 A/W macros
(`CreateWindow`, `SendMessage`, `GetObject`, ...). Linux never sees these, so they only break
the Windows build, after merge. Use `nearPlane`, `farDist`, `isSmall`. The lint
(the `win-macros` check in `scripts/check/`) runs at the start of `make`, `make windows` and CI;
run it yourself with `make check`. Add a name to its list when another one bites.

**Minimal configuration space.** Fewer settings, fewer paths. Non-default paths must earn their
place and are removed when they do not. Experiment and A/B settings are temporary: define them
with `Persist=0` (never saved), set them per session with `--set Name value`, and decide to
promote the winner or delete the loser before the PR merges. Validation runs the shipping
defaults, so a saved experiment value can hide a broken default path. Measure before merging
(fixed-camera A/B, `scripts/perf/regprobe` for shader register counts).

**Allocation failures never crash.** A failed RAM or VRAM allocation is not an `LL_ERRS`, an
abort or a soft-quit: keep what you have, retry a few frames later, then drop the asset or
request and log it (rate-limited, with what failed, the size and the memory pressure state).
New allocation sites must handle failure softly, especially on per-frame paths.

**No unbounded growth.** High-water-mark buffers are fine; any store, table or cache that grows
without bound is not. Refcount or reap unused entries, reuse freed slots (GPU slots only after
in-flight frames retire), give content-keyed caches an LRU or byte budget, and expose
live / capacity / peak in the periodic stats log. Be careful reading stats: some fields are
high-water marks, not live counts.

**Free on liveness.** Release materials, textures and geometry once no live object uses them,
after a short grace period. Do not keep a left region's assets resident in case the user comes
back. Memory budgets are a backstop, not the primary mechanism. Pressure-driven degradation
(lowering effective caps under memory pressure) must be soft and self-restoring, not a silent
rewrite of the user's settings.

**Logs keep everything except PII.** Log device, state and failure details generously, check
return codes on driver/device init and log failures, and scrub only personal data (names,
chat/IM text, credentials, tokens, session ids, IPs, URLs with secrets). Hardware and driver
info is not PII. Do not drop whole subsystems from bug-report excerpts for privacy; scrub the
fields.

**GPU-driven rendering.** New render paths are GPU-driven by design: persistent instance
buffers, bindless/descriptor-indexed textures, compute culling, multi-draw indirect. Per-object
or per-face CPU uniform updates, binds and draw calls are forbidden. "Correctness first, make it
GPU-driven later" is a trap: the architecture is the correctness model, and bolting features
onto a CPU loop locks in the wrong shape. Per-frame CPU work should be bounded by "push deltas
for changed instances", not "iterate visible faces".

**Standard pipelines over upstream parity.** Matching the inherited Linden behaviour is not the
quality bar. When a subsystem shows artifacts or costs, compare against modern engine practice
(clustered lists, Hi-Z tracing, compute-driven async updates, bindless / multi-draw) and file or
implement the standard replacement. "The old code does it too" explains a bug; it does not
close the ticket. Prefer replacing inherited code over wrapping it in guards and sweeps.

**Vulkan specifics.** Never mix CPU writes and compute writes to the same buffer. Never read
queued pipeline handles during init (pipelines compile asynchronously; use the
`createGraphicsPipelineInto` / `bindGraphicsPipeline` pattern). Cross-subgroup scans need the
full-subgroups requirement even on NVIDIA. Heed validation-layer output (section 6).
Slot 0 in an instance table is a valid id; empty entries need an explicit sentinel.

**Inventory changes must never risk server-side loss.** Classify any inventory change as
client-only, read-only server traffic, or server write. Server writes (create, move, copy, link,
delete/purge, outfit edits, AIS requests, UDP inventory messages) need extra care: use the
server's documented capabilities and limits, check per-item results, never auto-retry
non-idempotent writes, never reorder in a way that changes meaning, give caches provable
invalidation (version counters), and never weaken trash/removability/RLV/outfit protections.
Test with an alt account and compare against a backup.

**Never ship other viewers' unreleased work, and never name other viewers.** Do not take code,
art or features from any third-party viewer's unreleased work (development branches, PRs,
feature branches). Only released work is eligible, with credit, and even then prefer our own
implementation. Linden Lab's viewer code is the upstream base and is exempt. Do not mention
other viewers by name in commits, PRs or tickets.

**Network draw distance is capped at 512 m.** Do not raise it or work around the cap.

**Contributor credit.** Only code that lands is credited; credit goes in the release notes and
the About listing, not in per-item commit text.

## 6. Testing and releases (overview)

You are not expected to cut releases, but you should know how the pieces fit.

- **Builds:** `main` is the development line. CI builds a **Jetstream** pre-release from `main`
  every six hours when it has new commits (tags `jetstream-X.Y.Z.N`), for testers who want the
  newest code. Jetstream builds post the collected `Test-Plan` trailers.
- **Branch model:** `main` = dev. One `release/X.Y` branch per minor version carries that
  version's betas, final and hotfixes as **tags**. Fixes land on `main` first and are
  cherry-picked into the release branch with `git cherry-pick -x`. Release-branch-only commits
  never go back to `main`. `main` must never lack a fix that a beta has.
- **Tags drive CI** (`.forgejo/workflows/release.yml`): `vX.Y.Z-beta.N` builds a Beta
  pre-release, `vX.Y.Z` a Release, `jetstream-*` a Jetstream, `ci-*` a dry run. The tag must
  match `indra/newview/VIEWER_VERSION.txt`. Version is MAJOR.MINOR.PATCH.BUILD, with BUILD
  derived from tags (`scripts/ci/version.sh`; preview with `make version TAG=v1.5.0-beta.2`).
  MAJOR = a substantially different program, MINOR = features and new systems, PATCH = bug and
  hot fixes only. Only the owner tags.
- **Validation before every beta.** The exact release candidate runs under the Vulkan validation
  layers and every error is fixed or explicitly accepted before the beta tag. The automated
  harness is in `scripts/testctl/`: `slvalidate.sh` launches a test-control build
  (`make TESTCTL=1`; refused for CI release builds) under the layers in "core" or "sync" mode,
  `slval.py` walks every graphics setting, floaters, snapshots and teleports writing step
  markers into the log, and `slval_report.py` attributes validation messages to the step that
  caused them. This runs on a dedicated test machine and is the owner's step. To check your own
  change locally with validation, launch your build through the loader layers (this is much
  slower than normal; use short runs):

      VK_LOADER_LAYERS_ENABLE='*validation' VK_LAYER_REPORT_FLAGS=error,warn \
      VK_LAYER_DUPLICATE_MESSAGE_LIMIT=20 build-linux-x86_64/newview/packaged/slipstream

  Layer output goes to stdout, not `Slipstream.log`. Add `VK_LAYER_VALIDATE_SYNC=1` for
  synchronization validation, in a separate short run (core + sync together is too slow to
  play). Do not use the viewer's `SLIPSTREAM_VK_VALIDATION` variable for general play: its
  GPU-assisted mode makes the viewer unusable.
- **Platform checks:** the `package` stage (`make check-package`, formerly `make glibc-check`)
  fails if a packaged Linux binary needs a newer glibc or the x86-64-v4 instruction set than we
  promise, so tarballs run on other people's machines. New libm symbols are pinned in
  `indra/cmake/glibc_compat.h`.
- **Hardware coverage:** the renderer is tuned on three tiers (a high-end, a mid-range and an
  older Pascal card). Performance claims should say what GPU they were measured on.

### Checks and regression guards

All regression guards live in one framework, `scripts/check/`. **The rule: a new regression
guard is a new module in `scripts/check/checks/`, with its ticket reference in the module. It
is never a new workflow step, Makefile loop or ad-hoc script.** CI and the Makefile only run
the framework, so a new module is picked up everywhere with no other edit.

A check module declares `NAME`, `STAGE`, `DESCRIPTION`, `GUARDS` (what regression it prevents,
with the ticket / release references) and `run(ctx, result)`, which calls `result.fail(msg)`
for a problem (fails the run) or `result.warn(msg)` (shown, never fails) and sets
`result.summary`. Python 3 standard library only. Existing modules are the templates; the
helpers for packaged-binary checks are in `scripts/check/framework.py`.

Stages:

- `source`: runs on the repository tree, needs no build, takes seconds. `make check`.
  `make`, `make windows` and CI run it first.
- `package`: runs on the packaged viewer (`build-linux-x86_64/newview/packaged`).
  `make check-package` (alias: `make glibc-check`); `make` runs it after the build. Limits are
  `GLIBC_MAX`, `GLIBCXX_MAX`, and `GLIBCXX_STRICT=1` (CI) to make a libstdc++ overshoot fail
  instead of warn.

Other commands: `make check-list` lists every check and what it guards;
`python3 -I scripts/check/run.py --only NAME [--package DIR]` runs a single check.

The runtime suite is separate because it needs the test machine: `make validate PKG=<TESTCTL
package dir> NAME=<run name> SCENARIO=core|sync` wraps `scripts/testctl/slvalidate.sh` and is
the standard pre-release runtime regression suite (see "Validation before every beta" above).

In CI (`release.yml`, Linux job) the checks are the two steps **Source checks** and **Package
checks**; the Windows job has **Source checks** too. A failed guard shows as a red step, and
the run's log has a PASS / WARN / FAIL table with one line per check followed by the details.

## 7. Working with Claude Code on this repo

### 7.1 Give your Claude the conventions

Start sessions by pointing Claude at this guide and at `doc/CODING_RULES.md`, or put a short
note in your own `CLAUDE.md`: "Follow doc/DEVELOPER_GUIDE.md and doc/CODING_RULES.md." Every
brief to a sub-agent that touches C++ should repeat the key points: no Windows macro names as
identifiers, test trailers, no builds (below), no generated headers in commits.

### 7.2 Agent conventions

- **Agents claim before they work.** An agent picking up a ticket first checks its assignee:
  assigned to someone else -> stop and report, don't touch it; unassigned -> assign it to the
  person whose session it is and add `status/in-progress` before starting.
- **Agents work in worktrees and do not build.** Use `git worktree` (Claude Code's worktree
  isolation does this) so parallel agents do not trample each other. Agents commit and report
  "not built"; the human or main session merges the branches, builds **once**, and fixes
  compile errors. This avoids agents queuing on one build directory. A shader-only compile check
  is fine for an agent: `python3 indra/vkclean/regen_world_spv.py --out-dir <scratch dir>`.
- **Keep generated headers out of commits.** The SPIR-V headers are produced into the build
  directory by CMake. If one shows up in `git status`, it is in the wrong place; do not add it.
- **One simple shell command per call, absolute paths.** In auto-permission modes, compound
  commands (`cd x && y | z`) are denied without a verdict and stall agents. Use the Read, Grep
  and Edit tools for files, one plain command per Bash call, absolute paths, and temp files in
  the session scratchpad. Put this sentence in agent briefs.
- **Long commands run in the background** (builds, `make run`, log-poll loops); do not block the
  session on them. Never gate a build on `pgrep -f` (it matches its own shell); use
  `pgrep -x ninja` or just build.
- **Do not kill a running viewer** on your own initiative; ask the person who launched it to
  close it. Package every passing build into its own snapshot directory and run the viewer from
  the snapshot, so a rebuild never rewrites a library a running viewer has mapped (packaging in
  place has crashed running viewers with SIGBUS).
- **Diagnose before declaring a root cause.** Prefer measured evidence (a numeric readback, a
  fixed-camera A/B, driver register counts, a captured log) over colour-guessing or theories;
  compare against a released build before explaining a regression.
- **Research before briefing.** Search the research repo for the ticket number and hand agents
  the file paths; agents in worktrees cannot see your other checkouts.
- **Model split (suggestion):** a small fast model for search, triage and mechanical edits; a
  mid-size model for ordinary tickets; the strongest model for rendering architecture,
  concurrency, memory ownership, inventory and crash root-causing.
- **Release notes and tracker writes are shared surfaces.** Do not post sim-lag or
  crash-trigger techniques to the public tracker or chat; describe the client-side work only.
  Do not paste About-box or log contents publicly, since they carry personal and location data;
  ask reporters to use the in-viewer report tool.

### 7.3 Finding tickets with the Forgejo API

Create your own access token in the Forgejo web UI (Settings -> Applications) with repository
and issue scopes. Keep it **out of the repo and out of your shell history**: store it in a file
only you can read (for example `~/.config/forgejo-token`, mode 600) and read it from there.
Never print it, paste it into a prompt, put it in a commit, or leave it in a script in the tree.
Your Claude should read the file in a command rather than echo it.

    # open tickets labelled status/up-next
    curl -s -H "Authorization: token $(cat ~/.config/forgejo-token)" \
      "https://git.slipstreamviewer.org/api/v1/repos/sky_line/Slipstream/issues?state=open&type=issues&labels=status/up-next&limit=50"

    # one ticket and its comments
    curl -s -H "Authorization: token $(cat ~/.config/forgejo-token)" \
      https://git.slipstreamviewer.org/api/v1/repos/sky_line/Slipstream/issues/1052
    curl -s -H "Authorization: token $(cat ~/.config/forgejo-token)" \
      https://git.slipstreamviewer.org/api/v1/repos/sky_line/Slipstream/issues/1052/comments

Claiming a ticket is a `PATCH` on the issue (`{"assignees":["<your-login>"]}`) plus a label
change (`POST .../issues/1052/labels` with `{"labels":[<label ids>]}`; list ids with
`GET .../labels`). Doing it in the web UI is just as good. Opening the pull request from the
command line is `POST /repos/sky_line/Slipstream/pulls` with `head`, `base` = `main`, `title`
and `body`.

### 7.4 Where design docs live

- This repo: `doc/` (protocol notes, `doc/testplans/` examples, this guide, coding rules) and
  per-subsystem READMEs under `scripts/` (for example `scripts/perf/regprobe/README.md`, which
  explains getting shader register counts from the driver in about a minute without a viewer
  build, using Vulkan pipeline executable statistics).
- The private research repository `sky_line/slipstream-research`: design notes, benchmarks,
  roadmaps, per-ticket investigations. Check it before designing anything non-trivial; the
  answer to "why is it like this" is often there. Add your own non-sensitive notes there.
- Tickets: the discussion on the issue is the record of the decision. Summarise measurements
  and the final design on the issue when you finish.

## 8. Who to ask, and where to report

- **The owner** is the first stop for priorities, review, release questions, access (research
  repo, tokens, anything that needs a server-side change) and "is this in scope".
- **Discord:** the project server's dev channel is the place for quick questions, status and
  coordination. Test results for Jetstream and betas go to the testing channels there.
- **Bugs you find** while working: file a ticket (include the log excerpt, with personal data
  scrubbed, and hardware and driver info) rather than fixing unrelated things in the same
  branch.
- **Security-sensitive findings** (anything that could expose users, accounts or inventory): do
  not file them publicly; tell the owner directly.

When in doubt: small PRs, one ticket each, measured claims, defaults that ship are the defaults
you tested.
